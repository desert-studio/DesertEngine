#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialDeferredLighting.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/FrameGraphRefs.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <type_traits>
#include <string_view>

namespace Desert::Graphic::System
{
    // The graph textures the deferred composite samples, as refs of THIS frame's graph that the Composite node
    // declared reads of. An input not produced this frame is the system texture neutral for it (UE
    // GSystemTextures): never an unbound slot, never a skipped draw.
    struct DeferredCompositeInputs
    {
        RDG::TextureRef GBufferA;        // albedo + metallic
        RDG::TextureRef GBufferB;        // normal + roughness
        RDG::TextureRef GBufferShadingWord; // the uint shading word (R32_UINT)
        RDG::TextureRef GBufferDepth;       // device depth: world position is reconstructed from it
        RDG::TextureRef GBufferEmissive; // HDR emissive
        RDG::TextureRef SSAO;            // FrameTransients::SSAO, or System.White (AO = 1)
        RDG::TextureRef GI;              // RSM-GI accumulation, or System.Black (no indirect)
        // The scene/view inputs (SceneViewInputsOf): cascades, environment cubes, BRDF LUT, cloud shadow map.
        SceneViewInputs View;
    };

    // The frame's dynamic lights as graph buffers of THIS frame's graph, uploaded by "Upload: Lights.Point" /
    // "Upload: Lights.Spot" (Builder::QueueBufferUpload) before the Composite node that reads them.
    struct DeferredCompositeLights
    {
        RDG::BufferRef Point;
        RDG::BufferRef Spot;
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

        // GRAPH BUILD (before the Composite node): the lights as two graph buffers, each uploaded by the graph's
        // upload command. No light is one zeroed entry (a storage buffer is never empty); the shader loops
        // 0..count, the counts being the material's LightsMetadata.
        static DeferredCompositeLights UploadLights( RDG::Builder&                      graph,
                                                     const ShaderProtocols::PointLight& points,
                                                     const ShaderProtocols::SpotLight&  spots )
        {
            const auto upload = [&graph]( const auto& lights, std::string_view name )
            {
                using Payload = typename std::decay_t<decltype( lights )>::value_type;
                const Payload                    none{};
                const std::span<const std::byte> bytes =
                     lights.empty() ? std::as_bytes( std::span<const Payload>( &none, 1 ) )
                                    : std::as_bytes( std::span<const Payload>( lights.data(), lights.size() ) );
                const RDG::BufferRef buffer = graph.CreateBuffer( RDG::BufferDesc{ bytes.size() }, name );
                graph.QueueBufferUpload( buffer, bytes );
                return buffer;
            };
            return { upload( points.PointLights, "Lights.Point" ), upload( spots.SpotLights, "Lights.Spot" ) };
        }

        // SETUP of "Deferred: Composite", FIRST: the material's values for this frame (lead decision B - a
        // material whose route fill the setup validates is filled before the validation reads it, never in the
        // exec). lightDir.xyz = the direction the sun travels; lightColor.rgb/.a = colour/intensity; cameraPos.xyz
        // = camera world position; debugMode selects a raw channel (0 = lit); giMode picks the indirect-light
        // source (0 = off, 1 = screen-space, 2 = RSM).
        void FillMaterial( const glm::vec4& lightDir, const glm::vec4& lightColor, const glm::vec4& cameraPos,
                           const glm::mat4& invJitteredViewProjection, int debugMode, uint32_t pointCount,
                           uint32_t spotCount, const DeferredShadowInput& shadow, float giIntensity,
                           bool ssaoEnabled, int giMode, const CloudShadowInput& cloudShadow,
                           const DeferredEnvironmentInput& environment )
        {
            if ( !m_Material )
                return;
            ReportEnvironmentGap( environment );
            m_Material->BindInputs( lightDir, lightColor, cameraPos, invJitteredViewProjection, debugMode, pointCount, spotCount,
                                    shadow, giIntensity, ssaoEnabled, giMode, cloudShadow, environment );
        }

        // SETUP of "Deferred: Composite", after FillMaterial: the node's one block (block 0). The G-buffer, AO and
        // GI with the sampler the material route sampled them with (the image's own: linear, REPEAT, all mips),
        // the two light buffers as StorageRead entries, the scene/view inputs the shader samples.
        void DeclareCompositeBindings( RDG::PassBuilder& pass, const DeferredCompositeInputs& inputs,
                                       const DeferredCompositeLights& lights ) const
        {
            if ( !m_Pipeline || !m_Material )
                return;
            constexpr RDG::SamplerDesc kSampler = RDG::SamplerDesc::LinearRepeat();
            const auto&                layout   = m_BindingLayout.Get( m_Shader );
            auto block = pass.Bindings( layout, m_Material->GetMaterialExecutor()->GetRouteFill() );
            block.Sampled( "u_GBufferA", inputs.GBufferA, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), kSampler )
                 .Sampled( "u_GBufferB", inputs.GBufferB, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), kSampler )
                 // The word is an integer (texelFetch only) and depth is never filtered: both point-sampled.
                 .Sampled( "u_GBufferShadingWord", inputs.GBufferShadingWord, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_GBufferDepth", inputs.GBufferDepth, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), RDG::SamplerDesc::PointClamp() )
                 .Sampled( "u_GBufferEmissive", inputs.GBufferEmissive, RDG::Access::SampledGraphics,
                           RDG::SubresourceRange::All(), kSampler )
                 .Sampled( "u_SSAO", inputs.SSAO, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           kSampler )
                 .Sampled( "u_GI", inputs.GI, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           kSampler )
                 .Storage( ShaderProtocols::PointLight::Name, lights.Point, RDG::Access::StorageRead )
                 .Storage( ShaderProtocols::SpotLight::Name, lights.Spot, RDG::Access::StorageRead );
            if ( layout )
                BindSceneViewInputs( block, inputs.View, *layout );
        }

        // EXEC: shades the G-buffer into the scene target, inside the render pass the frame graph opens on it
        // with LOAD (which preserves the forward-rendered sky/grid; the shader discards non-geometry texels),
        // from block 0 that DeclareCompositeBindings declared.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context )
        {
            if ( !m_Pipeline || !m_Material )
                return Common::MakeError(
                     "Deferred: Composite: the deferred-lighting pipeline is not initialised" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                           m_Material->GetMaterialExecutor() );
        }

    private:
        // The DeferredLighting shader's layout, kept between frames (follows the shader object and its reload).
        mutable ShaderBindingLayoutCache m_BindingLayout;
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
