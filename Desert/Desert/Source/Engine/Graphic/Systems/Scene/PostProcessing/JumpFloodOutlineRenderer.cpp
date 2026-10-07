#include "JumpFloodOutlineRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <Engine/Runtime/ResourceRegistry.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Graphic::System
{
    namespace
    {
        constexpr Core::Formats::ImageFormat kSeedFormat = ViewTargetFormats::kJFASeed;
    }

    uint32_t JumpFloodOutlineRenderer::ComputeStepCount( uint32_t width, uint32_t height )
    {
        const uint32_t maxDim = std::max( { width, height, 2u } );
        return std::max( 1u, static_cast<uint32_t>( std::ceil( std::log2( static_cast<float>( maxDim ) ) ) ) );
    }

    Common::BoolResultStr JumpFloodOutlineRenderer::Initialize()
    {
        const auto& targetFramebuffer = m_TargetFramebuffer.lock();
        if ( !targetFramebuffer )
        {
            return Common::MakeError( "JumpFloodOutlineRenderer: target framebuffer is not available" );
        }

        FramebufferSpecification spec;
        spec.DebugName = "JFA_Output";
        spec.Attachments.Attachments.emplace_back( kSeedFormat );
        m_Framebuffer = Graphic::Framebuffer::Create( spec );
        if ( !m_Framebuffer )
        {
            return Common::MakeError( "JumpFloodOutlineRenderer: failed to create the output framebuffer" );
        }
        m_Framebuffer->Resize( targetFramebuffer->GetFramebufferWidth(),
                               targetFramebuffer->GetFramebufferHeight() );

        if ( !CreatePipelines() )
        {
            return Common::MakeError( "JumpFloodOutlineRenderer: failed to create pipelines" );
        }

        m_MaterialComposite = std::make_unique<MaterialJFAComposite>();

        return BOOLSUCCESS;
    }

    bool JumpFloodOutlineRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();

        // @p framebuffer: the output (Final); null: a seed transient, built against the graph's canonical render
        // pass for one kJFASeed colour target.
        const auto makePipeline = [&]( const std::string& shaderName, const std::shared_ptr<Framebuffer>& framebuffer,
                                       const std::string& debugName ) -> std::shared_ptr<GraphicsPipeline>
        {
            const auto shader = shaderService->GetByName( shaderName );
            if ( !shader )
            {
                LOG_ERROR( "JumpFloodOutlineRenderer: missing shader '{}'", shaderName );
                return nullptr;
            }

            GraphicsPipelineSpecification spec;
            spec.DebugName = debugName;
            spec.Shader    = shader;
            if ( framebuffer )
                spec.Framebuffer = framebuffer;
            else
                spec.TargetLayout =
                     RenderTargetLayout{ .ColorFormats = { kSeedFormat }, .DepthFormat = std::nullopt };
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            spec.CullMode          = CullMode::None;

            const auto pipeline = GraphicsPipeline::Create( spec );
            if ( !pipeline )
            {
                LOG_ERROR( "JumpFloodOutlineRenderer: {}", pipeline.GetError() );
                return nullptr;
            }
            return pipeline.GetValue();
        };

        m_InitPipeline  = makePipeline( "JFA_Init", nullptr, "JFA_InitPipeline" );
        m_StepPipeline  = makePipeline( "JFA_Step", nullptr, "JFA_StepPipeline" );
        m_FinalPipeline = makePipeline( "JFA_Final", m_Framebuffer, "JFA_FinalPipeline" );

        return m_InitPipeline && m_StepPipeline && m_FinalPipeline;
    }

    void JumpFloodOutlineRenderer::OnResize( uint32_t width, uint32_t height )
    {
        if ( width == 0 || height == 0 )
            return;
        if ( m_Framebuffer )
            m_Framebuffer->Resize( width, height );
    }

    bool JumpFloodOutlineRenderer::Prepare() const
    {
        if ( !m_TargetFramebuffer.lock() || !m_Framebuffer || !m_InitPipeline || !m_StepPipeline ||
             !m_FinalPipeline )
        {
            LOG_ERROR( "JumpFloodOutlineRenderer::Prepare: the scene framebuffer, the output or a pipeline is "
                       "unavailable" );
            return false;
        }
        return true;
    }

    uint32_t JumpFloodOutlineRenderer::GetStepCount() const
    {
        const auto scene = m_TargetFramebuffer.lock();
        return scene ? ComputeStepCount( scene->GetFramebufferWidth(), scene->GetFramebufferHeight() ) : 1u;
    }

    std::optional<RDG::TextureDesc> JumpFloodOutlineRenderer::GetSeedDesc() const
    {
        const auto scene = m_TargetFramebuffer.lock();
        if ( !scene )
        {
            LOG_ERROR( "JumpFloodOutlineRenderer: the scene framebuffer is gone; no seed is created" );
            return std::nullopt;
        }
        return RDG::TextureDesc{
             .Size   = { .Width = scene->GetFramebufferWidth(), .Height = scene->GetFramebufferHeight() },
             .Format = kSeedFormat };
    }

    bool JumpFloodOutlineRenderer::RunsInit() const
    {
        return m_Enabled && m_MaskFramebuffer.lock() != nullptr;
    }

    std::shared_ptr<Image2D> JumpFloodOutlineRenderer::GetMaskImage() const
    {
        const auto maskFramebuffer = m_MaskFramebuffer.lock();
        return maskFramebuffer ? maskFramebuffer->GetColorAttachmentImage() : nullptr;
    }

    std::shared_ptr<Image2D> JumpFloodOutlineRenderer::GetSceneColorImage() const
    {
        const auto sceneFramebuffer = m_TargetFramebuffer.lock();
        return sceneFramebuffer ? sceneFramebuffer->GetColorAttachmentImage() : nullptr;
    }

    // The mask and the seeds hold per-texel data (coverage, a seed coordinate) that must not be blended between
    // texels, and a neighbour beyond the border must not wrap to the opposite edge: PointClamp.
    void JumpFloodOutlineRenderer::DeclareInitBindings( RDG::PassBuilder& pass, RDG::TextureRef mask ) const
    {
        if ( !m_InitPipeline )
            return; // RecordInit refuses by name
        // No material: the other route fills nothing.
        pass.Bindings( m_InitLayout.Get( m_InitPipeline->GetSpecification().Shader ), RDG::OtherRouteFill{} )
             .Sampled( "u_StencilTexture", mask, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() );
    }

    Common::BoolResultStr JumpFloodOutlineRenderer::RecordInit( const RDG::PassContext& context )
    {
        if ( !m_InitPipeline )
            return Common::MakeError( "JumpFloodOutlineRenderer: the init pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_InitPipeline, nullptr );
    }

    void JumpFloodOutlineRenderer::DeclareStepBindings( RDG::PassBuilder& pass, RDG::TextureRef source ) const
    {
        if ( !m_StepPipeline )
            return; // RecordStep refuses by name
        pass.Bindings( m_StepLayout.Get( m_StepPipeline->GetSpecification().Shader ), RDG::OtherRouteFill{} )
             .Sampled( "u_InputTexture", source, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() )
             .PushConstantBytes( static_cast<uint32_t>( sizeof( int32_t ) ) );
    }

    Common::BoolResultStr JumpFloodOutlineRenderer::RecordStep( const RDG::PassContext& context, uint32_t step )
    {
        if ( !m_StepPipeline )
            return Common::MakeError( "JumpFloodOutlineRenderer: the step pipeline is not initialised" );
        // Ping-pong propagation with halving sample distance.
        const int32_t stepLength = 1 << ( GetStepCount() - 1 - step );

        RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        bindings.PushConstants( &stepLength, sizeof( stepLength ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_StepPipeline, nullptr );
    }

    void JumpFloodOutlineRenderer::FillFinalMaterial()
    {
        if ( !m_MaterialComposite )
            return;
        const float effectiveWidth = RunsSteps() ? m_OutlineWidth : 0.0f;
        m_MaterialComposite->SetParams( glm::vec4( m_OutlineColor, 1.0f ), effectiveWidth, m_Smoothness );
    }

    void JumpFloodOutlineRenderer::DeclareFinalBindings( RDG::PassBuilder& pass, RDG::TextureRef seed,
                                                         RDG::TextureRef scene ) const
    {
        if ( !m_FinalPipeline || !m_MaterialComposite )
            return; // RecordFinal refuses by name
        pass.Bindings( m_FinalLayout.Get( m_FinalPipeline->GetSpecification().Shader ),
                       m_MaterialComposite->GetMaterialExecutor()->GetRouteFill() )
             .Sampled( "u_JFATexture", seed, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::PointClamp() )
             .Sampled( "u_SceneTexture", scene, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                       RDG::SamplerDesc::LinearClamp() );
    }

    Common::BoolResultStr JumpFloodOutlineRenderer::RecordFinal( const RDG::PassContext& context )
    {
        if ( !m_FinalPipeline || !m_MaterialComposite )
            return Common::MakeError( "JumpFloodOutlineRenderer: the composite pipeline is not initialised" );
        const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
        return Renderer::GetInstance().DrawFullscreen( bindings, *m_FinalPipeline,
                                                       m_MaterialComposite->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
