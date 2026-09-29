#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>

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

        // Records the fullscreen FXAA inside the render pass the frame graph opens on GetOutputImage().
        void Record();
        void Resize( uint32_t width, uint32_t height );

    private:
        std::shared_ptr<GraphicsPipeline> m_Pipeline;
        std::shared_ptr<Shader>           m_Shader;
        std::unique_ptr<MaterialFXAA>     m_MaterialFXAA;
    };
} // namespace Desert::Graphic::System
