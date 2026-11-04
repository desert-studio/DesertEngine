#pragma once

#include <Common/Core/Singleton.hpp>
#include <Engine/Assets/FrameRetireQueue.hpp>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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
            const uint64_t frame = m_AbsoluteFrameCount.fetch_add( 1 ) + 1;
            // What RetireAfterFrames parked and no frame in flight can read any more dies here, at the frame
            // boundary (UE: the RHI's pending deletes are flushed once per frame), outside the lock.
            std::vector<std::shared_ptr<const void>> released;
            {
                std::lock_guard lock( m_RetireMutex );
                m_Retiring.TakeDue( frame, m_MaxFramesInFlight.load(), released );
            }
        }

        /**
         * @brief THE DEFERRED RELEASE OF WHAT A RECORDED FRAME MAY STILL READ. Any thread.
         *
         * UE: an RHI object whose last reference the game thread drops is not destroyed then; it goes on the
         * pending-delete list and dies once no frame in flight names it (FRHIResource deferred deletion). Here a
         * component's render caches (a procedural or edited mesh, its material instances: the draws of the
         * frame being recorded name them by raw pointer) are dropped by the interface while the render task
         * records that frame (RenderTask), and the GPU still reads the frames before it. Parked with the frame
         * that dropped them, they are released by NextFrame once FrameRetireQueue's margin has passed: the render
         * task's jobs of that frame have finished (Join precedes the submit) and its fence has been waited on.
         * Null is ignored. A destructor that runs at release may retire again (the queue is not locked then).
         */
        void RetireAfterFrames( std::shared_ptr<const void> object )
        {
            if ( !object )
                return;
            std::lock_guard lock( m_RetireMutex );
            m_Retiring.Park( std::move( object ), m_AbsoluteFrameCount.load() );
        }

        /// Releases everything RetireAfterFrames parked, now. Only for a caller that has idled the device and
        /// joined the render task (a world's teardown, the application's shutdown before the device dies).
        void ReleaseRetiredNow()
        {
            Assets::FrameRetireQueue<std::shared_ptr<const void>> released;
            {
                std::lock_guard lock( m_RetireMutex );
                std::swap( released, m_Retiring );
            }
        }

        /// THE SLOT OF THE FRAME THIS THREAD RECORDS (UE: per-frame resources are indexed by the frame being
        /// recorded, not by one global current frame). On the render task while it runs a job: the slot the job
        /// was enqueued for (RenderTask stamps it, ScopedRecordingFrame binds it), so a job keeps reading and
        /// writing its own slot after the game thread has submitted its frame and moved on (NextFrame). On every
        /// other thread, and on the render task between jobs: the game thread's slot (GetGameFrameIndex).
        [[nodiscard]] uint32_t GetCurrentFrameIndex() const
        {
            if ( t_RecordingSlot != k_NoRecordingSlot )
                return t_RecordingSlot;
            return m_CurrentFrameIndex.load();
        }
        /// The game thread's slot, whatever the calling thread records: the slot NextFrame moves and the next
        /// RenderTask::Enqueue stamps on its job.
        [[nodiscard]] uint32_t GetGameFrameIndex() const
        {
            return m_CurrentFrameIndex.load();
        }

        /// Binds @p slot as the calling thread's recording frame for the scope (RenderTask::ThreadMain, around
        /// one job). Scopes nest: the previous binding comes back at the end.
        class ScopedRecordingFrame
        {
        public:
            explicit ScopedRecordingFrame( uint32_t slot ) : m_Previous( t_RecordingSlot )
            {
                t_RecordingSlot = slot;
            }
            ~ScopedRecordingFrame()
            {
                t_RecordingSlot = m_Previous;
            }
            ScopedRecordingFrame( const ScopedRecordingFrame& )            = delete;
            ScopedRecordingFrame& operator=( const ScopedRecordingFrame& ) = delete;

        private:
            uint32_t m_Previous;
        };
        [[nodiscard]] uint32_t GetMaxFramesInFlight() const
        {
            return m_MaxFramesInFlight.load();
        }
        /// The frame being recorded, counted from the first swapchain; never wraps (unlike the ring slot), so a
        /// deletion tagged with it is retired by one rule whatever thread queued it (VulkanAllocator::IsRetired).
        [[nodiscard]] uint64_t GetAbsoluteFrameCount() const
        {
            return m_AbsoluteFrameCount.load();
        }

    private:
        static constexpr uint32_t k_NoRecordingSlot = 0xFFFFFFFFu;
        // The recording frame of this thread (ScopedRecordingFrame); k_NoRecordingSlot: the game thread's slot.
        static inline thread_local uint32_t t_RecordingSlot = k_NoRecordingSlot;

        std::atomic<uint32_t> m_CurrentFrameIndex{ 0 };
        std::atomic<uint32_t> m_MaxFramesInFlight{ 2 };
        std::atomic<uint64_t> m_AbsoluteFrameCount{ 0 };
        bool                  m_CountFixedBySwapchain = false;
        // See RetireAfterFrames.
        std::mutex                                            m_RetireMutex;
        Assets::FrameRetireQueue<std::shared_ptr<const void>> m_Retiring;
    };

} // namespace Desert::Engine
