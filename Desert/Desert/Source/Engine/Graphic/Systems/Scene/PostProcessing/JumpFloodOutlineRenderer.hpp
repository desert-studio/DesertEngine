#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/RDG/RDGBuilder.hpp>

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
    //   Final  : seed[N%2] + scene color -> output (outlined scene)
    // The two seeds are transients of that graph (Builder::CreateTexture with GetSeedDesc); the renderer keeps
    // no image for them. Only the output is a member: the tonemap reads it (TonemapRenderer::Inputs::Source).
    //
    // The output framebuffer is what the tonemap stage consumes. When the outline is disabled the
    // Final pass simply passes the scene color through (see JFA_Final.glsl.frag).
    class JumpFloodOutlineRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        virtual Common::BoolResultStr Initialize() override;

        // JFA is added to the frame graph by SceneRenderer (AddFrameJumpFlood), not through the phase graph.
        void RegisterPasses( RenderGraphBuilder& /*builder*/ ) override
        {
        }

        // False: nothing can be recorded this frame (the scene framebuffer is gone); the error is logged.
        bool Prepare() const;
        // Init runs when the outline is enabled and the silhouette mask exists, selected or not, so seed[0]
        // is written this frame and the composite may sample it.
        bool RunsInit() const;
        // The ~log2(width) propagation steps run only when something is outlined this frame.
        bool RunsSteps() const
        {
            return RunsInit() && m_OutlineActive;
        }
        // ~log2 of this frame's scene size (the seeds are sized from it).
        uint32_t GetStepCount() const;
        // Step @p step samples seed[GetStepSource(step)] and writes the other seed.
        static uint32_t GetStepSource( uint32_t step )
        {
            return step % 2;
        }
        // The seed the composite samples: the last step's target, or seed[0] when no step ran.
        uint32_t GetFinalSeedIndex() const
        {
            return RunsSteps() ? GetStepCount() % 2 : 0u;
        }
        // The seed transients of this frame: the scene's size, ViewTargetFormats::kJFASeed. Nullopt: the scene
        // framebuffer is gone (logged).
        std::optional<RDG::TextureDesc> GetSeedDesc() const;

        std::shared_ptr<Image2D> GetMaskImage() const;
        std::shared_ptr<Image2D> GetSceneColorImage() const;
        // What the tonemap consumes: the outlined scene (or the scene passed through).
        std::shared_ptr<Image2D> GetOutputImage() const
        {
            return m_Framebuffer ? m_Framebuffer->GetColorAttachmentImage() : nullptr;
        }

        // Each records one fullscreen triangle inside the render pass the frame graph opens on its target:
        // Init on seed[0], Step on the seed GetStepSource does not name, Final on GetOutputImage().
        // Init: the silhouette @p mask -> seed[0].
        [[nodiscard]] Common::BoolResultStr RecordInit( const RDG::PassContext& context, RDG::TextureRef mask );
        // Step @p step: @p source (seed[GetStepSource(step)]) -> the other seed, sampling 2^(N-1-step) texels away.
        [[nodiscard]] Common::BoolResultStr RecordStep( const RDG::PassContext& context, uint32_t step,
                                                        RDG::TextureRef source );
        // Final: @p seed (the final seed, or FrameTextures::System.Black when Init did not run) composited over
        // @p scene. No steps ran -> width 0, which makes JFA_Final pass the scene through unchanged.
        [[nodiscard]] Common::BoolResultStr RecordFinal( const RDG::PassContext& context, RDG::TextureRef seed,
                                                         RDG::TextureRef scene );

        // Resizes the output (the only image the renderer keeps); the seeds follow the scene size per frame.
        void OnResize( uint32_t width, uint32_t height );

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
        // When false (nothing selected) the graph gets no ~log2(width) ping-pong STEP nodes (it still gets
        // the cheap Init, so seed[0] is written this frame and the composite may sample it; the graph
        // orders Init's colour write before the composite's sampled read).
        void SetOutlineActive( bool active )
        {
            m_OutlineActive = active;
        }

    private:
        bool CreatePipelines();

        static uint32_t ComputeStepCount( uint32_t width, uint32_t height );

    private:
        // The final output is m_Framebuffer (RenderSystem base); the ping-pong seeds are graph transients.
        std::shared_ptr<GraphicsPipeline> m_InitPipeline;
        std::shared_ptr<GraphicsPipeline> m_StepPipeline;
        std::shared_ptr<GraphicsPipeline> m_FinalPipeline;

        // The composite's uniform block (outline colour, width, smoothness); every texture is a graph binding.
        std::unique_ptr<MaterialJFAComposite> m_MaterialComposite;

        std::weak_ptr<Framebuffer> m_MaskFramebuffer;

        glm::vec3 m_OutlineColor = glm::vec3( 1.0f, 0.5f, 0.0f );
        float     m_OutlineWidth = 4.0f;
        float     m_Smoothness   = 2.0f;
        bool      m_Enabled       = true;
        bool      m_OutlineActive = false; // per-frame: is anything selected/outlined? (MeshRenderer::HasOutline)
    };
} // namespace Desert::Graphic::System
