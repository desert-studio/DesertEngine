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

// THE EMISSIVE TRIANGLE LIST ON THE GPU (EmissiveLights.hpp is the list; Common/EmissiveLights.glslh the GPU
// side; Programs/EmissiveLights/EmissiveLights.shader the passes). Two parts under one root (EmissiveLights.hpp):
//   * STATIC: the view keeps ONE copy of its three buffers across frames - segments, triangles, power tree - plus
//     the hit lookup (which names the rows of both parts), and rebuilds them in the frame's graph only when the
//     static hash moves (an emissive instance moved, its mesh's buffers changed, its material's emission changed,
//     the TLAS order changed);
//   * DYNAMIC: the deformed primitives' segments, expanded EVERY frame from the skin cache's positions of that
//     frame into transient buffers (the Build pass reads the streams, so the graph orders it after the skin
//     pass that wrote them). A frame with no deformed emitter binds the view's kept EMPTY part instead (one
//     zero row per buffer, root power 0, uploaded once): nothing is uploaded or dispatched for it.
// Per part "EmissiveLights: Build" expands the segments into world triangles and leaf powers, one
// "EmissiveLights: Reduce" per tree level sums them. Every consumer (the path tracer now, MegaLights next) reads
// the same EmissiveLightsFrame and picks a part by the two roots.
//
// LIFETIME. A rebuilt static part's previous buffers are PARKED (Assets::FrameRetireQueue), not dropped: the
// frames recorded before the rebuild may still be reading them on the GPU, and the graph pool frees a leased
// buffer as soon as the last reference drops (VulkanRdgPool::BeginFrame).
namespace Desert::Graphic::RayTracing
{
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
            if ( m_BuildPipeline && m_ReducePipeline )
                return BOOLSUCCESS;
            auto* shaders  = Runtime::ResourceRegistry::GetShaderService();
            m_BuildShader  = shaders->GetByName( "EmissiveLights/Build" );
            m_ReduceShader = shaders->GetByName( "EmissiveLights/Reduce" );
            if ( !m_BuildShader || !m_ReduceShader )
                return Common::MakeError( "EmissiveLights: the programs 'EmissiveLights/Build' / "
                                          "'EmissiveLights/Reduce' are not registered" );
            const auto build =
                 ComputePipeline::Create( { .Shader = m_BuildShader, .DebugName = "EmissiveLightsBuild" } );
            if ( !build )
                return Common::MakeError( build.GetError() );
            const auto reduce =
                 ComputePipeline::Create( { .Shader = m_ReduceShader, .DebugName = "EmissiveLightsReduce" } );
            if ( !reduce )
                return Common::MakeError( reduce.GetError() );
            m_BuildPipeline  = build.GetValue();
            m_ReducePipeline = reduce.GetValue();
            return BOOLSUCCESS;
        }

        // GRAPH BUILD: this frame's list in @p graph - the static part kept when @p list's StaticHash is the kept
        // one, rebuilt (uploads, Build, Reduce, extracted for the next frames) otherwise; the dynamic part built
        // in this graph from list.DynamicPositions, which must be registered in @p graph.
        [[nodiscard]] Common::ResultStr<EmissiveLightsFrame> AddPasses( RDG::Builder&               graph,
                                                                        const EmissiveTriangleList& list )
        {
            if ( !m_BuildPipeline || !m_ReducePipeline )
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
                         list.DynamicPositions, kEmissiveBuildDynamicPassName, kEmissiveReduceDynamicPassName );
            }

            // ---- Static: kept while its hash holds. ----
            if ( m_Kept.Hash == list.StaticHash && m_Kept.Segments.Physical && m_Kept.Lookup.Physical &&
                 m_Kept.Triangles.Physical && m_Kept.Power.Physical )
            {
                frame.Static.Segments  = graph.RegisterExternal( m_Kept.Segments, kEmissiveSegmentsName );
                frame.Lookup           = graph.RegisterExternal( m_Kept.Lookup, kEmissiveLookupName );
                frame.Static.Triangles = graph.RegisterExternal( m_Kept.Triangles, kEmissiveTrianglesName );
                frame.Static.Power     = graph.RegisterExternal( m_Kept.Power, kEmissivePowerName );
                return Common::MakeSuccess( frame );
            }

            const uint32_t emptyLookup    = kNoEmissiveSegment;
            const auto     staticSegments = rowsOf( list.Segments );
            const auto lookup = list.Lookup.empty() ? std::as_bytes( std::span<const uint32_t>( &emptyLookup, 1 ) )
                                                    : std::as_bytes( std::span<const uint32_t>( list.Lookup ) );
            frame.Static.Segments =
                 graph.CreateBuffer( RDG::BufferDesc{ staticSegments.size() }, kEmissiveSegmentsName );
            frame.Lookup = graph.CreateBuffer( RDG::BufferDesc{ lookup.size() }, kEmissiveLookupName );
            CreateTriangles( graph, frame.Static, kEmissiveTrianglesName, kEmissivePowerName );
            graph.QueueBufferUpload( frame.Static.Segments, staticSegments );
            graph.QueueBufferUpload( frame.Lookup, lookup );
            AddTree( graph, frame.Static, static_cast<uint32_t>( list.Segments.size() ), {},
                     kEmissiveBuildPassName, kEmissiveReducePassName );

            // The replaced buffers wait out the frames in flight that may still read them.
            if ( m_Kept.Segments.Physical || m_Kept.Lookup.Physical || m_Kept.Triangles.Physical ||
                 m_Kept.Power.Physical )
                m_Retiring.Park( std::move( m_Kept ), frames.GetAbsoluteFrameCount() );
            // Kept for the next frames; the Physical buffers arrive on Execute, so a graph that fails leaves them
            // empty and the next frame rebuilds.
            m_Kept      = {};
            m_Kept.Hash = list.StaticHash;
            graph.Extract( frame.Static.Segments, m_Kept.Segments, RDG::Access::StorageRead );
            graph.Extract( frame.Lookup, m_Kept.Lookup, RDG::Access::StorageRead );
            graph.Extract( frame.Static.Triangles, m_Kept.Triangles, RDG::Access::StorageRead );
            graph.Extract( frame.Static.Power, m_Kept.Power, RDG::Access::StorageRead );
            return Common::MakeSuccess( frame );
        }

    private:
        struct Kept
        {
            RDG::ExternalBuffer Segments;
            RDG::ExternalBuffer Lookup;
            RDG::ExternalBuffer Triangles;
            RDG::ExternalBuffer Power;
            uint64_t            Hash = 0;
        };

        // The empty dynamic part every consumer binds when no deformed emitter is in the list: never written, so
        // made once (zeros, a root power of 0 that no sample picks) and registered in each graph after that.
        struct EmptyPart
        {
            RDG::ExternalBuffer Segments;
            RDG::ExternalBuffer Triangles;
            RDG::ExternalBuffer Power;
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
            return ( threads + 63u ) / 64u; // LocalSize(64, 1, 1) in both passes
        }

        static EmissiveLightsPart DescribePart( const uint32_t triangles )
        {
            EmissiveLightsPart part;
            part.TriangleCount = triangles;
            part.Leaves        = std::bit_ceil( std::max( triangles, 1u ) );
            part.Levels        = static_cast<uint32_t>( std::bit_width( part.Leaves ) );
            return part;
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

        // @p part's Build (reading @p positions besides its segments: the streams its rows address) and one
        // Reduce per tree level above 0.
        void AddTree( RDG::Builder& graph, const EmissiveLightsPart& part, const uint32_t segmentCount,
                      std::span<const RDG::BufferRef> positions, const std::string_view buildName,
                      const std::string_view reduceName )
        {
            const glm::uvec4 counts( part.TriangleCount, part.Leaves, segmentCount, 0u );
            graph.AddPass(
                 buildName, RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_BuildLayout.Get( m_BuildShader ),
                                    Renderer::GetPipelineRouteFill( *m_BuildPipeline ) )
                          .Storage( "EmissiveSegmentsUB", part.Segments, RDG::Access::StorageRead )
                          .Storage( "EmissiveTrianglesUB", part.Triangles, RDG::Access::StorageWrite )
                          .Storage( "EmissivePowerUB", part.Power, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                     // Read by device address (EmissiveSegment::VertexAddress), declared for the barrier.
                     for ( const RDG::BufferRef stream : positions )
                         pass.Read( stream, RDG::Access::StorageRead );
                 },
                 [this, counts, buildName]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS_DYNAMIC( buildName.data() );
                     // Named by the zone alone, which a build without the profiler compiles out.
                     static_cast<void>( buildName );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &counts, static_cast<uint32_t>( sizeof( counts ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_BuildPipeline,
                                                                     Groups( counts.y ), 1, 1 );
                 } );

            // Each level reads the one below it, so each is its own pass (the graph orders the buffer's writes).
            for ( uint32_t level = 1; level < part.Levels; ++level )
            {
                const glm::uvec4 reduce( part.TriangleCount, part.Leaves, 0u, level );
                graph.AddPass(
                     reduceName, RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_ReduceLayout.Get( m_ReduceShader ),
                                        Renderer::GetPipelineRouteFill( *m_ReducePipeline ) )
                              .Storage( "EmissivePowerUB", part.Power, RDG::Access::StorageWrite )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                     },
                     [this, reduce, reduceName]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS_DYNAMIC( reduceName.data() );
                         // Named by the zone alone, which a build without the profiler compiles out.
                         static_cast<void>( reduceName );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &reduce, static_cast<uint32_t>( sizeof( reduce ) ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *m_ReducePipeline,
                                                                         Groups( reduce.y >> reduce.w ), 1, 1 );
                     } );
            }
        }

        std::shared_ptr<Shader>          m_BuildShader;
        std::shared_ptr<Shader>          m_ReduceShader;
        std::shared_ptr<ComputePipeline> m_BuildPipeline;
        std::shared_ptr<ComputePipeline> m_ReducePipeline;
        mutable ShaderBindingLayoutCache m_BuildLayout;
        mutable ShaderBindingLayoutCache m_ReduceLayout;
        Kept                             m_Kept;
        EmptyPart                        m_EmptyDynamic;
        // Static parts a rebuild replaced, kept until no frame in flight can still read them.
        Assets::FrameRetireQueue<Kept> m_Retiring;
    };
} // namespace Desert::Graphic::RayTracing
