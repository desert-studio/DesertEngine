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

    void JumpFloodOutlineRenderer::RunQuad( const std::shared_ptr<Framebuffer>& target, const std::string& debugName,
                                            const GraphicsPipeline* pipeline, const MaterialExecutor* executor )
    {
        auto& renderer = Renderer::GetInstance();

        auto renderPass = RenderPass::Create( {
             .TargetFramebuffer = target,
             .DebugName         = debugName,
        } );

        renderer.BeginRenderPass( renderPass.get(), true );
        renderer.SubmitFullscreenQuad( pipeline, executor );
        renderer.EndRenderPass();
    }

    void JumpFloodOutlineRenderer::Execute()
    {
        const auto& sceneFramebuffer = m_TargetFramebuffer.lock();
        if ( !sceneFramebuffer )
        {
            LOG_ERROR( "JumpFloodOutlineRenderer::Execute: scene framebuffer is unavailable" );
            return;
        }

        const auto sceneColor = sceneFramebuffer->GetColorAttachmentImage();

        // Index of the seed framebuffer holding the most recent result.
        int readIndex = 0;

        const auto& maskFramebuffer = m_MaskFramebuffer.lock();
        if ( m_Enabled && maskFramebuffer )
        {
            // Init: silhouette mask -> seed[0]. ALWAYS run it (even with nothing selected) so seed[0] is
            // written + transitioned to SHADER_READ_ONLY this frame and the composite below can safely
            // sample it. (Skipping Init too leaves seed[0] in UNDEFINED layout on the first frame -> hazard.)
            m_MaterialInit->BindInputs( maskFramebuffer->GetColorAttachmentImage().get() );
            RunQuad( m_SeedFramebuffers[0], "JFA_Init", m_InitPipeline.get(),
                     m_MaterialInit->GetMaterialExecutor() );

            // Ping-pong propagation steps with halving sample distance — the EXPENSIVE part (~log2(width)
            // full-screen passes). Only needed when something is actually outlined; with an empty mask the
            // composite passes the scene through (effectiveWidth == 0) and never needs the propagated seeds.
            // Sync between Init and the composite is guaranteed by the render pass finalLayout
            // (SHADER_READ_ONLY) + EndRenderPass's COLOR_WRITE->SHADER_READ barrier, NOT by these passes.
            if ( m_OutlineActive )
            {
                for ( uint32_t i = 0; i < m_StepCount; ++i )
                {
                    const int writeIndex = 1 - readIndex;
                    m_StepMaterials[i]->BindInputs( m_SeedFramebuffers[readIndex]->GetColorAttachmentImage().get(),
                                                    1 << ( m_StepCount - 1 - i ) );
                    RunQuad( m_SeedFramebuffers[writeIndex], "JFA_Step", m_StepPipeline.get(),
                             m_StepMaterials[i]->GetMaterialExecutor() );
                    readIndex = writeIndex;
                }
            }
        }

        // Final composite -> output framebuffer. When disabled, width 0 makes the shader pass the
        // scene through unchanged (JFA_Final early-out).
        // No steps ran (nothing selected) -> width 0 makes JFA_Final pass the scene through unchanged.
        const float effectiveWidth = ( m_Enabled && m_OutlineActive ) ? m_OutlineWidth : 0.0f;
        m_MaterialComposite->BindInputs( m_SeedFramebuffers[readIndex]->GetColorAttachmentImage().get(),
                                         sceneColor.get(), glm::vec4( m_OutlineColor, 1.0f ), effectiveWidth,
                                         m_Smoothness );
        RunQuad( m_Framebuffer, "JFA_Final", m_FinalPipeline.get(), m_MaterialComposite->GetMaterialExecutor() );
    }
} // namespace Desert::Graphic::System
