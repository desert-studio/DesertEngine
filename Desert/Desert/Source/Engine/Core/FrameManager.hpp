#pragma once

#include <Common/Core/Singleton.hpp>
#include <cstdint>

namespace Desert::Engine
{
    /**
     * @brief Manages frame-level synchronization and indexing.
     */
    class FrameManager final : public Common::Singleton<FrameManager>
    {
    public:
        void Initialize( uint32_t maxFramesInFlight )
        {
            m_MaxFramesInFlight  = maxFramesInFlight;
            m_CurrentFrameIndex  = 0;
            m_AbsoluteFrameCount = 0;
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
            return imageCount == m_MaxFramesInFlight;
        }

        /**
         * @brief Advances to the next frame.
         */
        void NextFrame()
        {
            m_CurrentFrameIndex = ( m_CurrentFrameIndex + 1 ) % m_MaxFramesInFlight;
            m_AbsoluteFrameCount++;
        }

        [[nodiscard]] uint32_t GetCurrentFrameIndex() const
        {
            return m_CurrentFrameIndex;
        }
        [[nodiscard]] uint32_t GetMaxFramesInFlight() const
        {
            return m_MaxFramesInFlight;
        }
        [[nodiscard]] uint64_t GetAbsoluteFrameCount() const
        {
            return m_AbsoluteFrameCount;
        }

    private:
        uint32_t m_CurrentFrameIndex     = 0;
        uint32_t m_MaxFramesInFlight     = 2;
        uint64_t m_AbsoluteFrameCount    = 0;
        bool     m_CountFixedBySwapchain = false;
    };

} // namespace Desert::Engine
