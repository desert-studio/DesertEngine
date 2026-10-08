#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <optional>

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
        // opens the render pass on the step's output. The edges and the blend weights are transients of that graph
        // (Builder::CreateTexture with GetIntermediateDesc); the renderer keeps no image for them.
        // Nullopt: nothing to record this frame (the input, a LUT or a pipeline is missing; logged).
        [[nodiscard]] std::optional<RDG::TextureDesc> GetIntermediateDesc() const;
        // Every Declare* adds block 0 to the node's setup; its Record* draws from that block only.
        // Pass 1, into the edges target the node declared: edge detection on @p input.
        void DeclareEdgesBindings( RDG::PassBuilder& pass, RDG::TextureRef input ) const;
        [[nodiscard]] Common::BoolResultStr RecordEdges( const RDG::PassContext& context );
        // Pass 2, into the weights target: blend weights from @p edges + AreaTex + SearchTex.
        void DeclareWeightsBindings( RDG::PassBuilder& pass, RDG::TextureRef edges, RDG::TextureRef area,
                                     RDG::TextureRef search ) const;
        [[nodiscard]] Common::BoolResultStr RecordWeights( const RDG::PassContext& context );
        // Pass 3, into GetOutputImage(): neighbourhood blending of @p input with @p weights (@p edges and @p area
        // are the shader's diagnostic views).
        void DeclareBlendBindings( RDG::PassBuilder& pass, RDG::TextureRef input, RDG::TextureRef weights,
                                   RDG::TextureRef edges, RDG::TextureRef area ) const;
        [[nodiscard]] Common::BoolResultStr RecordBlend( const RDG::PassContext& context );

        [[nodiscard]] std::shared_ptr<Image2D> GetInputImage() const
        {
            const auto input = m_TargetFramebuffer.lock();
            return input ? input->GetColorAttachmentImage() : nullptr;
        }
        [[nodiscard]] std::shared_ptr<Image2D> GetOutputImage() const
        {
            return m_Framebuffer ? m_Framebuffer->GetColorAttachmentImage( 0 ) : nullptr;
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetAreaTex() const
        {
            return m_AreaTex;
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetSearchTex() const
        {
            return m_SearchTex;
        }

        void Resize( uint32_t width, uint32_t height );

    private:
        void LoadLUTs();

        // The output is the base m_Framebuffer (read after the graph by whatever presents the view); the edges
        // and weights are this frame's graph transients.
        std::shared_ptr<GraphicsPipeline> m_EdgesPipeline;
        std::shared_ptr<GraphicsPipeline> m_WeightsPipeline;
        std::shared_ptr<GraphicsPipeline> m_BlendPipeline;
        // The three pipelines' shaders' layouts, kept between frames (keyed on GetSpecification().Shader).
        mutable ShaderBindingLayoutCache m_EdgesLayout;
        mutable ShaderBindingLayoutCache m_WeightsLayout;
        mutable ShaderBindingLayoutCache m_BlendLayout;

        std::shared_ptr<Image2D> m_AreaTex;
        std::shared_ptr<Image2D> m_SearchTex;
    };
} // namespace Desert::Graphic::System
