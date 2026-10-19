#pragma once

#include <Common/Core/Core.hpp>
#include <Common/Core/Profiler.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/Systems/RayTracing/EmissiveLights.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

// THE EMISSIVE TRIANGLE LIST ON THE GPU (EmissiveLights.hpp is the list; Common/EmissiveLights.glslh the GPU
// side; Programs/EmissiveLights/EmissiveLights.shader the passes). The view keeps ONE copy of the four buffers
// across frames - segments, hit lookup, triangles, power tree - and rebuilds them in the frame's graph only when
// the list's hash moves (an emissive instance moved, its mesh's buffers changed, its material's emission changed,
// the TLAS order changed): "EmissiveLights: Build" expands the segments into world triangles and leaf powers,
// one "EmissiveLights: Reduce" per tree level sums them. Every consumer (the path tracer now, MegaLights next)
// reads the same EmissiveLightsFrame.
namespace Desert::Graphic::RayTracing
{
    inline constexpr std::string_view kEmissiveSegmentsName  = "EmissiveLights.Segments";
    inline constexpr std::string_view kEmissiveLookupName    = "EmissiveLights.Lookup";
    inline constexpr std::string_view kEmissiveTrianglesName = "EmissiveLights.Triangles";
    inline constexpr std::string_view kEmissivePowerName     = "EmissiveLights.Power";

    // This frame's list as the graph sees it (refs valid only for that graph). A consumer binds the four buffers
    // under Common/EmissiveLights.glslh's block names and passes TriangleCount / Leaves / Levels / InstanceCount;
    // TriangleCount 0: no emissive triangle (the buffers hold one empty row each, the root power is 0).
    struct EmissiveLightsFrame
    {
        bool           Valid = false;
        RDG::BufferRef Segments;
        RDG::BufferRef Lookup;
        RDG::BufferRef Triangles;
        RDG::BufferRef Power;
        uint32_t       TriangleCount           = 0;
        uint32_t       Leaves                  = 1; // the tree's level 0, a power of two
        uint32_t       Levels                  = 1; // Leaves, Leaves / 2, ..., 1
        uint32_t       InstanceCount           = 0; // the TLAS instances the lookup was built for
        uint32_t       SkippedDeformedSegments = 0;
        uint64_t       Hash                    = 0; // EmissiveTriangleList::Hash
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

        // GRAPH BUILD: this frame's list in @p graph - the kept buffers when @p list hashes as the kept one, a
        // rebuild (uploads, Build, Reduce, extracted for the next frames) otherwise.
        [[nodiscard]] Common::ResultStr<EmissiveLightsFrame> AddPasses( RDG::Builder&               graph,
                                                                        const EmissiveTriangleList& list )
        {
            if ( !m_BuildPipeline || !m_ReducePipeline )
                return Common::MakeError<EmissiveLightsFrame>(
                     "EmissiveLights: the pipelines were not made (Initialize)" );

            EmissiveLightsFrame frame;
            frame.Valid                   = true;
            frame.TriangleCount           = list.TriangleCount;
            frame.Leaves                  = std::bit_ceil( std::max( list.TriangleCount, 1u ) );
            frame.Levels                  = static_cast<uint32_t>( std::bit_width( frame.Leaves ) );
            frame.InstanceCount           = list.InstanceCount;
            frame.SkippedDeformedSegments = list.SkippedDeformedSegments;
            frame.Hash                    = list.Hash;

            if ( m_Kept.Hash == list.Hash && m_Kept.Segments.Physical && m_Kept.Lookup.Physical &&
                 m_Kept.Triangles.Physical && m_Kept.Power.Physical )
            {
                frame.Segments  = graph.RegisterExternal( m_Kept.Segments, kEmissiveSegmentsName );
                frame.Lookup    = graph.RegisterExternal( m_Kept.Lookup, kEmissiveLookupName );
                frame.Triangles = graph.RegisterExternal( m_Kept.Triangles, kEmissiveTrianglesName );
                frame.Power     = graph.RegisterExternal( m_Kept.Power, kEmissivePowerName );
                return Common::MakeSuccess( frame );
            }

            // A graph buffer cannot be empty: an empty list keeps one zero row of each (never read: the root is
            // 0).
            const EmissiveSegmentGpu emptySegment{};
            const uint32_t           emptyLookup = kNoEmissiveSegment;
            const auto               segments    = list.Segments.empty()
                                                        ? std::as_bytes( std::span<const EmissiveSegmentGpu>( &emptySegment, 1 ) )
                                                        : std::as_bytes( std::span<const EmissiveSegmentGpu>( list.Segments ) );
            const auto lookup = list.Lookup.empty() ? std::as_bytes( std::span<const uint32_t>( &emptyLookup, 1 ) )
                                                    : std::as_bytes( std::span<const uint32_t>( list.Lookup ) );
            frame.Segments    = graph.CreateBuffer( RDG::BufferDesc{ segments.size() }, kEmissiveSegmentsName );
            frame.Lookup      = graph.CreateBuffer( RDG::BufferDesc{ lookup.size() }, kEmissiveLookupName );
            frame.Triangles =
                 graph.CreateBuffer( RDG::BufferDesc{ static_cast<uint64_t>( std::max( list.TriangleCount, 1u ) ) *
                                                      kEmissiveTriangleBytes },
                                     kEmissiveTrianglesName );
            frame.Power = graph.CreateBuffer( RDG::BufferDesc{ ( 2ull * frame.Leaves - 1ull ) * sizeof( float ) },
                                              kEmissivePowerName );
            graph.QueueBufferUpload( frame.Segments, segments );
            graph.QueueBufferUpload( frame.Lookup, lookup );

            const glm::uvec4 counts( list.TriangleCount, frame.Leaves,
                                     static_cast<uint32_t>( list.Segments.size() ), 0u );
            graph.AddPass(
                 "EmissiveLights: Build", RDG::PassFlags::Compute,
                 [&]( RDG::PassBuilder& pass )
                 {
                     pass.Bindings( m_BuildLayout.Get( m_BuildShader ),
                                    Renderer::GetPipelineRouteFill( *m_BuildPipeline ) )
                          .Storage( "EmissiveSegmentsUB", frame.Segments, RDG::Access::StorageRead )
                          .Storage( "EmissiveTrianglesUB", frame.Triangles, RDG::Access::StorageWrite )
                          .Storage( "EmissivePowerUB", frame.Power, RDG::Access::StorageWrite )
                          .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                 },
                 [this, counts]( RDG::PassContext& context ) -> Common::BoolResultStr
                 {
                     DESERT_PROFILE_PASS( "EmissiveLights: Build" );
                     RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                     bindings.PushConstants( &counts, static_cast<uint32_t>( sizeof( counts ) ) );
                     return Renderer::GetInstance().DispatchCompute( bindings, *m_BuildPipeline,
                                                                     Groups( counts.y ), 1, 1 );
                 } );
            // Each level reads the one below it, so each is its own pass (the graph orders the buffer's writes).
            for ( uint32_t level = 1; level < frame.Levels; ++level )
            {
                const glm::uvec4 reduce( list.TriangleCount, frame.Leaves, 0u, level );
                graph.AddPass(
                     "EmissiveLights: Reduce", RDG::PassFlags::Compute,
                     [&]( RDG::PassBuilder& pass )
                     {
                         pass.Bindings( m_ReduceLayout.Get( m_ReduceShader ),
                                        Renderer::GetPipelineRouteFill( *m_ReducePipeline ) )
                              .Storage( "EmissivePowerUB", frame.Power, RDG::Access::StorageWrite )
                              .PushConstantBytes( static_cast<uint32_t>( sizeof( glm::uvec4 ) ) );
                     },
                     [this, reduce]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "EmissiveLights: Reduce" );
                         RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         bindings.PushConstants( &reduce, static_cast<uint32_t>( sizeof( reduce ) ) );
                         return Renderer::GetInstance().DispatchCompute( bindings, *m_ReducePipeline,
                                                                         Groups( reduce.y >> reduce.w ), 1, 1 );
                     } );
            }

            // Kept for the next frames; the Physical buffers arrive on Execute, so a graph that fails leaves them
            // empty and the next frame rebuilds.
            m_Kept      = {};
            m_Kept.Hash = list.Hash;
            graph.Extract( frame.Segments, m_Kept.Segments, RDG::Access::StorageRead );
            graph.Extract( frame.Lookup, m_Kept.Lookup, RDG::Access::StorageRead );
            graph.Extract( frame.Triangles, m_Kept.Triangles, RDG::Access::StorageRead );
            graph.Extract( frame.Power, m_Kept.Power, RDG::Access::StorageRead );
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

        static uint32_t Groups( const uint32_t threads )
        {
            return ( threads + 63u ) / 64u; // LocalSize(64, 1, 1) in both passes
        }

        std::shared_ptr<Shader>          m_BuildShader;
        std::shared_ptr<Shader>          m_ReduceShader;
        std::shared_ptr<ComputePipeline> m_BuildPipeline;
        std::shared_ptr<ComputePipeline> m_ReducePipeline;
        mutable ShaderBindingLayoutCache m_BuildLayout;
        mutable ShaderBindingLayoutCache m_ReduceLayout;
        Kept                             m_Kept;
    };
} // namespace Desert::Graphic::RayTracing
