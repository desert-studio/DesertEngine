#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>

#include <glm/glm.hpp>

#include <array>
#include <string_view>

namespace Desert::Graphic::System
{
    // The graph textures the deferred composite samples, as refs of THIS frame's graph that the Composite node
    // declared reads of. An input not produced this frame is the system texture neutral for it (UE
    // GSystemTextures): never an unbound slot, never a skipped draw.
    struct DeferredCompositeInputs
    {
        RDG::TextureRef                GBufferA;        // albedo + metallic
        RDG::TextureRef                GBufferB;        // normal + roughness
        RDG::TextureRef                GBufferC;        // world position
        RDG::TextureRef                GBufferEmissive; // HDR emissive
        RDG::TextureRef                SSAO;            // FrameTransients::SSAO, or System.White (AO = 1)
        RDG::TextureRef                GI;              // RSM-GI accumulation, or System.Black (no indirect)
        std::array<RDG::TextureRef, 4> ShadowMaps;      // cascade i, or System.White past the valid count
        RDG::TextureRef CloudShadowMap; // FrameTransients::CloudShadowMap, or System.White (no cloud shadow)
    };

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

        // Shades the G-buffer into the scene target, inside the render pass the frame graph opens on it with
        // LOAD ("Deferred: Composite"), which preserves the forward-rendered sky/grid; the shader discards
        // non-geometry texels. Every texture of @p inputs is bound by shader name through RDG::PassBindings;
        // the material writes this frame's values (lights, shadow/cloud UBs, environment) every frame, so the
        // draw's every-slot-filled check holds in every mode. lightDir.xyz = the direction the sun travels;
        // lightColor.rgb/.a = colour/intensity; cameraPos.xyz = camera world position; debugMode selects a raw
        // channel (0 = lit); giMode picks the indirect-light source (0 = off, 1 = screen-space, 2 = RSM).
        [[nodiscard]] Common::BoolResultStr
        Record( const RDG::PassContext& context, const DeferredCompositeInputs& inputs, const glm::vec4& lightDir,
                const glm::vec4& lightColor, const glm::vec4& cameraPos, int debugMode,
                const ShaderProtocols::PointLight& pointLights, const ShaderProtocols::SpotLight& spotLights,
                const DeferredShadowInput& shadow, float giIntensity, bool ssaoEnabled, int giMode,
                const CloudShadowInput& cloudShadow, const DeferredEnvironmentInput& environment )
        {
            if ( !m_Pipeline || !m_Material )
                return Common::MakeError(
                     "Deferred: Composite: the deferred-lighting pipeline is not initialised" );

            ReportEnvironmentGap( environment );
            m_Material->BindInputs( lightDir, lightColor, cameraPos, debugMode, pointLights, spotLights, shadow,
                                    giIntensity, ssaoEnabled, giMode, cloudShadow, environment );

            // The sampler the material route sampled these images with (the image's own: linear, REPEAT).
            constexpr RDG::SamplerDesc kSampler = RDG::SamplerDesc::LinearRepeat();
            RDG::PassBindings          bindings( context );
            const auto                 sampled = [&]( std::string_view name, RDG::TextureRef texture ) {
                bindings.Sampled( name, texture, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                                                  kSampler );
            };
            sampled( "u_GBufferA", inputs.GBufferA );
            sampled( "u_GBufferB", inputs.GBufferB );
            sampled( "u_GBufferC", inputs.GBufferC );
            sampled( "u_GBufferEmissive", inputs.GBufferEmissive );
            sampled( "u_SSAO", inputs.SSAO );
            sampled( "u_GI", inputs.GI );
            static constexpr std::string_view kShadowMaps[4] = { "u_ShadowMap0", "u_ShadowMap1", "u_ShadowMap2",
                                                                 "u_ShadowMap3" };
            for ( size_t i = 0; i < inputs.ShadowMaps.size(); ++i )
                sampled( kShadowMaps[i], inputs.ShadowMaps[i] );
            sampled( "u_CloudShadowMap", inputs.CloudShadowMap );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                           m_Material->GetMaterialExecutor() );
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
