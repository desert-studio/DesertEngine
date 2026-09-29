#include "JumpFloodOutlineRenderer.hpp"
#include <Engine/Graphic/ViewTargetFormats.hpp>

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

        const uint32_t width  = targetFramebuffer->GetFramebufferWidth();
        const uint32_t height = targetFramebuffer->GetFramebufferHeight();

        if ( !CreateFramebuffers( width, height ) )
        {
            return Common::MakeError( "JumpFloodOutlineRenderer: failed to create framebuffers" );
        }

        if ( !CreatePipelines() )
        {
            return Common::MakeError( "JumpFloodOutlineRenderer: failed to create pipelines" );
        }

        m_MaterialInit      = std::make_unique<MaterialJFAInit>();
        m_MaterialComposite = std::make_unique<MaterialJFAComposite>();

        m_StepCount = ComputeStepCount( width, height );
        m_StepMaterials.clear();
        for ( uint32_t i = 0; i < m_StepCount; ++i )
        {
            m_StepMaterials.push_back( std::make_unique<MaterialJFAStep>() );
        }

        return BOOLSUCCESS;
    }

    bool JumpFloodOutlineRenderer::CreateFramebuffers( uint32_t width, uint32_t height )
    {
        const auto makeFramebuffer = [&]( const std::string& name )
        {
            FramebufferSpecification spec;
            spec.DebugName = name;
            spec.Attachments.Attachments.push_back( kSeedFormat );

            auto framebuffer = Graphic::Framebuffer::Create( spec );
            framebuffer->Resize( width, height );
            return framebuffer;
        };

        m_SeedFramebuffers[0] = makeFramebuffer( "JFA_Seed0" );
        m_SeedFramebuffers[1] = makeFramebuffer( "JFA_Seed1" );
        m_Framebuffer         = makeFramebuffer( "JFA_Output" );

        return m_SeedFramebuffers[0] && m_SeedFramebuffers[1] && m_Framebuffer;
    }

    bool JumpFloodOutlineRenderer::CreatePipelines()
    {
        const auto shaderService = Runtime::ResourceRegistry::GetShaderService();

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
            spec.DebugName         = debugName;
            spec.Shader            = shader;
            spec.Framebuffer       = framebuffer;
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

        m_InitPipeline  = makePipeline( "JFA_Init", m_SeedFramebuffers[0], "JFA_InitPipeline" );
        m_StepPipeline  = makePipeline( "JFA_Step", m_SeedFramebuffers[0], "JFA_StepPipeline" );
        m_FinalPipeline = makePipeline( "JFA_Final", m_Framebuffer, "JFA_FinalPipeline" );

        return m_InitPipeline && m_StepPipeline && m_FinalPipeline;
    }

    void JumpFloodOutlineRenderer::OnResize( uint32_t width, uint32_t height )
    {
        if ( width == 0 || height == 0 )
            return;

        if ( m_SeedFramebuffers[0] )
            m_SeedFramebuffers[0]->Resize( width, height );
        if ( m_SeedFramebuffers[1] )
            m_SeedFramebuffers[1]->Resize( width, height );
        if ( m_Framebuffer )
            m_Framebuffer->Resize( width, height );

        const uint32_t newStepCount = ComputeStepCount( width, height );
        if ( newStepCount != m_StepCount )
        {
            m_StepCount = newStepCount;
            m_StepMaterials.clear();
            for ( uint32_t i = 0; i < m_StepCount; ++i )
            {
                m_StepMaterials.push_back( std::make_unique<MaterialJFAStep>() );
            }
        }
    }

    bool JumpFloodOutlineRenderer::Prepare() const
    {
        if ( !m_TargetFramebuffer.lock() || !m_Framebuffer || !m_SeedFramebuffers[0] || !m_SeedFramebuffers[1] )
        {
            LOG_ERROR( "JumpFloodOutlineRenderer::Prepare: the scene, seed or output framebuffer is unavailable" );
            return false;
        }
        return true;
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

    void JumpFloodOutlineRenderer::RecordInit()
    {
        // Init: silhouette mask -> seed[0].
        const auto mask = GetMaskImage();
        if ( !mask )
            return;
        m_MaterialInit->BindInputs( mask.get() );
        Renderer::GetInstance().SubmitFullscreenQuad( m_InitPipeline.get(),
                                                      m_MaterialInit->GetMaterialExecutor() );
    }

    void JumpFloodOutlineRenderer::RecordStep( uint32_t step )
    {
        // Ping-pong propagation with halving sample distance.
        m_StepMaterials[step]->BindInputs( GetSeedImage( GetStepSource( step ) ).get(),
                                           1 << ( m_StepCount - 1 - step ) );
        Renderer::GetInstance().SubmitFullscreenQuad( m_StepPipeline.get(),
                                                      m_StepMaterials[step]->GetMaterialExecutor() );
    }

    void JumpFloodOutlineRenderer::RecordFinal()
    {
        // Final composite -> output. No steps ran (nothing selected, or the outline is off) -> width 0 makes
        // JFA_Final pass the scene through unchanged.
        const auto  sceneColor     = GetSceneColorImage();
        const float effectiveWidth = RunsSteps() ? m_OutlineWidth : 0.0f;
        m_MaterialComposite->BindInputs( GetSeedImage( GetFinalSeedIndex() ).get(), sceneColor.get(),
                                         glm::vec4( m_OutlineColor, 1.0f ), effectiveWidth, m_Smoothness );
        Renderer::GetInstance().SubmitFullscreenQuad( m_FinalPipeline.get(),
                                                      m_MaterialComposite->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
