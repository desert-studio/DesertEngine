#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialSSAO.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::System
{
    // Screen-space ambient occlusion. Fullscreen: reads the scene renderer's G-buffer (world position +
    // normal) and writes an AO factor into its own target framebuffer, which the deferred lighting pass then
    // multiplies into the ambient term. Runs in the manual chain (like DeferredLighting), only when Deferred.
    class SSAORenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSAO" );
            if ( !m_Shader )
                return Common::MakeError( "SSAO shader not found" );

            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "SSAO target framebuffer missing" );

            GraphicsPipelineSpecification spec;
            spec.DebugName         = "SSAO";
            spec.Framebuffer       = target;
            spec.Shader            = m_Shader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();

            m_Material = std::make_unique<MaterialSSAO>();
            return BOOLSUCCESS;
        }

        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        // Records AO from the G-buffer into the SSAO target (GetAOImage), inside the render pass the frame
        // graph opens on it (SceneRendererFrameDeferred.cpp "Deferred: SSAO"). viewProj = world->clip; cameraPos.xyz = camera.
        // radius and bias are WORLD distances (the shader offsets samples in world space), and a world
        // unit is a centimetre - callers passing literature values must convert through Common::Units.
        void Execute( const std::shared_ptr<Framebuffer>& gbuffer, const glm::mat4& viewProj,
                      const glm::vec4& cameraPos, float radius, float bias, float power, int sampleCount )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !gbuffer || !m_Pipeline || !m_Material )
                return;

            auto& renderer = Renderer::GetInstance();
            // GBufferC(2) = world position, GBufferB(1) = normal.
            m_Material->BindInputs( gbuffer->GetColorAttachmentImage( 2 ), gbuffer->GetColorAttachmentImage( 1 ),
                                    viewProj, cameraPos, radius, bias, power, sampleCount );
            renderer.SubmitFullscreenQuad( m_Pipeline.get(), m_Material->GetMaterialExecutor() );
        }

        std::shared_ptr<Image2D> GetAOImage() const
        {
            const auto& target = m_TargetFramebuffer.lock();
            return target ? target->GetColorAttachmentImage( 0 ) : nullptr;
        }

    private:
        std::shared_ptr<Shader>            m_Shader;
        std::shared_ptr<GraphicsPipeline>  m_Pipeline;
        std::unique_ptr<MaterialSSAO>      m_Material;
    };
} // namespace Desert::Graphic::System
