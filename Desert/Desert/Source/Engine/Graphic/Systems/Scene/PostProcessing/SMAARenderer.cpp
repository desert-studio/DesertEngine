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
        std::shared_ptr<Framebuffer> MakeColorFB( std::string_view name, Core::Formats::ImageFormat format,
                                                  uint32_t w, uint32_t h )
        {
            FramebufferSpecification fbSpec;
            fbSpec.DebugName = std::string( name );
            fbSpec.Attachments.Attachments.push_back( format );
            auto fb = Graphic::Framebuffer::Create( fbSpec );
            fb->Resize( w, h );
            return fb;
        }

        // @p fb: the output framebuffer (pass 3); null: a graph transient of @p transientFormat (passes 1 and 2),
        // built against the graph's canonical render pass for that one colour target.
        NO_DISCARD Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
                   MakePipeline( std::string_view name, const std::shared_ptr<Framebuffer>& fb,
                                 const std::shared_ptr<Shader>& shader, Core::Formats::ImageFormat transientFormat )
        {
            Graphic::GraphicsPipelineSpecification spec;
            spec.DebugName = std::string( name );
            if ( fb )
                spec.Framebuffer = fb;
            else
                spec.TargetLayout = RenderTargetLayout{ .ColorFormats = { transientFormat } };
            spec.Shader = shader;
            return Graphic::GraphicsPipeline::Create( spec );
        }
    } // namespace

    Common::BoolResultStr SMAARenderer::Initialize()
    {
        const auto& targetFramebuffer = m_TargetFramebuffer.lock();
        if ( !targetFramebuffer )
        {
            // `DESERT_VERIFY( false )` stood here — it takes the process down, and then FALLS THROUGH
            // to dereference the null it was asserting about. This function returns a Result; say so.
            return Common::MakeError( "SMAARenderer: the target framebuffer is gone." );
        }

        const uint32_t w = targetFramebuffer->GetFramebufferWidth();
        const uint32_t h = targetFramebuffer->GetFramebufferHeight();

        // Edges + weights are LDR (RGBA8) graph transients; the final output matches the FXAA output (RGBA32F).
        m_Framebuffer = MakeColorFB( "SMAABlend", ViewTargetFormats::kSMAABlend, w, h );

        // The shaders are held by the pipelines alone: each node's layout is keyed on the shader its pipeline
        // records with (GetSpecification().Shader), so no second handle can go stale under a reload.
        auto&      shaders       = *Runtime::ResourceRegistry::GetShaderService();
        const auto edgesShader   = shaders.GetByName( "SMAAEdges" );
        const auto weightsShader = shaders.GetByName( "SMAAWeights" );
        const auto blendShader   = shaders.GetByName( "SMAABlend" );

        // The three shaders above were taken from GetByName WITHOUT a check until Г22, and a missing one
        // is a null shared_ptr that reached VulkanPipeline::CreatePipelineLayout and was dereferenced.
        // The rule inside Create refuses it by name now, and the refusal arrives here.
        const auto edges = MakePipeline( "SMAAEdges", nullptr, edgesShader, ViewTargetFormats::kSMAAEdges );
        if ( !edges )
            return Common::MakeError( edges.GetError() );
        m_EdgesPipeline = edges.GetValue();

        const auto weights = MakePipeline( "SMAAWeights", nullptr, weightsShader, ViewTargetFormats::kSMAAEdges );
        if ( !weights )
            return Common::MakeError( weights.GetError() );
        m_WeightsPipeline = weights.GetValue();

        const auto blend = MakePipeline( "SMAABlend", m_Framebuffer, blendShader, ViewTargetFormats::kSMAABlend );
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

    std::optional<RDG::TextureDesc> SMAARenderer::GetIntermediateDesc() const
    {
        const auto input = m_TargetFramebuffer.lock();
        if ( !input || !input->GetColorAttachmentImage() || !m_AreaTex || !m_SearchTex || !m_EdgesPipeline ||
             !m_WeightsPipeline || !m_BlendPipeline )
        {
            LOG_ERROR(
                 "SMAARenderer: the input framebuffer, a LUT or a pipeline is missing; SMAA is not recorded" );
            return std::nullopt;
        }
        return RDG::TextureDesc{
             .Size   = { .Width = input->GetFramebufferWidth(), .Height = input->GetFramebufferHeight() },
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

    void SMAARenderer::Resize( uint32_t width, uint32_t height )
    {
        // Only the output is a member; the edges and weights are created per frame from the input's size.
        if ( m_Framebuffer )
            m_Framebuffer->Resize( width, height );
    }
} // namespace Desert::Graphic::System
