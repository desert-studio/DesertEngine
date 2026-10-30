#pragma once

// WHAT AN ACQUIRE LEAVES BEHIND, AND WHAT THE FRAME MAY DO NEXT. Vulkan-free on purpose, so that the rule
// the frame loop runs is the rule a suite with no device can drive (SwapchainAcquire suite).
//
// The incident: showing the editor window after the splash made vkAcquireNextImageKHR answer
// VK_SUBOPTIMAL_KHR. The backend read every non-VK_SUCCESS as "no image", rebuilt the swapchain and
// acquired AGAIN on the same semaphore -- but a suboptimal acquire HAS acquired: the image is ours and the
// semaphore has a signal pending. The second acquire was on a signalled semaphore, the first image was never
// waited on, and the GPU stalled until macOS killed the command buffer (kIOGPUCommandBufferCallbackError-
// Timeout, then VK_ERROR_DEVICE_LOST), on every launch.
//
// The standard scheme (Vulkan spec, WSI chapter; UE's FVulkanViewport::TryAcquireImageIndex): SUCCESS and
// SUBOPTIMAL both hand over an image and signal the semaphore, so the frame goes on with it and the rebuild
// waits for the present; only OUT_OF_DATE leaves the semaphore untouched. The acquire is late (at the
// frame's first back-buffer import, PRESENT-COST), so an OUT_OF_DATE there is "no image this frame" and the
// rebuild is requested for the next frame boundary rather than run mid-frame (AcquireLate).

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace Desert::Graphic
{
    enum class AcquireStatus : uint8_t
    {
        Acquired,           // VK_SUCCESS
        AcquiredSuboptimal, // VK_SUBOPTIMAL_KHR -- an image IS acquired; the rebuild belongs after the present
        OutOfDate,          // VK_ERROR_OUT_OF_DATE_KHR -- no image, the semaphore was not touched
    };

    // Whether the semaphore handed to the acquire now carries a pending signal. Every later step depends on
    // this one fact: a signalled semaphore must be waited on by the frame's submit, and must never be handed
    // to another acquire.
    [[nodiscard]] constexpr bool SignalsSemaphore( const AcquireStatus status )
    {
        return status != AcquireStatus::OutOfDate;
    }

    // The one acquire of a frame, at the frame's first import of the back buffer (VulkanRendererAPI::
    // ImportBackBuffer -> VulkanSwapChainOutput::AcquireImage). `acquire()` returns
    // Common::ResultStr<AcquireStatus> (an error is anything that is neither an image nor OUT_OF_DATE);
    // `requestRebuild()` asks for a rebuild at the NEXT frame boundary (RebuildAtFrameBoundary) and returns
    // nothing. The answer is true when the frame holds an image (SUCCESS or SUBOPTIMAL: the semaphore is
    // signalled and the frame's submit must wait on it; a SUBOPTIMAL rebuild belongs after the present), false
    // when the swapchain was OUT_OF_DATE: no image this frame, the semaphore untouched, the frame submitted
    // without the output and not presented. It never rebuilds and acquires again: this runs at the frame's
    // last pass, and a rebuild now would release what the frame already recorded against (the composite
    // framebuffer, after a device wait-idle in the middle of the frame).
    template <typename Acquire, typename RequestRebuild>
    [[nodiscard]] Common::ResultStr<bool> AcquireLate( Acquire&& acquire, RequestRebuild&& requestRebuild )
    {
        const auto acquired = std::forward<Acquire>( acquire )();
        if ( !acquired.IsSuccess() )
            return Common::MakeFormattedError<bool>( "{}", acquired.GetError() );
        if ( SignalsSemaphore( acquired.GetValue() ) )
            return Common::MakeSuccess( true );

        std::forward<RequestRebuild>( requestRebuild )();
        return Common::MakeSuccess( false );
    }

    struct SwapchainExtent
    {
        uint32_t Width  = 0;
        uint32_t Height = 0;
    };

    class SwapchainRebuildRequest
    {
    public:
        void Request( const SwapchainExtent extent )
        {
            const std::scoped_lock lock( m_Mutex );
            m_Pending = extent;
        }

        void RequestUnlessPending( const SwapchainExtent extent )
        {
            const std::scoped_lock lock( m_Mutex );
            if ( !m_Pending )
                m_Pending = extent;
        }

        [[nodiscard]] bool IsPending() const
        {
            const std::scoped_lock lock( m_Mutex );
            return m_Pending.has_value();
        }

        [[nodiscard]] std::optional<SwapchainExtent> Peek() const
        {
            const std::scoped_lock lock( m_Mutex );
            return m_Pending;
        }

        void Settle( const SwapchainExtent served )
        {
            const std::scoped_lock lock( m_Mutex );
            if ( m_Pending && m_Pending->Width == served.Width && m_Pending->Height == served.Height )
                m_Pending.reset();
        }

    private:
        mutable std::mutex             m_Mutex;
        std::optional<SwapchainExtent> m_Pending;
    };

    template <typename Rebuild>
    [[nodiscard]] Common::ResultStr<bool> RebuildAtFrameBoundary( SwapchainRebuildRequest& request,
                                                                  Rebuild&&                rebuild )
    {
        const std::optional<SwapchainExtent> pending = request.Peek();
        if ( !pending )
            return Common::MakeSuccess( false );

        auto rebuilt = std::forward<Rebuild>( rebuild )( *pending );
        if ( !rebuilt.IsSuccess() || rebuilt.GetValue() )
            request.Settle( *pending );
        return rebuilt;
    }
} // namespace Desert::Graphic
