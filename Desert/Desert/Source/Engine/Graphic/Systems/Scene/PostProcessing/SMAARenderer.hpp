#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Materials/PostProcessing/MaterialSMAA.hpp>

namespace Desert::Graphic::System
{
    // SMAA 1x post-process (3 passes: edge detection -> blend-weight calculation -> neighborhood
    // blending). Reads the tonemapped LDR image (m_TargetFramebuffer) and writes the anti-aliased result
    // into its own framebuffer (GetSystemFramebuffer()). Runs only when this machine's post AA is SMAA
    // (Common::Settings::MachineSettings::AA; it was SceneSettings::AA until К3). Uses the
    // precomputed AreaTex/SearchTex LUTs loaded from Resources/Textures/SMAA.
    class SMAARenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        // Each pass is a raster node of the frame graph (SceneRendererFramePostFX.cpp "PostFX: SMAA*"), which
        // opens the render pass on the step's output. False: the input or a LUT is missing.
        bool Prepare() const;
        // Pass 1, into GetEdgesImage(): edge detection on GetInputImage().
        void RecordEdges();
        // Pass 2, into GetWeightsImage(): blend weights from the edges + AreaTex + SearchTex.
        void RecordWeights();
        // Pass 3, into GetOutputImage(): neighbourhood blending of the input with the weights.
        void RecordBlend();

        std::shared_ptr<Image2D> GetInputImage() const
        {
            const auto input = m_TargetFramebuffer.lock();
            return input ? input->GetColorAttachmentImage() : nullptr;
        }
        std::shared_ptr<Image2D> GetEdgesImage() const
        {
            return m_EdgesFB ? m_EdgesFB->GetColorAttachmentImage( 0 ) : nullptr;
        }
        std::shared_ptr<Image2D> GetWeightsImage() const
        {
            return m_WeightsFB ? m_WeightsFB->GetColorAttachmentImage( 0 ) : nullptr;
        }
        std::shared_ptr<Image2D> GetOutputImage() const
        {
            return m_Framebuffer ? m_Framebuffer->GetColorAttachmentImage( 0 ) : nullptr;
        }
        const std::shared_ptr<Image2D>& GetAreaTex() const
        {
            return m_AreaTex;
        }
        const std::shared_ptr<Image2D>& GetSearchTex() const
        {
            return m_SearchTex;
        }

        void Resize( uint32_t width, uint32_t height );

    private:
        void LoadLUTs();

        // Intermediate targets (final output is the base m_Framebuffer).
        std::shared_ptr<Framebuffer> m_EdgesFB;
        std::shared_ptr<Framebuffer> m_WeightsFB;

        std::shared_ptr<GraphicsPipeline> m_EdgesPipeline;
        std::shared_ptr<GraphicsPipeline> m_WeightsPipeline;
        std::shared_ptr<GraphicsPipeline> m_BlendPipeline;
        std::shared_ptr<Shader>           m_EdgesShader;
        std::shared_ptr<Shader>           m_WeightsShader;
        std::shared_ptr<Shader>           m_BlendShader;

        std::unique_ptr<MaterialSMAAEdges>   m_MatEdges;
        std::unique_ptr<MaterialSMAAWeights> m_MatWeights;
        std::unique_ptr<MaterialSMAABlend>   m_MatBlend;

        std::shared_ptr<Image2D> m_AreaTex;
        std::shared_ptr<Image2D> m_SearchTex;
    };
} // namespace Desert::Graphic::System
