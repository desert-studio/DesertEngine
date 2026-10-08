#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/RDG/RDGPassBindings.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>

namespace Desert::Graphic::System
{
    // Full-screen copy: snapshots the composited scene colour into this frame's scene-copy transient
    // (FrameTransients::SceneColorCopy) that the glass (refraction) and SSR (reflection source) sample, so the
    // glass pass never reads + writes the same attachment (feedback loop).
    class CopyRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override
        {
            m_Shader = Runtime::ResourceRegistry::GetShaderService()->GetByName( "Copy" );
            if ( !m_Shader )
                return Common::MakeError( "Copy shader not found" );

            // The copy is a graph transient: the pipeline is built against the graph's canonical render pass for
            // that one colour target (no framebuffer of its own).
            GraphicsPipelineSpecification spec;
            spec.DebugName         = "Copy";
            spec.TargetLayout      = RenderTargetLayout{ .ColorFormats = { ViewTargetFormats::kSceneColorCopy },
                                                         .DepthFormat  = std::nullopt };
            spec.Shader            = m_Shader;
            spec.DepthTestEnabled  = false;
            spec.DepthWriteEnabled = false;
            const auto pipeline    = Graphic::GraphicsPipeline::Create( spec );
            if ( !pipeline )
                return Common::MakeError( pipeline.GetError() );
            m_Pipeline = pipeline.GetValue();
            return BOOLSUCCESS;
        }

        // SETUP of "Deferred: SceneCopy": the node's one block (block 0) - u_Input, the shader's only resource,
        // so no other route. The sampler is the one the material route sampled the scene colour with (the
        // image's own: linear, REPEAT). Without a pipeline nothing is declared and Record refuses.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef source ) const
        {
            if ( !m_Pipeline )
                return;
            pass.Bindings( m_BindingLayout.Get( m_Shader ), RDG::OtherRouteFill{} )
                 .Sampled( "u_Input", source, RDG::Access::SampledGraphics, RDG::SubresourceRange::All(),
                           RDG::SamplerDesc::LinearRepeat() );
        }

        // Inside the render pass the graph opens on the copy: opens the block DeclareBindings declared.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context )
        {
            if ( !m_Pipeline )
                return Common::MakeError( "Deferred: SceneCopy: the copy pipeline is not initialised" );
            const RDG::PassBindings bindings( context, context.GetBindingBlock( 0 ) );
            return Renderer::GetInstance().DrawFullscreen( bindings, *m_Pipeline, nullptr );
        }

    private:
        std::shared_ptr<Shader>           m_Shader;
        // The block layout, derived from m_Shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache  m_BindingLayout;
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
    };
} // namespace Desert::Graphic::System
