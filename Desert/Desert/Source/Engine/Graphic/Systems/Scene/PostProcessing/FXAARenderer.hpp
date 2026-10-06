#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

#include <Engine/Graphic/Materials/PostProcessing/MaterialFXAA.hpp>

namespace Desert::Graphic::System
{
    // FXAA post-process pass. Reads the tonemapped image (m_TargetFramebuffer) and writes an
    // anti-aliased copy into its own framebuffer (GetSystemFramebuffer()). A raster node of the frame graph
    // (SceneRendererFramePostFX.cpp "PostFX: FXAA"), which opens the render pass on GetOutputImage().
    class FXAARenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        // The sampled input (the configured source framebuffer's colour 0) and the ColorTarget.
        std::shared_ptr<Image2D> GetInputImage() const
        {
            const auto input = m_TargetFramebuffer.lock();
            return input ? input->GetColorAttachmentImage() : nullptr;
        }
        std::shared_ptr<Image2D> GetOutputImage() const
        {
            return m_Framebuffer ? m_Framebuffer->GetColorAttachmentImage( 0 ) : nullptr;
        }

        // SETUP of "PostFX: FXAA": declares the node's one block (block 0) - @p input (GetInputImage() imported)
        // as u_InputTexture, with the sampler the material route sampled it with (the image's own: linear,
        // REPEAT); the material is the other route. Not initialised: nothing is declared and Record refuses.
        void DeclareBindings( RDG::PassBuilder& pass, RDG::TextureRef input ) const;
        // Records the fullscreen FXAA inside the render pass the frame graph opens on GetOutputImage(), drawing
        // the block DeclareBindings declared.
        [[nodiscard]] Common::BoolResultStr Record( const RDG::PassContext& context );
        void Resize( uint32_t width, uint32_t height );

    private:
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        // The block layout, derived from the pipeline's shader's reflection once per compile (not per frame).
        mutable ShaderBindingLayoutCache  m_BindingLayout;
        std::unique_ptr<MaterialFXAA>     m_MaterialFXAA;
    };
} // namespace Desert::Graphic::System
