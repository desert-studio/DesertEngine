#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <Engine/Graphic/Materials/PostProcessing/MaterialJFAComposite.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace Desert::Graphic::System
{
    // Jump Flood Algorithm based object outline.
    //
    // Pipeline (all fullscreen passes; each is a Raster node of the frame graph, SceneRendererFramePostFX.cpp
    // "PostFX: JumpFlood*", and the graph opens the render pass on the image the node declares):
    //   Init   : silhouette mask        -> seed[0]
    //   Step xN: seed[i%2]              -> seed[(i+1)%2]   (ping-pong, halving step length)
    //   Final  : seed[N%2]              -> blended over the DISPLAY image (the outline band only)
    // The two seeds are transients of that graph (Builder::CreateTexture with GetSeedDesc); the renderer keeps
    // no image at all.
    //
    // AN EDITOR OVERLAY AFTER THE TONEMAP (UE EPass::SelectionOutline, composited after the tonemap and FXAA and
    // before EPass::EditorPrimitive). The selection depth is the scene's own: the silhouette mask MeshRenderer draws
    // for the selected meshes. The chain runs only on a frame that outlines something, after the spatial AA, and the
    // Final pass blends the band (One, OneMinusSrcAlpha) over the display image, encoded for the view's output
    // device; the scene colour the tonemap reads is never written by it.
    class JumpFloodOutlineRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // JFA is added to the frame graph by SceneRenderer (AddFrameJumpFlood), not through the phase graph.

        // False: nothing can be recorded this frame (the scene framebuffer is gone); the error is logged.
        [[nodiscard]] bool Prepare() const;
        // The chain (Init, the ~log2(width) steps, Final) runs when the outline is enabled, the silhouette mask
        // exists and something is outlined this frame; otherwise the frame graph gets no outline node at all.
        [[nodiscard]] bool Runs() const;
        // ~log2 of this frame's scene size (the seeds are sized from it).
        [[nodiscard]] uint32_t GetStepCount() const;
        // Step @p step samples seed[GetStepSource(step)] and writes the other seed.
        static uint32_t GetStepSource( uint32_t step )
        {
            return step % 2;
        }
        // The seed the composite samples: the last step's target.
        [[nodiscard]] uint32_t GetFinalSeedIndex() const
        {
            return GetStepCount() % 2;
        }
        // The seed transients of this frame: the scene's size, ViewTargetFormats::kJFASeed. Nullopt: the scene
        // framebuffer is gone (logged).
        [[nodiscard]] std::optional<RDG::TextureDesc> GetSeedDesc() const;

        [[nodiscard]] std::shared_ptr<Image2D> GetMaskImage() const;

        // Each records one fullscreen triangle inside the render pass the frame graph opens on its target:
        // Init on seed[0], Step on the seed GetStepSource does not name, Final on the display image (the tonemap's
        // or the spatial AA's output, ViewTargetFormats::kTonemap), loaded.
        // Every Declare* adds block 0 to the node's setup; its Record* draws from that block only.
        // Init: the silhouette @p mask -> seed[0].
        void DeclareInitBindings( RDG::PassBuilder& pass, RDG::TextureRef mask ) const;
        [[nodiscard]] Common::BoolResultStr RecordInit( const RDG::PassContext& context );
        // Step @p step: @p source (seed[GetStepSource(step)]) -> the other seed, sampling 2^(N-1-step) texels
        // away.
        void DeclareStepBindings( RDG::PassBuilder& pass, RDG::TextureRef source ) const;
        [[nodiscard]] Common::BoolResultStr RecordStep( const RDG::PassContext& context, uint32_t step );
        // Final: the band around @p seed (the last step's seed) blended over the display image. The composite's
        // uniform block is filled by FillFinalMaterial (@p displayGamma: the view's Film Gamma, the output device
        // the tonemap encoded for), called in the setup before the declaration.
        void FillFinalMaterial( float displayGamma );
        void DeclareFinalBindings( RDG::PassBuilder& pass, RDG::TextureRef seed ) const;
        [[nodiscard]] Common::BoolResultStr RecordFinal( const RDG::PassContext& context );

        // The silhouette mask is produced by MeshRenderer; wired once after both systems init.
        void SetMaskFramebuffer( const std::weak_ptr<Framebuffer>& maskFramebuffer )
        {
            m_MaskFramebuffer = maskFramebuffer;
        }

        void SetOutlineColor( const glm::vec3& color )
        {
            m_OutlineColor = color;
        }
        void SetOutlineWidth( float width )
        {
            m_OutlineWidth = width;
        }
        void SetOutlineSmoothness( float smoothness )
        {
            m_Smoothness = smoothness;
        }
        void SetEnabled( bool enabled )
        {
            m_Enabled = enabled;
        }
        // When false (nothing selected) the graph gets no outline node.
        void SetOutlineActive( bool active )
        {
            m_OutlineActive = active;
        }

    private:
        bool CreatePipelines();

        static uint32_t ComputeStepCount( uint32_t width, uint32_t height );

    private:
        // No image: the ping-pong seeds are graph transients and Final draws onto the display image.
        std::shared_ptr<GraphicsPipeline> m_InitPipeline;
        std::shared_ptr<GraphicsPipeline> m_StepPipeline;
        std::shared_ptr<GraphicsPipeline> m_FinalPipeline;

        // The composite's uniform block (outline colour, width, smoothness); every texture is a graph binding.
        std::unique_ptr<MaterialJFAComposite> m_MaterialComposite;
        mutable ShaderBindingLayoutCache      m_InitLayout; // the three shaders' layouts, kept between frames
        mutable ShaderBindingLayoutCache      m_StepLayout;
        mutable ShaderBindingLayoutCache      m_FinalLayout;

        std::weak_ptr<Framebuffer> m_MaskFramebuffer;

        glm::vec3 m_OutlineColor = glm::vec3( 1.0f, 0.5f, 0.0f );
        float     m_OutlineWidth = 4.0f;
        float     m_Smoothness   = 2.0f;
        bool      m_Enabled       = true;
        bool      m_OutlineActive = false; // per-frame: is anything selected/outlined? (MeshRenderer::HasOutline)
    };
} // namespace Desert::Graphic::System
