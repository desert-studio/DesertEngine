#pragma once

#include <Engine/Graphic/ViewTargetFormats.hpp>
#include <Engine/Graphic/Systems/RenderSystem.hpp>

#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Graphic/View/PassHistory.hpp>
#include <Engine/Graphic/View/ViewFrame.hpp>

#include <cstdint>
#include <memory>

namespace Desert::Graphic::System
{
    // THE VIEW'S SCENE COLOUR HISTORY (UE: FPreviousViewInfo's scene colour the screen traces read): last frame's
    // lit opaque scene, the radiance every screen trace returns at a hit - SSR's reflection rays and SSGI's
    // diffuse rays alike. One image per view, filled by the frame's scene copy (SceneRenderer::AddFrameSceneCopy)
    // after the deferred lighting and the forward opaque meshes, read by the next frame's traces through the hit
    // pixel's velocity. It belongs to neither trace: with SSR off and SSGI on (or the reverse) the history is the
    // same.
    class SceneColorHistoryRenderer final : public RenderSystem
    {
    public:
        using RenderSystem::RenderSystem;

        Common::BoolResultStr Initialize() override
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target )
                return Common::MakeError( "Scene colour history: target framebuffer missing" );
            FramebufferSpecification spec;
            spec.DebugName = "SceneColorHistory";
            spec.Attachments.Attachments.emplace_back( ViewTargetFormats::kSceneColorCopy );
            m_Framebuffer = Framebuffer::Create( spec );
            m_Framebuffer->Resize( target->GetFramebufferWidth(), target->GetFramebufferHeight() );
            return BOOLSUCCESS;
        }

        // The previous frame is a different world — see IRenderSystem::OnSceneReplaced, kind 1.
        void OnSceneReplaced() override
        {
            m_Stamp.Invalidate();
        }
        // Camera cut — see IRenderSystem::OnTemporalHistoryReset.
        void OnTemporalHistoryReset() override
        {
            m_Stamp.Invalidate();
        }

        // GRAPH BUILD, before a trace imports the image: follows the scene target's size (a resize invalidates the
        // history). False: nothing to hold (no target, zero size).
        [[nodiscard]] bool Prepare()
        {
            const auto& target = m_TargetFramebuffer.lock();
            if ( !target || !m_Framebuffer )
                return false;
            const uint32_t w = target->GetFramebufferWidth();
            const uint32_t h = target->GetFramebufferHeight();
            if ( w == 0u || h == 0u )
                return false;
            if ( m_Framebuffer->GetFramebufferWidth() != w || m_Framebuffer->GetFramebufferHeight() != h )
            {
                m_Framebuffer->Resize( w, h );
                m_Stamp.Invalidate();
            }
            return true;
        }

        // The image this frame's scene copy writes and the next frame's traces read.
        [[nodiscard]] std::shared_ptr<Image2D> GetImage() const
        {
            return m_Framebuffer ? m_Framebuffer->GetColorAttachmentImage( 0 ) : nullptr;
        }
        // Whether it holds the view's previous frame (PassHistory.hpp), asked while the graph is built.
        [[nodiscard]] bool ReadableIn( const ViewFrame& frame ) const
        {
            return m_Stamp.ReadableIn( frame );
        }
        // Stamped by the scene copy's exec once the copy was recorded.
        void Stamp( const uint64_t viewFrameIndex )
        {
            m_Stamp.Stamp( viewFrameIndex );
        }

    private:
        std::shared_ptr<Framebuffer> m_Framebuffer;
        PassHistoryStamp             m_Stamp; // which view frame wrote GetImage()
    };
} // namespace Desert::Graphic::System
