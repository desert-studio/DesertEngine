#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <Engine/Graphic/Materials/PostProcessing/MaterialFXAA.hpp>

namespace Desert::Graphic::System
{
    // FXAA post-process pass. A raster node of the frame graph (SceneRendererFramePostFX.cpp "PostFX: FXAA"):
    // reads the tonemap's output (a transient of the graph) and writes the target the frame hands it (the view's
    // output). It keeps no image; the pipeline is built against the graph's render pass for one
    // ViewTargetFormats::kFXAA colour target.
    class FXAARenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // SETUP of "PostFX: FXAA": declares the node's one block (block 0) - @p input (the tonemap's output) as
        // u_InputTexture, with the sampler the material route sampled it with (the image's own: linear,
        // REPEAT); the material is the other route. Not initialised: nothing is declared and Record refuses.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef input ) const;
        // Records the fullscreen FXAA inside the render pass the frame graph opens on the node's target, drawing
        // the block DeclareBindings declared.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context );

    private:
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        // The block layout, derived from the pipeline's shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache  m_BindingLayout;
        std::unique_ptr<MaterialFXAA>     m_MaterialFXAA;
    };
} // namespace Desert::Graphic::System
