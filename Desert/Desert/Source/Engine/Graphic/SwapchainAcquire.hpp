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
// waits for the present; only OUT_OF_DATE leaves the semaphore untouched, which is the one case where
// rebuilding and acquiring again on the same semaphore is legal.

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

    // One acquire for one frame, the way the loop must run it. `acquire()` returns
    // Common::ResultStr<AcquireStatus> (an error is anything that is neither an image nor OUT_OF_DATE);
    // `rebuild()` returns Common::ResultStr<bool> and must not change which frame-in-flight slot is current --
    // the fence was reset and the semaphore chosen for that slot, and the submit reads the slot again.
    template <typename Acquire, typename Rebuild>
    [[nodiscard]] Common::ResultStr<AcquireStatus> AcquireForFrame( Acquire&& acquire, Rebuild&& rebuild )
    {
        auto first = std::forward<Acquire>( acquire )();
        if ( !first.IsSuccess() || SignalsSemaphore( first.GetValue() ) )
            return first;

        const auto rebuilt = std::forward<Rebuild>( rebuild )();
        if ( !rebuilt.IsSuccess() )
            return Common::MakeFormattedError<AcquireStatus>(
                 "the swapchain was out of date and rebuilding it failed: {}", rebuilt.GetError() );

        auto second = acquire();
        if ( second.IsSuccess() && !SignalsSemaphore( second.GetValue() ) )
            return Common::MakeError<AcquireStatus>(
                 "the swapchain was out of date again immediately after it was rebuilt; no image was acquired "
                 "for this frame." );
        return second;
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
