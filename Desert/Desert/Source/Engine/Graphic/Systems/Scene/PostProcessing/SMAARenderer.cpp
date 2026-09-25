#include "SMAARenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
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

        NO_DISCARD Common::ResultStr<std::shared_ptr<GraphicsPipeline>>
                   MakePipeline( std::string_view name, const std::shared_ptr<Framebuffer>& fb,
                                 const std::shared_ptr<Shader>& shader )
        {
            Graphic::GraphicsPipelineSpecification spec;
            spec.DebugName   = std::string( name );
            spec.Framebuffer = fb;
            spec.Shader      = shader;
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

        // Intermediate targets: edges + weights are LDR (RGBA8); final matches the FXAA output (RGBA32F).
        m_EdgesFB     = MakeColorFB( "SMAAEdges", ViewTargetFormats::kSMAAEdges, w, h );
        m_WeightsFB   = MakeColorFB( "SMAAWeights", ViewTargetFormats::kSMAAEdges, w, h );
        m_Framebuffer = MakeColorFB( "SMAABlend", ViewTargetFormats::kSMAABlend, w, h );

        auto& shaders   = *Runtime::ResourceRegistry::GetShaderService();
        m_EdgesShader   = shaders.GetByName( "SMAAEdges" );
        m_WeightsShader = shaders.GetByName( "SMAAWeights" );
        m_BlendShader   = shaders.GetByName( "SMAABlend" );

        // The three shaders above were taken from GetByName WITHOUT a check until Г22, and a missing one
        // is a null shared_ptr that reached VulkanPipeline::CreatePipelineLayout and was dereferenced.
        // The rule inside Create refuses it by name now, and the refusal arrives here.
        const auto edges = MakePipeline( "SMAAEdges", m_EdgesFB, m_EdgesShader );
        if ( !edges )
            return Common::MakeError( edges.GetError() );
        m_EdgesPipeline = edges.GetValue();

        const auto weights = MakePipeline( "SMAAWeights", m_WeightsFB, m_WeightsShader );
        if ( !weights )
            return Common::MakeError( weights.GetError() );
        m_WeightsPipeline = weights.GetValue();

        const auto blend = MakePipeline( "SMAABlend", m_Framebuffer, m_BlendShader );
        if ( !blend )
            return Common::MakeError( blend.GetError() );
        m_BlendPipeline = blend.GetValue();

        m_MatEdges   = std::make_unique<MaterialSMAAEdges>();
        m_MatWeights = std::make_unique<MaterialSMAAWeights>();
        m_MatBlend   = std::make_unique<MaterialSMAABlend>();

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

    void SMAARenderer::Execute()
    {
        const auto& input = m_TargetFramebuffer.lock();
        if ( !input || !m_AreaTex || !m_SearchTex )
        {
            LOG_ERROR( "SMAARenderer::Execute: missing input framebuffer or LUTs" );
            return;
        }

        auto&      renderer   = Renderer::GetInstance();
        const auto inputColor = input->GetColorAttachmentImage();

        // Pass 1 — edge detection: scene color -> edges.
        {
            auto rp = RenderPass::Create( { .TargetFramebuffer = m_EdgesFB, .DebugName = "SMAAEdgesPass" } );
            renderer.BeginRenderPass( rp.get() );
            m_MatEdges->BindInputs( inputColor );
            renderer.SubmitFullscreenQuad( m_EdgesPipeline.get(), m_MatEdges->GetMaterialExecutor() );
            renderer.EndRenderPass();
        }

        // Pass 2 — blend weights: edges + AreaTex + SearchTex -> weights.
        {
            auto rp = RenderPass::Create( { .TargetFramebuffer = m_WeightsFB, .DebugName = "SMAAWeightsPass" } );
            renderer.BeginRenderPass( rp.get() );
            m_MatWeights->BindInputs( m_EdgesFB->GetColorAttachmentImage().get(), m_AreaTex.get(),
                                      m_SearchTex.get() );
            renderer.SubmitFullscreenQuad( m_WeightsPipeline.get(), m_MatWeights->GetMaterialExecutor() );
            renderer.EndRenderPass();
        }

        // Pass 3 — neighborhood blending: scene color + weights -> final.
        {
            auto rp = RenderPass::Create( { .TargetFramebuffer = m_Framebuffer, .DebugName = "SMAABlendPass" } );
            renderer.BeginRenderPass( rp.get() );
            m_MatBlend->BindInputs( inputColor, m_WeightsFB->GetColorAttachmentImage().get(),
                                    m_EdgesFB->GetColorAttachmentImage().get(), m_AreaTex.get() );
            renderer.SubmitFullscreenQuad( m_BlendPipeline.get(), m_MatBlend->GetMaterialExecutor() );
            renderer.EndRenderPass();
        }
    }

    void SMAARenderer::Resize( uint32_t width, uint32_t height )
    {
        if ( m_EdgesFB )   m_EdgesFB->Resize( width, height );
        if ( m_WeightsFB ) m_WeightsFB->Resize( width, height );
        if ( m_Framebuffer ) m_Framebuffer->Resize( width, height );
    }
} // namespace Desert::Graphic::System
