#pragma once

#include <Common/Core/Profiler.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <bit>
#include <format>

namespace Desert::Graphic::System
{
    // THE CLOSEST-DEPTH HIERARCHICAL Z-BUFFER of the view (UE: BuildHZB's ClosestHZB), built from the scene depth
    // once per frame as a transient of the frame graph: mip 0 the depth at full resolution, each mip above it the
    // closest depth of the texels it covers (Programs/Deferred/HZB.shader, one dispatch per mip). Screen tracing
    // walks it (Common/HZBTrace.glslh): today SSR's rays; it is the view's, not SSR's - occlusion culling and
    // screen traces of other passes read the same hierarchy.
    class HZBRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "HZB" );
            if ( !m_Shader )
                return Common::MakeError( "HZB shader not found" );
            const auto pipeline = ComputePipeline::Create( { .Shader = m_Shader, .DebugName = "HZB" } );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            return BOOLSUCCESS;
        }

        // GRAPH BUILD: the hierarchy of @p depth (the scene depth, @p width x @p height) as a new transient of
        // @p graph, and the passes that fill it ("HZB: Mip n"). An invalid ref when the pipeline is missing.
        [[nodiscard]] RDG::TextureRef AddPasses( RDG::Builder& graph, const RDG::TextureRef depth,
                                                 const uint32_t width, const uint32_t height )
        {
            if ( !m_Pipeline || !depth.IsValid() || width == 0u || height == 0u )
                return {};
            const uint32_t        mips = static_cast<uint32_t>( std::bit_width( std::max( width, height ) ) );
            const RDG::TextureRef hzb =
                 graph.CreateTexture( RDG::TextureDesc{ .Size   = { .Width = width, .Height = height },
                                                        .Format = ViewTargetFormats::kHZB,
                                                        .Mips   = mips },
                                      "HZB" );
            for ( uint32_t mip = 0; mip < mips; ++mip )
                graph.AddPass(
                     std::format( "HZB: Mip {}", mip ), RDG::PassFlags::Compute,
                     [&, mip]( RDG::PassBuilder& pass )
                     {
                         // Mip 0 reads the depth; every other mip the one below it, as a single-mip view.
                         const RDG::TextureRef       source = mip == 0u ? depth : hzb;
                         const RDG::SubresourceRange range =
                              mip == 0u ? RDG::SubresourceRange::All() : RDG::SubresourceRange::Mip( mip - 1u );
                         pass.Bindings( m_Layout.Get( m_Shader ), Renderer::GetPipelineRouteFill( *m_Pipeline ) )
                              .Texture( "SourceMipTexture", source, RDG::Access::SampledCompute, range )
                              .Sampler( "PointClampSampler", RDG::SamplerDesc::PointClamp() )
                              .Storage( "DestinationMipTexture", hzb, RDG::Access::StorageWrite, mip );
                     },
                     [pipeline = m_Pipeline, mip, width,
                      height]( RDG::PassContext& context ) -> Common::BoolResultStr
                     {
                         DESERT_PROFILE_PASS( "HZB" );
                         const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
                         return Renderer::GetInstance().DispatchCompute(
                              bindings, *pipeline, GroupCount( std::max( 1u, width >> mip ) ),
                              GroupCount( std::max( 1u, height >> mip ) ), 1 );
                     } );
            return hzb;
        }

    private:
        // HZB.shader's LocalSize(8, 8, 1).
        static uint32_t GroupCount( const uint32_t texels )
        {
            return ( texels + 7u ) / 8u;
        }

        std::shared_ptr<Shader>          m_Shader;
        mutable ShaderBindingLayoutCache m_Layout;
        std::shared_ptr<ComputePipeline> m_Pipeline;
    };
} // namespace Desert::Graphic::System
