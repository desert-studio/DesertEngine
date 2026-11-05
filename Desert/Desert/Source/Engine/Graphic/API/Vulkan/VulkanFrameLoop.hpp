#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRdgQueues.hpp>

#include <vulkan/vulkan.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanSwapChainOutput;

    // What a window adds to a frame (VulkanSwapChainOutput::GetFrameOutput): the first Graphics submission at or
    // after FirstEntryAfterAcquire waits ImageAcquired at ImageFirstUse — the stage the back buffer is first
    // written — and the frame's last submission signals RenderComplete, which the output's present waits. The
    // image is acquired late (at the back-buffer import, PRESENT-COST), so the entries before
    // FirstEntryAfterAcquire were recorded before its index existed and cannot touch it: they run without waiting
    // on the display. A frame with no window, or one that holds no image, has none.
    struct VulkanFrameOutput
    {
        VkSemaphore          ImageAcquired  = VK_NULL_HANDLE;
        VkPipelineStageFlags ImageFirstUse  = 0; // RDG::kPresentAcquiredState's stages, set by the output
        VkSemaphore          RenderComplete = VK_NULL_HANDLE;
        // Index into the frame's submissions at the acquire (VulkanRendererAPI::ImportBackBuffer); 0 = the first.
        size_t FirstEntryAfterAcquire = 0;
    };

    // A frame's present as the submit thread runs it (VK-SUBMIT-THREAD), captured on the frame's own thread at
    // the hand-off (VulkanSwapChainOutput::TakePresent): the image index and the semaphore the present waits
    // are the frame's, so the submit thread reads nothing the next frame's acquire changes.
    struct VulkanPresentRequest
    {
        VulkanSwapChainOutput* Output         = nullptr;
        uint32_t               ImageIndex     = 0;
        VkSemaphore            RenderComplete = VK_NULL_HANDLE;
    };

    // THE FRAME'S ONE SUBMISSION PATH, owned by the frame loop and not by a window. Per frame slot: the fence the
    // slot's last submission signals, and the slot's graph objects (VulkanRdgQueueObjects: command pools and
    // semaphores). Every frame is begun here (BeginSlot), submitted here (Submit) and waited here (WaitSlot); a
    // window is only an optional VulkanFrameOutput of Submit, so a frame with no window (EngineHost) is the same
    // frame. CPU/GPU sync is the slot fence, never a device-wide wait.
    // Never: begin a slot's objects before its fence was waited; submit a frame whose last entry is not its
    // graphics tail; reset a slot fence anywhere but immediately before the submission that signals it.
    //
    // THE SUBMIT THREAD (VK-SUBMIT-THREAD; UE 5.x: the RHI submission thread). The frame's thread does not call
    // vkQueueSubmit or vkQueuePresentKHR: Submit hands the slot's finished frame (its submissions, the window's
    // output and the captured present) to one thread the loop owns, and returns. That thread submits and
    // presents in hand-off order, and runs the releases the frame threads deferred to it (DeferRelease). The
    // rules that keep it correct:
    //   - a slot's fence is waited (WaitSlot) only after the thread has submitted the slot's last frame, so the
    //     wait never returns on the fence of the frame before (the reset happens on the submit thread);
    //   - every VkQueue call still goes through the device's queue lock (VulkanLogicalDevice::SubmitToQueue,
    //     PresentToQueue), so a queue is used by one thread at a time whichever thread that is;
    //   - the swapchain is externally synchronised between the frame thread's acquire and this thread's
    //     present by VulkanSwapChainOutput (an acquire waits until the previous present was issued);
    //   - a failure the thread meets is kept and returned by the next Submit / WaitSlot (the run ends there),
    //     a lost device by the DeviceLost latch; nothing waits forever on a frame that was never submitted;
    //   - a swapchain rebuild, a device-idle wait and the teardown drain the thread first (Drain, and
    //     DrainSubmitThread from VulkanLogicalDevice::WaitIdle), and the destructor joins it;
    //   - EVERY ONE-OFF SUBMISSION (CommandBufferAllocator: the unwaited uploads and the blocking flushes, on any
    //     queue) is ordered work of this thread while it runs (SubmitOrdered), FIFO with the frames: an upload
    //     a frame thread makes for frame N+1 reaches the queue after frame N's draws, as it did when the frame
    //     thread submitted N itself, and the upload timeline value is picked in that same queue order;
    //   - the destructor drains and joins the thread (Stop) BEFORE it unregisters the loop, and no lock is held
    //     across a drain: a release or an upload made during the teardown is still run in order, and nothing
    //     queued here outlives the loop (it is destroyed before the VMA allocator, VulkanContext::Shutdown).
    class VulkanFrameLoop
    {
    public:
        VulkanFrameLoop( VkDevice device, uint32_t graphicsFamily, std::optional<uint32_t> computeFamily,
                         uint32_t frameSlots );
        ~VulkanFrameLoop();
        VulkanFrameLoop( const VulkanFrameLoop& )            = delete;
        VulkanFrameLoop& operator=( const VulkanFrameLoop& ) = delete;

        // Waits for the slot's previous frame, then begins the slot's graph objects (their command buffers and
        // semaphores become reusable).
        Common::BoolResultStr BeginSlot( uint32_t slot );
        // HANDS THE FRAME TO THE SUBMIT THREAD, which submits it IN ORDER (RDG-CONTRACTS B(3)): the frame command
        // buffer split at every graph and the graphs' segment submissions. The last entry (the frame's graphics
        // tail) signals the slot fence, which covers every AsyncCompute entry too (each one is joined by a later
        // Graphics entry). With @p output, the first Graphics entry from output->FirstEntryAfterAcquire on also
        // waits for the acquired image and the last one signals render-complete; with @p presentTo as well, the
        // held image is taken (VulkanSwapChainOutput::TakePresent) and the thread presents it after the submit.
        // @p frame is swapped with the slot's storage and comes back empty with the capacity of an earlier
        // frame, so a steady frame allocates nothing. Refused (nothing handed over, the image still held) on a
        // lost device, a malformed frame or a failure the thread met on an earlier frame.
        Common::BoolResultStr Submit( uint32_t slot, std::vector<VulkanRdgSubmission>& frame,
                                      const VulkanFrameOutput* output, VulkanSwapChainOutput* presentTo );
        // Waits for the slot's last frame: first until the submit thread has submitted it, then its fence (a
        // slot never submitted is signalled).
        Common::BoolResultStr WaitSlot( uint32_t slot );
        // Returns once the submit thread has run everything handed to it before the call (frames, presents,
        // deferred releases). Not a device wait: the GPU may still be executing the frames.
        void Drain();

        // THE PROCESS'S SUBMIT THREAD, for code that cannot reach the frame loop (one VkDevice, one loop):
        // VulkanLogicalDevice::WaitIdle and the swapchain rebuild drain it. A no-op with no loop running, and
        // on the submit thread itself. The registry lock is not held across the drain (a lease is).
        static void DrainSubmitThread();
        // Runs @p release on the submit thread after everything handed to it so far: for destruction a frame
        // thread has proven safe (the resources' last frame's fence was waited) but should not pay for
        // (vkDestroy*, vmaFreeMemory). Inline when no loop runs or its thread has exited (everything handed to
        // it already ran, so inline is still after it).
        static void DeferRelease( std::function<void()> release );
        // A ONE-OFF vkQueueSubmit IN QUEUE ORDER (UE: the RHI submission thread takes the uploads as well):
        // @p submit runs on the submit thread after everything handed to it so far, and the caller blocks until
        // it ran and gets its result. When no loop runs, on the submit thread itself, and when the loop's
        // thread has exited or failed a frame, it runs inline on the caller, after everything handed over was
        // handled. @p submit must not wait for the GPU (the caller waits its fence afterwards).
        static VkResult SubmitOrdered( const std::function<VkResult()>& submit );

        VulkanRdgQueueObjects& GetQueueObjects()
        {
            return m_QueueObjects;
        }

    private:
        // One slot's frame as the submit thread runs it. Written by the frame thread at Submit and read by the
        // submit thread until the hand-off's ticket is handled; the slot is not handed over again before that
        // (BeginSlot -> WaitSlot, and Submit waits it as well), so the two never touch it at once.
        struct SlotWork
        {
            std::vector<VulkanRdgSubmission>    Frame;
            std::optional<VulkanFrameOutput>    Output;
            std::optional<VulkanPresentRequest> Present;
        };
        // One entry of the submit thread's queue, in hand-off order: a slot's frame, a deferred release or a
        // one-off submission.
        struct QueuedWork
        {
            uint64_t              Ticket = 0;
            uint32_t              Slot   = 0;
            std::function<void()> Release; // set: a DeferRelease entry
            // Set: a SubmitOrdered entry. Both live on the caller's stack, which blocks until the ticket is
            // handled; the thread writes *Result before it marks the ticket handled (under m_Mutex).
            const std::function<VkResult()>* Ordered = nullptr;
            VkResult*                        Result  = nullptr;
            // Neither set: the slot's frame.
        };

        void ThreadMain();
        // The submit thread's half of one frame: the submission, then the present (or, when the submission
        // failed, the present given up). Empty on success, else the failure Submit / WaitSlot return next.
        std::string RunFrame( uint32_t slot, bool earlierFailed );
        // vkResetFences + vkQueueSubmit of one frame, on the submit thread.
        Common::BoolResultStr ExecuteFrame( uint32_t slot, const SlotWork& work );
        // Under m_Mutex (@p lock): returns once the submit thread has handled @p ticket.
        void WaitHandled( std::unique_lock<std::mutex>& lock, uint64_t ticket );
        // Queues @p release unless the thread has exited; false: the caller runs it.
        bool EnqueueRelease( std::function<void()>& release );
        // SubmitOrdered on this loop (not on its submit thread): queued and waited, or inline (see there).
        VkResult RunOrdered( const std::function<VkResult()>& submit );
        // Drains and joins the submit thread: everything queued before the thread exits runs (the destructor,
        // before the loop is unregistered; idempotent).
        void Stop();

        // One frame entry as vkQueueSubmit gets it (ExecuteFrame): the entry's waits and signals plus the window's
        // and the uploads', with the timeline info that points into them.
        struct PreparedSubmission
        {
            VkQueue                           Queue = VK_NULL_HANDLE;
            std::vector<VkSemaphore>          Waits;
            std::vector<VkPipelineStageFlags> Stages;
            std::vector<VkSemaphore>          Signals;
            std::vector<uint64_t>             WaitValues;
            std::vector<uint64_t>             SignalValues;
            VkTimelineSemaphoreSubmitInfo Timeline{ .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            bool                          HasTimeline = false;
        };

        VkDevice              m_Device = VK_NULL_HANDLE;
        VulkanRdgQueueObjects m_QueueObjects;
        std::vector<VkFence>  m_Fences; // per slot; VK_NULL_HANDLE when its creation failed (refused by name)
        // ExecuteFrame's per-frame storage, kept across frames so a steady frame allocates nothing to submit.
        // The submit thread's alone.
        std::vector<PreparedSubmission> m_Prepared;
        std::vector<VkSubmitInfo>       m_SubmitInfos;
        std::vector<SlotWork>           m_Work; // per slot

        // THE HAND-OFF, all of it under m_Mutex. Tickets number the queue's entries from 1; m_HandledTicket is
        // the last one the thread finished, m_SlotTicket the ticket of each slot's last frame.
        std::mutex              m_Mutex;
        std::condition_variable m_Wake;    // the submit thread: work arrived, or stop
        std::condition_variable m_Handled; // the frame threads: a ticket was handled
        std::deque<QueuedWork>  m_Queue;
        std::vector<uint64_t>   m_SlotTicket;
        uint64_t                m_NextTicket    = 0;
        uint64_t                m_HandledTicket = 0;
        std::string             m_Failure; // the first failure the submit thread met; empty while none
        bool                    m_Stop = false;
        // Set by the submit thread as it returns, under m_Mutex and only with m_Queue empty: from then on
        // nothing is queued (EnqueueRelease, RunOrdered and Submit check it), so no entry is left unrun.
        bool                    m_Exited = false;
        std::thread             m_Thread; // declared last: started once everything above exists
    };
} // namespace Desert::Graphic::API::Vulkan
