#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Graphic/Materials/Deferred/MaterialSSAO.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic::System
{
    // Screen-space ambient occlusion. Fullscreen: reads the G-buffer (world position + normal) and writes an AO
    // factor into a per-frame graph transient (FrameTransients::SSAO, SceneRendererFrameDeferred.cpp
    // "Deferred: SSAO"), which the deferred Composite then multiplies into the ambient term. Deferred only.
    class SSAORenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "SSAO" );
            if ( !m_Shader )
                return Common::MakeError( "SSAO shader not found" );

            // The AO image is a graph transient: the pipeline is built against the graph's canonical render
            // pass for that one colour target (no framebuffer of its own).
            GraphicsPipelineSpecification spec;
            spec.DebugName         = "SSAO";
            spec.TargetLayout      = RenderTargetLayout{ .ColorFormats = { ViewTargetFormats::kSSAO } };
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

        // Records AO into the colour target the graph node opened (the SSAO transient). @p worldPos = GBufferC,
        // @p normal = GBufferB, bound by shader name through RDG::PassBindings. viewProj = world->clip;
        // cameraPos.xyz = camera. radius and bias are WORLD distances (the shader offsets samples in world
        // space), and a world unit is a centimetre - callers passing literature values must convert through
        // Common::Units.
        //
        // SETUP: DeclareBindings declares the node's one block (block 0): u_GBufferPos / u_GBufferNormal with the
        // sampler the material route sampled the G-buffer with (the image's own: linear, REPEAT), the material
        // as the other route. Not initialised: nothing is declared and Record refuses.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef worldPos, RDG::TextureRef normal ) const
        {
            if ( !m_Pipeline || !m_Material )
                return;
            constexpr RDG::SamplerDesc kSampler = RDG::SamplerDesc::LinearRepeat();
            pass.Bindings( m_BindingLayout.Get( *m_Shader ), m_Material->GetMaterialExecutor()->GetRouteFill() )
                 .Sampled( "u_GBufferPos", worldPos, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           kSampler )
                 .Sampled( "u_GBufferNormal", normal, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           kSampler );
        }

        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context, const glm::mat4& viewProj,
                                                    const glm::vec4& cameraPos, float radius, float bias,
                                                    float power, int sampleCount )
        {
            if ( !m_Pipeline || !m_Material )
                return Common::MakeError( "Deferred: SSAO: the SSAO pipeline is not initialised" );

            m_Material->BindInputs( viewProj, cameraPos, radius, bias, power, sampleCount );

            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline,
                                                           m_Material->GetMaterialExecutor() );
        }

    private:
        std::shared_ptr<Shader>            m_Shader;
        // The block layout, derived from m_Shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache   m_BindingLayout;
        std::shared_ptr<GraphicsPipeline>  m_Pipeline;
        std::unique_ptr<MaterialSSAO>      m_Material;
    };
} // namespace Desert::Graphic::System
