#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>
#include <Engine/Core/FrameManager.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/EmissiveLights.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// THE EMISSIVE TRIANGLE LIST ON THE GPU (EmissiveLights.hpp is the list; Common/EmissiveLights.glslh the GPU
// side; Programs/EmissiveLights/EmissiveLights.shader the passes). Two parts under one root (EmissiveLights.hpp):
//   * STATIC: the view keeps ONE copy of its three buffers across frames - segments, triangles, power tree - and
//     re-expands them whole in the frame's graph only when the structure hash moves (an emitter added, removed or
//     re-materialled, its mesh's buffers changed, its emissive map streamed in); a moved emitter's rows are
//     updated IN PLACE ("EmissiveLights: Move" writes their transforms, Build and Reduce rewrite their leaves and
//     the path to the root only). The hit lookup (which names the rows of both parts) is kept beside them and
//     re-uploaded alone when the TLAS order changed;
//   * DYNAMIC: the deformed primitives' segments, expanded EVERY frame from the skin cache's positions of that
//     frame into transient buffers (the Build pass reads the streams, so the graph orders it after the skin
//     pass that wrote them). A frame with no deformed emitter binds the view's kept EMPTY part instead (one
//     zero row per buffer, root power 0, uploaded once): nothing is uploaded or dispatched for it.
// Per part "EmissiveLights: Build" expands the segments into world triangles and leaf powers (the emissive map's
// footprint average included), one "EmissiveLights: Reduce" per tree level sums them - over the runs of nodes the
// CPU names (EmissiveDirtyRuns: the whole tree, or a moved row's leaves and their parents). Every consumer (the path tracer now, MegaLights next) reads
// the same EmissiveLightsFrame and picks a part by the two roots.
//
// LIFETIME. A rebuilt static part's previous buffers are PARKED (Assets::FrameRetireQueue), not dropped: the
// frames recorded before the rebuild may still be reading them on the GPU, and the graph pool frees a leased
// buffer as soon as the last reference drops (VulkanRdgPool::BeginFrame).
namespace Desert::Graphic::RayTracing
{
    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct EmissiveLightsBuildShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveSegmentsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveTrianglesUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissivePowerUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveRunsUB );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct EmissiveLightsReduceShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissivePowerUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveRunsUB );
    };

    // The slot names this pass binds, spelled once (RDGShaderParameters.hpp).
    struct EmissiveLightsMoveShaderParameters
    {
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveSegmentsUB );
        DESERT_SHADER_PARAMETER( StorageBuffer, EmissiveMovesUB );
    };

    // std430, 64 B: EmissiveLights.shader "Move"'s EmissiveMove - a kept static row whose transform moved.
    struct EmissiveMoveGpu
    {
        glm::uvec4               Row{ 0u }; // x: the static segment row
        std::array<glm::vec4, 3> ObjectToWorld{};
    };
    static_assert( sizeof( EmissiveMoveGpu ) == 64 );

    inline constexpr std::string_view kEmissiveSegmentsName          = "EmissiveLights.Segments";
    inline constexpr std::string_view kEmissiveLookupName            = "EmissiveLights.Lookup";
    inline constexpr std::string_view kEmissiveTrianglesName         = "EmissiveLights.Triangles";
    inline constexpr std::string_view kEmissivePowerName             = "EmissiveLights.Power";
    inline constexpr std::string_view kEmissiveDynamicSegmentsName   = "EmissiveLights.DynamicSegments";
    inline constexpr std::string_view kEmissiveDynamicTrianglesName  = "EmissiveLights.DynamicTriangles";
    inline constexpr std::string_view kEmissiveDynamicPowerName      = "EmissiveLights.DynamicPower";
    inline constexpr std::string_view kEmissiveBuildPassName         = "EmissiveLights: Build";
    inline constexpr std::string_view kEmissiveReducePassName        = "EmissiveLights: Reduce";
    inline constexpr std::string_view kEmissiveBuildDynamicPassName  = "EmissiveLights: Build Dynamic";
    inline constexpr std::string_view kEmissiveReduceDynamicPassName = "EmissiveLights: Reduce Dynamic";
    inline constexpr std::string_view kEmissiveRunsName              = "EmissiveLights.Runs";
    inline constexpr std::string_view kEmissiveDynamicRunsName       = "EmissiveLights.DynamicRuns";
    inline constexpr std::string_view kEmissiveMovedRunsName         = "EmissiveLights.MovedRuns";
    inline constexpr std::string_view kEmissiveMovesName             = "EmissiveLights.Moves";
    inline constexpr std::string_view kEmissiveMovePassName          = "EmissiveLights: Move";
    inline constexpr std::string_view kEmissiveBuildMovedPassName    = "EmissiveLights: Build Moved";
    inline constexpr std::string_view kEmissiveReduceMovedPassName   = "EmissiveLights: Reduce Moved";

    // One part of the list as the graph sees it.
    struct EmissiveLightsPart
    {
        RDG::BufferRef Segments;
        RDG::BufferRef Triangles;
        RDG::BufferRef Power;
        uint32_t       TriangleCount = 0;
        uint32_t       Leaves        = 1; // the tree's level 0, a power of two
        uint32_t       Levels        = 1; // Leaves, Leaves / 2, ..., 1
    };

    // This frame's list as the graph sees it (refs valid only for that graph). A consumer binds the seven buffers
    // under Common/EmissiveLights.glslh's block names and passes StaticParams / DynamicParams (its uvec4 pair);
    // a part with TriangleCount 0 holds one empty row per buffer and a root power of 0.
    struct EmissiveLightsFrame
    {
        bool               Valid = false;
        EmissiveLightsPart Static;
        EmissiveLightsPart Dynamic;
        RDG::BufferRef     Lookup;
        uint32_t           InstanceCount = 0; // the TLAS instances the lookup was built for
        uint64_t           Hash          = 0; // EmissiveTriangleList::Hash

        // Common/EmissiveLights.glslh's EmissiveLightsParams: x = triangles, y = leaves, z = levels, w = TLAS
        // instances of the lookup (static) / 0 (dynamic).
        [[nodiscard]] glm::uvec4 StaticParams() const
        {
            return { Static.TriangleCount, Static.Leaves, Static.Levels, InstanceCount };
        }
        [[nodiscard]] glm::uvec4 DynamicParams() const
        {
            return { Dynamic.TriangleCount, Dynamic.Leaves, Dynamic.Levels, 0u };
        }
    };

    class EmissiveLightsRenderer
    {
    public:
        [[nodiscard]] Common::BoolResultStr Initialize()
        {
            if ( m_BuildPipeline && m_ReducePipeline && m_MovePipeline )
                return BOOLSUCCESS;
            auto* shaders  = Runtime::ResourceRegistry::GetShaderService();
            m_BuildShader  = shaders->GetByName( "EmissiveLights/Build" );
            m_ReduceShader = shaders->GetByName( "EmissiveLights/Reduce" );
            m_MoveShader   = shaders->GetByName( "EmissiveLights/Move" );
            if ( !m_BuildShader || !m_ReduceShader || !m_MoveShader )
                return Common::MakeError( "EmissiveLights: the programs 'EmissiveLights/Build' / "
                                          "'EmissiveLights/Reduce' / 'EmissiveLights/Move' are not registered" );
            const auto build =
                 ComputePipeline::Create( { .Shader = m_BuildShader, .DebugName = "EmissiveLightsBuild" } );
            if ( !build )
                return Common::MakeError( build.GetError() );
            const auto reduce =
                 ComputePipeline::Create( { .Shader = m_ReduceShader, .DebugName = "EmissiveLightsReduce" } );
            if ( !reduce )
                return Common::MakeError( reduce.GetError() );
            const auto move = ComputePipeline::Create( { .Shader = m_MoveShader, .DebugName = "EmissiveLightsMove" } );
            if ( !move )
                return Common::MakeError( move.GetError() );
            m_BuildPipeline  = build.GetValue();
            m_ReducePipeline = reduce.GetValue();
            m_MovePipeline   = move.GetValue();
            return BOOLSUCCESS;
        }

        // GRAPH BUILD: this frame's list in @p graph. The static part, against what the view keeps on the GPU:
        //   * list.StructureHash moved (an emitter added, removed or re-materialled), or nothing kept: uploaded and
        //     expanded whole (Build over every leaf, Reduce over every node), extracted for the next frames;
        //   * else, the rows whose transform moved, IN PLACE in the kept buffers: "EmissiveLights: Move" writes
        //     their transforms, Build re-expands their leaves only and Reduce sums their path to the root
        //     (EmissiveDirtyRuns) - a moving emitter costs its own triangles, a still scene nothing;
        //   * else registered as kept.
        // The lookup is re-uploaded alone when list.LookupHash moved. The dynamic part is built whole in this graph
        // from list.DynamicPositions, which must be registered in @p graph.
        [[nodiscard]] Common::ResultStr<EmissiveLightsFrame> AddPasses( RDG::Builder&               graph,
                                                                        const EmissiveTriangleList& list )
        {
            if ( !m_BuildPipeline || !m_ReducePipeline || !m_MovePipeline )
                return Common::MakeError<EmissiveLightsFrame>(
                     "EmissiveLights: the pipelines were not made (Initialize)" );
            const auto& frames = ::Desert::Engine::FrameManager::GetInstance();
            m_Retiring.Collect( frames.GetAbsoluteFrameCount(), frames.GetMaxFramesInFlight() );

            EmissiveLightsFrame frame;
            frame.Valid         = true;
            frame.InstanceCount = list.InstanceCount;
            frame.Hash          = list.Hash;
            frame.Static        = DescribePart( list.TriangleCount );
            frame.Dynamic       = DescribePart( list.DynamicTriangleCount );

            // A graph buffer cannot be empty: an empty part keeps one zero row of each (never read: its root is
            // 0).
            const EmissiveSegmentGpu emptySegment{};
            const auto               rowsOf = [&emptySegment]( const std::vector<EmissiveSegmentGpu>& rows )
            {
                return rows.empty() ? std::as_bytes( std::span<const EmissiveSegmentGpu>( &emptySegment, 1 ) )
                                    : std::as_bytes( std::span<const EmissiveSegmentGpu>( rows ) );
            };

            // ---- Dynamic: every frame, from this frame's skinned positions; none: the kept empty part. ----
            if ( list.DynamicTriangleCount == 0 )
                RegisterEmptyDynamic( graph, frame.Dynamic );
            else
            {
                const auto dynamicSegments = rowsOf( list.DynamicSegments );
                frame.Dynamic.Segments =
                     graph.CreateBuffer( RDG::BufferDesc{ dynamicSegments.size() }, kEmissiveDynamicSegmentsName );
                CreateTriangles( graph, frame.Dynamic, kEmissiveDynamicTrianglesName, kEmissiveDynamicPowerName );
                graph.QueueBufferUpload( frame.Dynamic.Segments, dynamicSegments );
                AddTree( graph, frame.Dynamic, static_cast<uint32_t>( list.DynamicSegments.size() ),
                         list.DynamicPositions, WholeTree( frame.Dynamic ),
                         { kEmissiveDynamicRunsName, kEmissiveBuildDynamicPassName, kEmissiveReduceDynamicPassName } );
            }

            // ---- Lookup: kept while its hash holds. ----
            if ( m_Kept.Lookup.Physical && m_Kept.LookupHash == list.LookupHash )
                frame.Lookup = graph.RegisterExternal( m_Kept.Lookup, kEmissiveLookupName );
            else
            {
                const uint32_t emptyLookup = kNoEmissiveSegment;
                const auto lookup = list.Lookup.empty() ? std::as_bytes( std::span<const uint32_t>( &emptyLookup, 1 ) )
                                                        : std::as_bytes( std::span<const uint32_t>( list.Lookup ) );
                frame.Lookup      = graph.CreateBuffer( RDG::BufferDesc{ lookup.size() }, kEmissiveLookupName );
                graph.QueueBufferUpload( frame.Lookup, lookup );
                if ( m_Kept.Lookup.Physical )
                {
                    Kept replaced;
                    replaced.Lookup = std::move( m_Kept.Lookup );
                    m_Retiring.Park( std::move( replaced ), frames.GetAbsoluteFrameCount() );
                }
                // The Physical buffer arrives on Execute: a graph that fails leaves it empty and the next frame
                // uploads it again.
                m_Kept.Lookup     = {};
                m_Kept.LookupHash = list.LookupHash;
                graph.Extract( frame.Lookup, m_Kept.Lookup, RDG::Access::StorageRead );
            }

            // ---- Static, structure unchanged: kept, its moved rows updated in place. ----
            if ( m_Kept.StructureHash == list.StructureHash && m_Kept.Segments.Physical &&
                 m_Kept.Triangles.Physical && m_Kept.Power.Physical &&
                 m_KeptTransforms.size() == list.Segments.size() )
            {
                frame.Static.Segments  = graph.RegisterExternal( m_Kept.Segments, kEmissiveSegmentsName );
                frame.Static.Triangles = graph.RegisterExternal( m_Kept.Triangles, kEmissiveTrianglesName );
                frame.Static.Power     = graph.RegisterExternal( m_Kept.Power, kEmissivePowerName );
                std::vector<EmissiveMoveGpu> moves;
                std::vector<glm::uvec2>      leafRuns;
                for ( std::size_t row = 0; row < list.Segments.size(); ++row )
                {
                    const EmissiveSegmentGpu& segment = list.Segments[row];
                    if ( segment.ObjectToWorld == m_KeptTransforms[row] )
                        continue;
                    moves.push_back(
                         { .Row = glm::uvec4( static_cast<uint32_t>( row ), 0u, 0u, 0u ),
                           .ObjectToWorld = segment.ObjectToWorld } );
                    leafRuns.emplace_back( segment.FirstTriangle, segment.FirstTriangle + segment.TriangleCount );
                    m_KeptTransforms[row] = segment.ObjectToWorld;
                }
                if ( moves.empty() )
                    return Common::MakeSuccess( frame );
                DESERT_PROFILE_SCOPE( "EmissiveLights: Move" );
                AddMove( graph, frame.Static.Segments, moves );
                AddTree( graph, frame.Static, static_cast<uint32_t>( list.Segments.size() ), {},
                         EmissiveDirtyRuns( std::move( leafRuns ), frame.Static.Levels ),
                         { kEmissiveMovedRunsName, kEmissiveBuildMovedPassName, kEmissiveReduceMovedPassName } );
                ++m_StaticMoves;
                return Common::MakeSuccess( frame );
            }

            // ---- Static, structure changed: rebuilt whole. ----
            DESERT_PROFILE_SCOPE( "EmissiveLights: Rebuild" );
            const auto staticSegments = rowsOf( list.Segments );
            frame.Static.Segments =
                 graph.CreateBuffer( RDG::BufferDesc{ staticSegments.size() }, kEmissiveSegmentsName );
            CreateTriangles( graph, frame.Static, kEmissiveTrianglesName, kEmissivePowerName );
            graph.QueueBufferUpload( frame.Static.Segments, staticSegments );
            AddTree( graph, frame.Static, static_cast<uint32_t>( list.Segments.size() ), {},
                     WholeTree( frame.Static ), { kEmissiveRunsName, kEmissiveBuildPassName, kEmissiveReducePassName } );

            // The replaced buffers wait out the frames in flight that may still read them.
            if ( m_Kept.Segments.Physical || m_Kept.Triangles.Physical || m_Kept.Power.Physical )
            {
                Kept replaced;
                replaced.Segments  = std::move( m_Kept.Segments );
                replaced.Triangles = std::move( m_Kept.Triangles );
                replaced.Power     = std::move( m_Kept.Power );
                m_Retiring.Park( std::move( replaced ), frames.GetAbsoluteFrameCount() );
            }
            // Kept for the next frames; the Physical buffers arrive on Execute, so a graph that fails leaves them
            // empty and the next frame rebuilds.
            m_Kept.Segments      = {};
            m_Kept.Triangles     = {};
            m_Kept.Power         = {};
            m_Kept.StructureHash = list.StructureHash;
            m_KeptTransforms.clear();
            m_KeptTransforms.reserve( list.Segments.size() );
            for ( const EmissiveSegmentGpu& segment : list.Segments )
                m_KeptTransforms.push_back( segment.ObjectToWorld );
            graph.Extract( frame.Static.Segments, m_Kept.Segments, RDG::Access::StorageRead );
            graph.Extract( frame.Static.Triangles, m_Kept.Triangles, RDG::Access::StorageRead );
            graph.Extract( frame.Static.Power, m_Kept.Power, RDG::Access::StorageRead );
            ++m_StaticRebuilds;
            return Common::MakeSuccess( frame );
        }

        // How often the static part was expanded whole / updated for moved rows (a still scene: 1 / 0; a moving
        // emitter: 1 / one per frame it moves).
        [[nodiscard]] uint64_t GetStaticRebuildCount() const
        {
            return m_StaticRebuilds;
        }
        [[nodiscard]] uint64_t GetStaticMoveCount() const
        {
            return m_StaticMoves;
        }

    private:
        struct Kept
        {
            RDG::ExternalBuffer Segments;
            RDG::ExternalBuffer Lookup;
            RDG::ExternalBuffer Triangles;
            RDG::ExternalBuffer Power;
            uint64_t            StructureHash = 0;
            uint64_t            LookupHash    = 0;
        };

        // The empty dynamic part every consumer binds when no deformed emitter is in the list: never written, so
        // made once (zeros, a root power of 0 that no sample picks) and registered in each graph after that.
        struct EmptyPart
        {
            RDG::ExternalBuffer Segments;
            RDG::ExternalBuffer Triangles;
            RDG::ExternalBuffer Power;
        };

        // The names one tree's passes and its runs buffer go by.
        struct TreeNames
        {
            std::string_view Runs;
            std::string_view Build;
            std::string_view Reduce;
        };

        void RegisterEmptyDynamic( RDG::Builder& graph, EmissiveLightsPart& part )
        {
            if ( m_EmptyDynamic.Segments.Physical && m_EmptyDynamic.Triangles.Physical &&
                 m_EmptyDynamic.Power.Physical )
            {
                part.Segments  = graph.RegisterExternal( m_EmptyDynamic.Segments, kEmissiveDynamicSegmentsName );
                part.Triangles = graph.RegisterExternal( m_EmptyDynamic.Triangles, kEmissiveDynamicTrianglesName );
                part.Power     = graph.RegisterExternal( m_EmptyDynamic.Power, kEmissiveDynamicPowerName );
                return;
            }
            const EmissiveSegmentGpu                            segment{};
            const std::array<std::byte, kEmissiveTriangleBytes> triangle{};
            const float                                         rootPower = 0.0f;
            part.Segments =
                 graph.CreateBuffer( RDG::BufferDesc{ sizeof( EmissiveSegmentGpu ) }, kEmissiveDynamicSegmentsName );
            CreateTriangles( graph, part, kEmissiveDynamicTrianglesName, kEmissiveDynamicPowerName );
            graph.QueueBufferUpload( part.Segments,
                                     std::as_bytes( std::span<const EmissiveSegmentGpu>( &segment, 1 ) ) );
            graph.QueueBufferUpload( part.Triangles, std::span<const std::byte>( triangle ) );
            graph.QueueBufferUpload( part.Power, std::as_bytes( std::span<const float>( &rootPower, 1 ) ) );
            // The Physical buffers arrive on Execute: a graph that fails leaves them empty and the next frame
            // makes them again.
            graph.Extract( part.Segments, m_EmptyDynamic.Segments, RDG::Access::StorageRead );
            graph.Extract( part.Triangles, m_EmptyDynamic.Triangles, RDG::Access::StorageRead );
            graph.Extract( part.Power, m_EmptyDynamic.Power, RDG::Access::StorageRead );
        }

        static uint32_t Groups( const uint32_t threads )
        {
            return ( threads + 63u ) / 64u; // LocalSize(64, 1, 1) in every pass
        }

        static EmissiveLightsPart DescribePart( const uint32_t triangles )
        {
            EmissiveLightsPart part;
            part.TriangleCount = triangles;
            part.Leaves        = std::bit_ceil( std::max( triangles, 1u ) );
            part.Levels        = static_cast<uint32_t>( std::bit_width( part.Leaves ) );
            return part;
        }

        // Every node of @p part's tree: the one run [0, leaves) and its parents.
        static EmissiveTreeRuns WholeTree( const EmissiveLightsPart& part )
        {
            return EmissiveDirtyRuns( { glm::uvec2( 0u, part.Leaves ) }, part.Levels );
        }

        static void CreateTriangles( RDG::Builder& graph, EmissiveLightsPart& part,
                                     const std::string_view triangles, const std::string_view power )
        {
            part.Triangles =
                 graph.CreateBuffer( RDG::BufferDesc{ static_cast<uint64_t>( std::max( part.TriangleCount, 1u ) ) *
                                                      kEmissiveTriangleBytes },
                                     triangles );
            part.Power =
                 graph.CreateBuffer( RDG::BufferDesc{ ( 2ull * part.Leaves - 1ull ) * sizeof( float ) }, power );
        }

        // "EmissiveLights: Move": the moved rows' transforms written into @p segments.
        void AddMove( RDG::Builder& graph, const RDG::BufferRef segments, std::span<const EmissiveMoveGpu> moves )
        {
            const RDG::BufferRef movesBuffer =
                 graph.CreateBuffer( RDG::BufferDesc{ moves.size_bytes() }, kEmissiveMovesName );
            graph.QueueBufferUpload( movesBuffer, std::as_bytes( moves ) );
            const glm::uvec4 counts( static_cast<uint32_t>( moves.size() ), 0u, 0u, 0u );
            graph.AddPass(
                 kEmissiveMovePassName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_MoveLayout.Get( m_MoveShader ), Renderer::GetPipelineRouteFill( *m_MovePipeline ) )
                          .Storage( EmissiveLightsMoveShaderParameters::EmissiveSegmentsUB, segments,
                                    RDG::Access::StorageWrite )
                          .Storage( EmissiveLightsMoveShaderParameters::EmissiveMovesUB, movesBuffer,
                                    RDG::Access::StorageRead )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                 },
                 [this, counts]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS_DYNAMIC( kEmissiveMovePassName.data() );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &counts, static_cast<uint32_t>( sizeof( counts ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_MovePipeline, Groups( counts.x ), 1,
                                                                     1 );
                 } );
        }

        // @p part's Build over @p runs' level 0 (reading @p positions besides its segments: the streams its rows
        // address) and one Reduce per tree level above 0 over that level's runs.
        void AddTree( RDG::Builder& graph, const EmissiveLightsPart& part, const uint32_t segmentCount,
                      std::span<const RDG::BufferRef> positions, const EmissiveTreeRuns& runs, const TreeNames names )
        {
            const RDG::BufferRef runsBuffer = graph.CreateBuffer(
                 RDG::BufferDesc{ runs.Runs.size() * sizeof( EmissiveNodeRunGpu ) }, names.Runs );
            graph.QueueBufferUpload( runsBuffer, std::as_bytes( std::span<const EmissiveNodeRunGpu>( runs.Runs ) ) );

            const EmissiveLevelRuns& leaves = runs.Levels[0];
            const std::array<glm::uvec4, 2> build{ glm::uvec4( part.TriangleCount, part.Leaves, segmentCount, 0u ),
                                                   glm::uvec4( leaves.FirstRun, leaves.RunCount, leaves.Threads, 0u ) };
            const std::string_view buildName = names.Build;
            graph.AddPass(
                 buildName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_BuildLayout.Get( m_BuildShader ),
                                    Renderer::GetPipelineRouteFill( *m_BuildPipeline ) )
                          .Storage( EmissiveLightsBuildShaderParameters::EmissiveSegmentsUB, part.Segments,
                                    RDG::Access::StorageRead )
                          .Storage( EmissiveLightsBuildShaderParameters::EmissiveTrianglesUB, part.Triangles,
                                    RDG::Access::StorageWrite )
                          .Storage( EmissiveLightsBuildShaderParameters::EmissivePowerUB, part.Power,
                                    RDG::Access::StorageWrite )
                          .Storage( EmissiveLightsBuildShaderParameters::EmissiveRunsUB, runsBuffer,
                                    RDG::Access::StorageRead )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( build ) ) );
                     // Read by device address (EmissiveSegment::VertexAddress), declared for the barrier.
                     for ( const RDG::BufferRef stream : positions )
                         pass.Read( stream, RDG::Access::StorageRead );
                 },
                 [this, build, buildName]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS_DYNAMIC( buildName.data() );
                     // Named by the zone alone, which a build without the profiler compiles out.
                     static_cast<void>( buildName );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( build.data(), static_cast<uint32_t>( sizeof( build ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_BuildPipeline,
                                                                     Groups( build[1].z ), 1, 1 );
                 } );

            // Each level reads the one below it, so each is its own pass (the graph orders the buffer's writes).
            const std::string_view reduceName = names.Reduce;
            for ( uint32_t level = 1; level < part.Levels; ++level )
            {
                const EmissiveLevelRuns&        nodes = runs.Levels[level];
                const std::array<glm::uvec4, 2> reduce{
                     glm::uvec4( part.TriangleCount, part.Leaves, 0u, level ),
                     glm::uvec4( nodes.FirstRun, nodes.RunCount, nodes.Threads, 0u ) };
                graph.AddPass(
                     reduceName, RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_ReduceLayout.Get( m_ReduceShader ),
                                        Renderer::GetPipelineRouteFill( *m_ReducePipeline ) )
                              .Storage( EmissiveLightsReduceShaderParameters::EmissivePowerUB, part.Power,
                                        RDG::Access::StorageWrite )
                              .Storage( EmissiveLightsReduceShaderParameters::EmissiveRunsUB, runsBuffer,
                                        RDG::Access::StorageRead )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( reduce ) ) );
                     },
                     [this, reduce, reduceName]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS_DYNAMIC( reduceName.data() );
                         // Named by the zone alone, which a build without the profiler compiles out.
                         static_cast<void>( reduceName );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( reduce.data(), static_cast<uint32_t>( sizeof( reduce ) ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *m_ReducePipeline,
                                                                         Groups( reduce[1].z ), 1, 1 );
                     } );
            }
        }

        std::shared_ptr<Shader>          m_BuildShader;
        std::shared_ptr<Shader>          m_ReduceShader;
        std::shared_ptr<Shader>          m_MoveShader;
        std::shared_ptr<ComputePipeline> m_BuildPipeline;
        std::shared_ptr<ComputePipeline> m_ReducePipeline;
        std::shared_ptr<ComputePipeline> m_MovePipeline;
        mutable ShaderBindingLayoutCache m_BuildLayout;
        mutable ShaderBindingLayoutCache m_ReduceLayout;
        mutable ShaderBindingLayoutCache m_MoveLayout;
        Kept                             m_Kept;
        // The transform of each kept static row as the GPU holds it (what a move is measured against).
        std::vector<std::array<glm::vec4, 3>> m_KeptTransforms;
        EmptyPart                             m_EmptyDynamic;
        // Static parts (and lookups) a rebuild replaced, kept until no frame in flight can still read them.
        Assets::FrameRetireQueue<Kept> m_Retiring;
        uint64_t                       m_StaticRebuilds = 0;
        uint64_t                       m_StaticMoves    = 0;
    };
} // namespace Desert::Graphic::RayTracing
