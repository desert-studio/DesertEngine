#pragma once

#include <Common/Settings/DisplaySettings.hpp>

#include <Engine/Core/Glfw.hpp>

#include <Engine/Core/Device.hpp>

#include <cstdint>
#include <vector>

namespace Desert::Graphic
{
    // A recorded capture handed out of the swap chain (SwapChain::TakeCapturedFrameTicket): the swap chain keeps the
    // backend's readback buffer and fence under Id until ReleaseCapturedFrame, and can record the next capture at
    // once. Width and Height are the captured surface's.
    struct CapturedFrameTicket
    {
        uint64_t Id     = 0;
        uint32_t Width  = 0;
        uint32_t Height = 0;
    };

    class SwapChain
    {
    public:
        SwapChain( const GLFWwindow* window ) : m_Window( window )
        {
        }
        virtual ~SwapChain() = default;

        virtual Common::ResultStr<bool> CreateSwapChain( const std::shared_ptr<Engine::Device>& device,
                                                      uint32_t* width, uint32_t* height ) = 0;

        virtual uint32_t GetBackBufferCount() const = 0;

        virtual uint32_t GetWidth() const                            = 0;
        virtual uint32_t GetHeight() const                           = 0;
        virtual void     RequestRebuild( uint32_t width, uint32_t height ) = 0;
        // A rebuild is requested and not applied yet: the frame loop joins the render task before the frame
        // boundary that applies it (Application::Run step 2), and only then (OVERLAP-ON-3).
        [[nodiscard]] virtual bool RebuildPending() const = 0;

        virtual void Release() = 0;

        /**
         * @brief CAPTURING THE FRAME THE USER IS LOOKING AT, declared here rather than on the backend.
         *
         * The swapchain is the only surface that holds the COMPOSITED picture — the scene plus everything
         * drawn over it in the swapchain pass: the editor's panels, and the shipping host's UI and loading
         * screen. `Scene::GetFinalImage()` holds none of that, which is why every capture taken before
         * this existed could argue about the world and not about the picture.
         *
         * IT IS ON THE BASE CLASS because the host that most needs it is the one that must not know about
         * Vulkan. `Runtime` compiles without the Vulkan headers on its include path — deliberately, it is
         * the shipping player — so the editor's `dynamic_pointer_cast<VulkanSwapChain>` is not a shape the
         * player can copy. Two halves, and the reason they are two is on the Vulkan override: a swapchain
         * image may only be touched between its acquire and its present.
         */
        [[nodiscard]] virtual bool SupportsFrameReadback() const noexcept = 0;

        /// Record the copy into this frame's command buffer, after the last pass and before the present. The image
        /// is acquired late, by the present graph's back-buffer import (RendererAPI::ImportBackBuffer), so this is
        /// called on the thread that executed that graph, after it: on the render task behind the job that ran it
        /// (the editor's interface, ViewportCapture::RecordWindowCapture), inline right after it where the graph
        /// runs inline (the runtime's present graph, RuntimeLayer::RecordShotIfDue).
        [[nodiscard]] virtual Common::BoolResultStr RecordFrameCapture() = 0;

        /// Collect what RecordFrameCapture asked for, tightly packed RGBA8. After the present that carried
        /// the copy.
        [[nodiscard]] virtual Common::ResultStr<std::vector<uint8_t>>
        TakeCapturedFrameRGBA8( uint32_t& outWidth, uint32_t& outHeight ) = 0;

        /// THE SAME CAPTURE IN THREE HALVES, so only the first is the frame's (the editor's shot.window; UE:
        /// FRHIGPUTextureReadback enqueued with a fence, read once IsReady, converted by FImageWriteQueue).
        /// A copy is recorded and not yet handed out as a ticket.
        [[nodiscard]] virtual bool HasPendingFrameCapture() const noexcept = 0;
        /// Release a recorded copy that will not be collected. The caller has waited for the device. No-op when none.
        virtual void DiscardFrameCapture() = 0;
        /// On the frame, after the render task handed the frame that recorded the copy to the submit: takes the
        /// copy out of the recording slot and queues a fence behind the frame. Waits for no GPU work.
        [[nodiscard]] virtual Common::ResultStr<CapturedFrameTicket> TakeCapturedFrameTicket() = 0;
        /// Any thread: waits the ticket's fence, reads the copy and packs it to tightly packed RGBA8 (BGRA and
        /// HDR surfaces converted by the backend, alpha opaque). The ticket stays held.
        [[nodiscard]] virtual Common::ResultStr<std::vector<uint8_t>>
        ResolveCapturedFrame( const CapturedFrameTicket& ticket ) = 0;
        /// On the game thread once nothing resolves the ticket: its readback memory goes to deferred deletion.
        /// Idempotent.
        virtual void ReleaseCapturedFrame( const CapturedFrameTicket& ticket ) = 0;

        // Present pacing (Scalability::DisplaySettings; machine.json's Display, handed over by QualityBoot through
        // Window::SetDisplay). Takes effect the next time the swapchain is (re)created — Window::SetDisplay
        // triggers the recreate through RequestRebuild. Until QualityBoot runs it is the DisplaySettings default.
        void SetDisplay( const Common::Scalability::DisplaySettings& display )
        {
            m_Display = display;
        }
        [[nodiscard]] const Common::Scalability::DisplaySettings& Display() const
        {
            return m_Display;
        }

        static std::shared_ptr<SwapChain> Create( const GLFWwindow* window );

    protected:
        const GLFWwindow* m_Window;
        Common::Scalability::DisplaySettings m_Display;
    };
} // namespace Desert::Graphic