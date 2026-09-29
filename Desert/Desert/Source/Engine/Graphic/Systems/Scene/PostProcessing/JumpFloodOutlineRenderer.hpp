#pragma once

#include <Engine/Graphic/Systems/RenderSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>

#include <Engine/Graphic/Materials/PostProcessing/JumpFloodMaterials.hpp>
#include <Engine/Graphic/Materials/PostProcessing/MaterialJFAComposite.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Graphic::System
{
    // Jump Flood Algorithm based object outline.
    //
    // Pipeline (all fullscreen passes; each is a Raster node of the frame graph, SceneRendererFramePostFX.cpp
    // "PostFX: JumpFlood*", and the graph opens the render pass on the image the node declares):
    //   Init   : silhouette mask        -> seed[0]
    //   Step xN: seed[i%2]              -> seed[(i+1)%2]   (ping-pong, halving step length)
    //   Final  : seed[N%2] + scene color -> output (outlined scene)
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
        uint32_t GetStepCount() const
        {
            return m_StepCount;
        }
        // Step @p step samples seed[GetStepSource(step)] and writes the other seed.
        static uint32_t GetStepSource( uint32_t step )
        {
            return step % 2;
        }
        // The seed the composite samples: the last step's target, or seed[0] when no step ran.
        uint32_t GetFinalSeedIndex() const
        {
            return RunsSteps() ? m_StepCount % 2 : 0u;
        }

        std::shared_ptr<Image2D> GetMaskImage() const;
        std::shared_ptr<Image2D> GetSceneColorImage() const;
        std::shared_ptr<Image2D> GetSeedImage( uint32_t index ) const
        {
            return m_SeedFramebuffers[index] ? m_SeedFramebuffers[index]->GetColorAttachmentImage() : nullptr;
        }
        // What the tonemap consumes: the outlined scene (or the scene passed through).
        std::shared_ptr<Image2D> GetOutputImage() const
        {
            return m_Framebuffer ? m_Framebuffer->GetColorAttachmentImage() : nullptr;
        }

        // Each records one fullscreen quad inside the render pass the frame graph opens on its target:
        // Init on seed[0], Step on the seed GetStepSource does not name, Final on GetOutputImage().
        void RecordInit();
        void RecordStep( uint32_t step );
        void RecordFinal();

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
        bool CreateFramebuffers( uint32_t width, uint32_t height );
        bool CreatePipelines();

        static uint32_t ComputeStepCount( uint32_t width, uint32_t height );

    private:
        // Ping-pong seed targets + final output (m_Framebuffer from RenderSystem base).
        std::shared_ptr<Framebuffer> m_SeedFramebuffers[2];

        std::shared_ptr<GraphicsPipeline> m_InitPipeline;
        std::shared_ptr<GraphicsPipeline> m_StepPipeline;
        std::shared_ptr<GraphicsPipeline> m_FinalPipeline;

        std::unique_ptr<MaterialJFAInit>                   m_MaterialInit;
        std::vector<std::unique_ptr<MaterialJFAStep>>      m_StepMaterials; // one per step (avoids descriptor aliasing)
        std::unique_ptr<MaterialJFAComposite>              m_MaterialComposite;

        std::weak_ptr<Framebuffer> m_MaskFramebuffer;

        uint32_t  m_StepCount    = 1;
        glm::vec3 m_OutlineColor = glm::vec3( 1.0f, 0.5f, 0.0f );
        float     m_OutlineWidth = 4.0f;
        float     m_Smoothness   = 2.0f;
        bool      m_Enabled       = true;
        bool      m_OutlineActive = false; // per-frame: is anything selected/outlined? (MeshRenderer::HasOutline)
    };
} // namespace Desert::Graphic::System
