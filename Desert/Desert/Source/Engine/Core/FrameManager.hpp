#pragma once

#include <Common/Core/Singleton.hpp>
#include <atomic>
#include <cstdint>

namespace Desert::Engine
{
    /**
     * @brief Manages frame-level synchronization and indexing.
     *
     * WRITTEN BY THE RENDER THREAD ONLY (NextFrame, Initialize), READ FROM ANY THREAD. A thread that drops the
     * last owner of a GPU object queues its deletion tagged with GetAbsoluteFrameCount (VulkanAllocator), so the
     * fields are atomics: a plain read racing NextFrame was a data race, and the ring slot it used to read could
     * name the slot whose fence had just been waited, under a frame still in flight.
     */
    class FrameManager final : public Common::Singleton<FrameManager>
    {
    public:
        void Initialize( uint32_t maxFramesInFlight )
        {
            m_MaxFramesInFlight.store( maxFramesInFlight );
            m_CurrentFrameIndex.store( 0 );
            m_AbsoluteFrameCount.store( 0 );
        }

        // A SWAPCHAIN REBUILD IS NOT A NEW START, and the rebuild used to call Initialize: the current slot
        // snapped back to 0 in the middle of a frame whose fence had been reset and whose acquire semaphore
        // had been chosen for the OLD slot, so the submit waited on a semaphore nothing would signal and
        // handed the queue a fence still signalled from an earlier frame (the reveal-time GPU timeout). The
        // absolute frame count went back to 0 too, under everything keyed on it.
        //
        // The first swapchain fixes the count; every rebuild must keep it, because the per-frame semaphores,
        // fences and command buffers were sized by it once. False means the rebuild changed the count.
        [[nodiscard]] bool AdoptSwapchainImageCount( uint32_t imageCount )
        {
            if ( !m_CountFixedBySwapchain )
            {
                Initialize( imageCount );
                m_CountFixedBySwapchain = true;
                return true;
            }
            return imageCount == m_MaxFramesInFlight.load();
        }

        /**
         * @brief Advances to the next frame. Render thread only.
         */
        void NextFrame()
        {
            m_CurrentFrameIndex.store( ( m_CurrentFrameIndex.load() + 1 ) % m_MaxFramesInFlight.load() );
            m_AbsoluteFrameCount.fetch_add( 1 );
        }

        [[nodiscard]] uint32_t GetCurrentFrameIndex() const { return m_CurrentFrameIndex.load(); }
        [[nodiscard]] uint32_t GetMaxFramesInFlight() const { return m_MaxFramesInFlight.load(); }
        /// The frame being recorded, counted from the first swapchain; never wraps (unlike the ring slot), so a
        /// deletion tagged with it is retired by one rule whatever thread queued it (VulkanAllocator::IsRetired).
        [[nodiscard]] uint64_t GetAbsoluteFrameCount() const { return m_AbsoluteFrameCount.load(); }

    private:
        std::atomic<uint32_t> m_CurrentFrameIndex{ 0 };
        std::atomic<uint32_t> m_MaxFramesInFlight{ 2 };
        std::atomic<uint64_t> m_AbsoluteFrameCount{ 0 };
        bool                  m_CountFixedBySwapchain = false;
    };

} // namespace Desert::Engine
