#include "SMAARenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include "SMAAAreaTexData.h"   // exact SMAA AreaTex bytes (RG8)   — engine-owned copy (official iryoku/smaa)
#include "SMAASearchTexData.h" // exact SMAA SearchTex bytes (R8)  — engine-owned copy

#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    namespace
    {
        // Every pass draws into one graph texture of @p format (the edges / weights transients, or the frame's
        // output): built against the graph's canonical render pass for that one colour target.
        NO_DISCARD Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
                   MakePipeline( std::string_view name, const std::shared_ptr<Shader>& shader,
                                 Core::Formats::ImageFormat format )
        {
            Graphic::GraphicsPipelineSpecification spec;
            spec.DebugName    = std::string( name );
            spec.TargetLayout = RenderTargetLayout{ .ColorFormats = { format }, .DepthFormat = std::nullopt };
            spec.Shader       = shader;
            return Graphic::GraphicsPipeline::Create( spec );
        }
    } // namespace

    Common::BoolResultStr SMAARenderer::Initialize()
    {
        // Edges + weights are LDR (RGBA8) graph transients; the output matches the FXAA output (kSMAABlend).

        // The shaders are held by the pipelines alone: each node's layout is keyed on the shader its pipeline
        // records with (GetSpecification().Shader), so no second handle can go stale under a reload.
        auto&      shaders       = *Runtime::ResourceRegistry::GetShaderService();
        const auto edgesShader   = shaders.GetByName( "SMAAEdges" );
        const auto weightsShader = shaders.GetByName( "SMAAWeights" );
        const auto blendShader   = shaders.GetByName( "SMAABlend" );

        // The three shaders above were taken from GetByName WITHOUT a check until Г22, and a missing one
        // is a null shared_ptr that reached VulkanPipeline::CreatePipelineLayout and was dereferenced.
        // The rule inside Create refuses it by name now, and the refusal arrives here.
        const auto edges = MakePipeline( "SMAAEdges", edgesShader, ViewTargetFormats::kSMAAEdges );
        if ( !edges )
            return Common::MakeError( edges.GetError() );
        m_EdgesPipeline = edges.GetValue();

        const auto weights = MakePipeline( "SMAAWeights", weightsShader, ViewTargetFormats::kSMAAEdges );
        if ( !weights )
            return Common::MakeError( weights.GetError() );
        m_WeightsPipeline = weights.GetValue();

        const auto blend = MakePipeline( "SMAABlend", blendShader, ViewTargetFormats::kSMAABlend );
        if ( !blend )
            return Common::MakeError( blend.GetError() );
        m_BlendPipeline = blend.GetValue();

        LoadLUTs();

        return BOOLSUCCESS;
    }

    void SMAARenderer::LoadLUTs()
    {
        // AreaTex + SearchTex are precomputed, content-independent SMAA lookup tables — engine resources, not
        // user assets. They're NOT cheaply regenerable (unlike the BRDF LUT), so we embed the EXACT official
        // bytes (iryoku/smaa) as headers rather than ship loose image files that can go missing / not
        // round-trip. AreaTex is RG8 (areaTexBytes), SearchTex is R8 — both expanded to RGBA8 here.

        // AreaTex: RG8 -> RGBA8 (rg = lookup, b = 0, a = 255). The SMAA blend-weights/blend shaders sample .rg.
        {
            constexpr size_t                 kPixels = static_cast<size_t>( AREATEX_WIDTH ) * AREATEX_HEIGHT;
            std::vector<unsigned char> rgba( kPixels * 4, 0 );
            for ( size_t i = 0; i < kPixels; ++i )
            {
                rgba[i * 4 + 0] = areaTexBytes[i * 2 + 0];
                rgba[i * 4 + 1] = areaTexBytes[i * 2 + 1];
                rgba[i * 4 + 3] = 255;
            }
            Core::Formats::Image2DSpecification spec{
                 .Tag        = "SMAA_AreaTex",
                 .Width      = static_cast<uint32_t>( AREATEX_WIDTH ),
                 .Height     = static_cast<uint32_t>( AREATEX_HEIGHT ),
                 .Format     = Core::Formats::ImageFormat::RGBA8F,
                 .Mips       = 1,
                 .Data       = std::move( rgba ),
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
            };
            m_AreaTex = Image2D::Create( spec );
        }

        // SearchTex: R8 -> RGBA8 (value in .r).
        {
            std::vector<unsigned char> rgba( static_cast<size_t>( SEARCHTEX_WIDTH ) * SEARCHTEX_HEIGHT * 4, 0 );
            for ( size_t i = 0; i < static_cast<size_t>( SEARCHTEX_WIDTH ) * SEARCHTEX_HEIGHT; ++i )
            {
                rgba[i * 4 + 0] = searchTexBytes[i];
                rgba[i * 4 + 3] = 255;
            }
            Core::Formats::Image2DSpecification spec{
                 .Tag        = "SMAA_SearchTex",
                 .Width      = static_cast<uint32_t>( SEARCHTEX_WIDTH ),
                 .Height     = static_cast<uint32_t>( SEARCHTEX_HEIGHT ),
                 .Format     = Core::Formats::ImageFormat::RGBA8F,
                 .Mips       = 1,
                 .Data       = std::move( rgba ),
                 .Usage      = Core::Formats::Image2DUsage::Image2D,
                 .Properties = Core::Formats::ImageProperties::Sample,
            };
            m_SearchTex = Image2D::Create( spec );
        }
    }

    std::optional<RDG::TextureDesc> SMAARenderer::GetIntermediateDesc( const RDG::TextureDesc& input ) const
    {
        if ( !m_AreaTex || !m_SearchTex || !m_EdgesPipeline || !m_WeightsPipeline || !m_BlendPipeline )
        {
            LOG_ERROR( "SMAARenderer: a LUT or a pipeline is missing; SMAA is not recorded" );
            return std::nullopt;
        }
        return RDG::TextureDesc{ .Size   = { .Width = input.Size.Width, .Height = input.Size.Height },
                                 .Format = ViewTargetFormats::kSMAAEdges };
    }

    // SMAA samples every input bilinearly with clamped addressing (the reference LinearSampler); the bilinear
    // fetches of the edges and the AreaTex/SearchTex lookups depend on it.
    // No SMAA pass has a material: the other route fills nothing.
    void SMAARenderer::DeclareEdgesBindings( RDG::PassBuilder& pass, RDG::TextureRef input ) const
    {
        if ( !m_EdgesPipeline )
            return; // RecordEdges refuses by name
        pass.Bindings( m_EdgesLayout.Get( m_EdgesPipeline->GetSpecification().Shader ), RDG::OtherRouteFill{} )
             .Sampled( "u_ColorTex", input, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() );
    }

    Common::BoolResultStr SMAARenderer::RecordEdges( const RDG::PassContext& context )
    {
        if ( !m_EdgesPipeline )
            return Common::MakeError( "SMAARenderer: the edges pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_EdgesPipeline, nullptr );
    }

    void SMAARenderer::DeclareWeightsBindings( RDG::PassBuilder& pass, RDG::TextureRef edges, RDG::TextureRef area,
                                               RDG::TextureRef search ) const
    {
        if ( !m_WeightsPipeline )
            return; // RecordWeights refuses by name
        pass.Bindings( m_WeightsLayout.Get( m_WeightsPipeline->GetSpecification().Shader ), RDG::OtherRouteFill{} )
             .Sampled( "u_EdgesTex", edges, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Sampled( "u_AreaTex", area, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Sampled( "u_SearchTex", search, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() );
    }

    Common::BoolResultStr SMAARenderer::RecordWeights( const RDG::PassContext& context )
    {
        if ( !m_WeightsPipeline )
            return Common::MakeError( "SMAARenderer: the weights pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_WeightsPipeline, nullptr );
    }

    void SMAARenderer::DeclareBlendBindings( RDG::PassBuilder& pass, RDG::TextureRef input,
                                             RDG::TextureRef weights, RDG::TextureRef edges,
                                             RDG::TextureRef area ) const
    {
        if ( !m_BlendPipeline )
            return; // RecordBlend refuses by name
        pass.Bindings( m_BlendLayout.Get( m_BlendPipeline->GetSpecification().Shader ), RDG::OtherRouteFill{} )
             .Sampled( "u_ColorTex", input, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Sampled( "u_BlendTex", weights, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Sampled( "u_EdgesTex", edges, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() )
             .Sampled( "u_AreaTex", area, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() );
    }

    Common::BoolResultStr SMAARenderer::RecordBlend( const RDG::PassContext& context )
    {
        if ( !m_BlendPipeline )
            return Common::MakeError( "SMAARenderer: the blend pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_BlendPipeline, nullptr );
    }
} // namespace Desert::Graphic::System
