#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <optional>

namespace Desert::Graphic::System
{
    // SMAA 1x post-process (3 passes: edge detection -> blend-weight calculation -> neighborhood
    // blending). Reads the tonemap's output (a transient of the frame graph) and writes the anti-aliased result
    // into the target the frame hands it (the view's output); it keeps no image of its own. Runs only when this
    // machine's post AA is SMAA (Common::Settings::MachineSettings::AA; it was SceneSettings::AA until К3). Uses
    // the precomputed AreaTex/SearchTex LUTs loaded from Resources/Textures/SMAA.
    class SMAARenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // Each pass is a raster node of the frame graph (SceneRendererFramePostFX.cpp "PostFX: SMAA*"), which
        // opens the render pass on the step's output. The edges and the blend weights are transients of that graph
        // (Builder::CreateTexture with GetIntermediateDesc), sized from @p input (the tonemap output's desc).
        // Nullopt: nothing to record this frame (a LUT or a pipeline is missing; logged).
        [[nodiscard]] std::optional<RDG::TextureDesc> GetIntermediateDesc( const RDG::TextureDesc& input ) const;
        // Every Declare* adds block 0 to the node's setup; its Record* draws from that block only.
        // Pass 1, into the edges target the node declared: edge detection on @p input.
        void DeclareEdgesBindings( RDG::PassBuilder& pass, RDG::TextureRef input ) const;
        [[nodiscard]] Common::BoolResultStr RecordEdges( const RDG::PassContext& context );
        // Pass 2, into the weights target: blend weights from @p edges + AreaTex + SearchTex.
        void DeclareWeightsBindings( RDG::PassBuilder& pass, RDG::TextureRef edges, RDG::TextureRef area,
                                     RDG::TextureRef search ) const;
        [[nodiscard]] Common::BoolResultStr RecordWeights( const RDG::PassContext& context );
        // Pass 3, into the node's target: neighbourhood blending of @p input with @p weights (@p edges and @p area
        // are the shader's diagnostic views).
        void DeclareBlendBindings( RDG::PassBuilder& pass, RDG::TextureRef input, RDG::TextureRef weights,
                                   RDG::TextureRef edges, RDG::TextureRef area ) const;
        [[nodiscard]] Common::BoolResultStr RecordBlend( const RDG::PassContext& context );

        [[nodiscard]] const std::shared_ptr<Image2D>& GetAreaTex() const
        {
            return m_AreaTex;
        }
        [[nodiscard]] const std::shared_ptr<Image2D>& GetSearchTex() const
        {
            return m_SearchTex;
        }

    private:
        void LoadLUTs();

        // No image: the edges and weights are this frame's graph transients and the output is the frame's ref.
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
