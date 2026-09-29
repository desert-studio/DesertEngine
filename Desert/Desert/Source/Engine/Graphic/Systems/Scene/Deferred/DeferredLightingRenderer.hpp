#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::System
{
    // Deferred lighting + G-buffer debug pass. Fullscreen: reads the scene renderer's MRT G-buffer and writes
    // the shaded (or debug) result into the scene target framebuffer, which the post chain then tonemaps.
    // Runs in the manual chain (like Tonemap), only when RenderPath == Deferred.
    class DeferredLightingRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "DeferredLighting" );
            if ( !m_Shader )
                return Common::MakeError( "DeferredLighting shader not found" );

            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "Deferred lighting target framebuffer missing" );

            GraphicsPipelineSpecification spec;
            spec.DebugName         = "DeferredLighting";
            spec.Framebuffer       = target;
            spec.Shader            = m_Shader;
            // Fullscreen composite over the forward-rendered scene: no depth test/write (the quad has no
            // meaningful depth), and LOAD the target so the real sky/grid drawn by the forward passes are
            // preserved — the shader discards non-geometry texels so that scene shows through.
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            spec.UseLoadRenderPass = true;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();

            m_Material = std::make_unique<MaterialDeferredLighting>();
            return BOOLSUCCESS;
        }

        // Not a render-graph pass — driven from SceneRenderer's manual chain after the geometry graph.
        void RegisterPasses( RenderGraphBuilder& ) override
        {
        }

        // Shades the G-buffer into the scene target. lightDir.xyz = the direction the sun travels;
        // lightColor.rgb/.a = colour/intensity; cameraPos.xyz = camera world position (view vector);
        // debugMode selects a raw channel (0 = lit); point/spot = the scene's dynamic lights.
        // giMode picks the indirect-light source (0 = off, 1 = screen-space gather, 2 = the RSM giImage).
        void Execute( const std::shared_ptr<Framebuffer>& gbuffer, const glm::vec4& lightDir,
                      const glm::vec4& lightColor, const glm::vec4& cameraPos, int debugMode,
                      const ShaderProtocols::PointLight& pointLights, const ShaderProtocols::SpotLight& spotLights,
                      const DeferredShadowInput& shadow, const std::shared_ptr<Image2D>& aoImage,
                      float giIntensity, bool ssaoEnabled, int giMode, const std::shared_ptr<Image2D>& giImage,
                      const CloudShadowInput& cloudShadow, const DeferredEnvironmentInput& environment )
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !gbuffer || !m_Pipeline || !m_Material )
                return;

            ReportEnvironmentGap( environment );

            auto& renderer = Renderer::GetInstance();
            // Inside the render pass the frame graph opens on the scene target with LOAD ("Deferred:
            // Composite"), which preserves the forward-rendered sky/grid already in it; the shader writes lit
            // meshes where the G-buffer has geometry and discards elsewhere, compositing the deferred meshes
            // over the real forward scene (so the skybox toggle + camera motion still work).
            m_Material->BindInputs( gbuffer->GetColorAttachmentImage( 0 ), gbuffer->GetColorAttachmentImage( 1 ),
                                    gbuffer->GetColorAttachmentImage( 2 ), gbuffer->GetColorAttachmentImage( 3 ),
                                    lightDir, lightColor, cameraPos, debugMode, pointLights, spotLights, shadow,
                                    aoImage, giIntensity, ssaoEnabled, giMode, giImage, cloudShadow, environment );
            renderer.SubmitFullscreenQuad( m_Pipeline.get(), m_Material->GetMaterialExecutor() );
        }

    private:
        // §1.4: a resource that did not arrive is logged with its reason, never quietly replaced. There is
        // no "no environment" mode here — the ambient IS the environment, so an incomplete set means the
        // IBL bake failed or never ran, and every static opaque surface in the frame is about to be shaded
        // by whatever the descriptor fallback happens to be. Say which of the three is missing; a bake that
        // produced two cubes and no LUT is a different failure from one that produced nothing.
        //
        // Edge-triggered, and per RENDERER instance rather than per process: a fullscreen pass runs every
        // frame in every open viewport, so an unconditional log is 60 lines a second times the number of
        // views, and a `static` one would let the second viewport's failure hide behind the first's.
        void ReportEnvironmentGap( const DeferredEnvironmentInput& environment )
        {
            const bool complete = environment.IsComplete();
            if ( complete == m_EnvironmentWasComplete )
                return;
            m_EnvironmentWasComplete = complete;

            if ( !complete )
                LOG_ERROR( "DeferredLighting: no baked environment to shade the ambient with — irradiance "
                           "cube {}, prefiltered cube {}, BRDF LUT {}. Every missing one is bound to the "
                           "engine's EMPTY environment (a black cube), so static opaque geometry gets no "
                           "ambient at all — a deterministic wrong answer rather than the previous scene's "
                           "sky, which is what it used to get (Г14).",
                           environment.Irradiance ? "present" : "MISSING",
                           environment.Prefiltered ? "present" : "MISSING",
                           environment.BrdfLut ? "present" : "MISSING" );
        }

        std::shared_ptr<Shader>                   m_Shader;
        std::shared_ptr<GraphicsPipeline>         m_Pipeline;
        std::unique_ptr<MaterialDeferredLighting> m_Material;

        // Starts true so the first INCOMPLETE frame is the edge that logs; a renderer that never loses
        // its environment says nothing at all.
        bool m_EnvironmentWasComplete = true;
    };
} // namespace Desert::Graphic::System
