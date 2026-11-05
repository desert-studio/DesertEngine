#include <Engine/Graphic/API/Vulkan/VulkanFrameLoop.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanSwapChainOutput.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <Common/Core/EngineThread.hpp>
#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <utility>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // THE PROCESS'S FRAME LOOP, for DrainSubmitThread / DeferRelease: VulkanLogicalDevice::WaitIdle and the
        // transient allocator cannot reach the renderer's loop. One VkDevice, one loop. The mutex is held across
        // a drain, and the destructor unregisters under it, so a loop is never destroyed under a drain.
        std::mutex& ActiveLoopMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
        VulkanFrameLoop*  g_ActiveLoop     = nullptr; // under ActiveLoopMutex
        thread_local bool t_OnSubmitThread = false;
    } // namespace

    VulkanFrameLoop::VulkanFrameLoop( VkDevice device, uint32_t graphicsFamily,
                                      std::optional<uint32_t> computeFamily, uint32_t frameSlots )
         : m_Device( device ), m_QueueObjects( device, graphicsFamily, computeFamily, frameSlots ),
           m_Fences( std::max( 1u, frameSlots ), VK_NULL_HANDLE ), m_Work( std::max( 1u, frameSlots ) ),
           m_SlotTicket( std::max( 1u, frameSlots ), 0 )
    {
        // SIGNALLED: no frame is in flight yet, so the first wait on every slot returns at once.
        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for ( VkFence& fence : m_Fences )
        {
            if ( vkCreateFence( m_Device, &info, nullptr, &fence ) != VK_SUCCESS )
                fence = VK_NULL_HANDLE; // refused by name at BeginSlot / Submit / WaitSlot
        }
        m_Thread = Common::StartEngineThread( [this]() { ThreadMain(); } );
        const std::scoped_lock active( ActiveLoopMutex() );
        g_ActiveLoop = this;
    }

    VulkanFrameLoop::~VulkanFrameLoop()
    {
        {
            const std::scoped_lock active( ActiveLoopMutex() );
            if ( g_ActiveLoop == this )
                g_ActiveLoop = nullptr;
        }
        // Everything handed over is submitted, presented and released before a fence it signals is destroyed.
        Stop();
        for ( VkFence fence : m_Fences )
        {
            if ( fence != VK_NULL_HANDLE )
                vkDestroyFence( m_Device, fence, nullptr );
        }
    }

    void VulkanFrameLoop::Stop()
    {
        {
            const std::scoped_lock lock( m_Mutex );
            m_Stop = true;
        }
        m_Wake.notify_all();
        if ( m_Thread.joinable() )
            m_Thread.join();
    }

    Common::BoolResultStr VulkanFrameLoop::BeginSlot( uint32_t slot )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; no frame slot is begun." );
        if ( Common::BoolResultStr waited = WaitSlot( slot ); !waited )
            return waited;
        return m_QueueObjects.BeginFrameSlot( slot );
    }

    Common::BoolResultStr VulkanFrameLoop::Submit( uint32_t slot, std::vector<VulkanRdgSubmission>& frame,
                                                   const VulkanFrameOutput* output,
                                                   VulkanSwapChainOutput*   presentTo )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; nothing is submitted" );
        if ( frame.empty() || frame.back().OnPipe != RDG::Pipe::Graphics )
            return Common::MakeError( "the frame's last submission is not its graphics tail" );
        if ( slot >= m_Fences.size() || m_Fences[slot] == VK_NULL_HANDLE )
            return Common::MakeFormattedError<bool>( "frame slot {} has no fence (vkCreateFence failed)", slot );
        // The tail is a Graphics entry, so an output whose index lies inside the frame always finds its waiter.
        // Refused here, on the frame's thread, rather than by the submit thread: a frame that signals
        // RenderComplete without waiting the acquire would write an image the presentation engine may still read.
        if ( output != nullptr && output->FirstEntryAfterAcquire >= frame.size() )
            return Common::MakeFormattedError<bool>(
                 "no graphics submission at or after entry {} of {} waits the acquired image",
                 output->FirstEntryAfterAcquire, frame.size() );

        // Read HERE, on the frame's thread (ExecuteFrame says what it is for): every upload a resource of this
        // frame needed was submitted before this call, so this value covers them all.
        const CommandBufferAllocator::UploadSyncPoint uploads =
             CommandBufferAllocator::GetInstance().NewestUploadSync();

        std::unique_lock<std::mutex> lock( m_Mutex );
        // The slot's previous frame left the thread before this frame began (BeginSlot waited it); waited here as
        // well, so no path can swap storage the thread is still reading.
        WaitHandled( lock, m_SlotTicket[slot] );
        if ( !m_Failure.empty() )
            return Common::MakeFormattedError<bool>( "the submit thread failed an earlier frame: {}", m_Failure );
        SlotWork& work = m_Work[slot];
        work.Frame.swap( frame );
        frame.clear(); // the slot's earlier frame, already submitted: its capacity is reused, its entries are not
        work.Output.reset();
        work.Present.reset();
        if ( output != nullptr )
        {
            work.Output = *output;
            // Taken only once the hand-off cannot be refused any more: a refused frame keeps its image held.
            if ( presentTo != nullptr )
                work.Present = presentTo->TakePresent();
        }
        work.UploadTimeline   = uploads.Timeline;
        work.UploadValue      = uploads.Value;
        const uint64_t ticket = ++m_NextTicket;
        m_SlotTicket[slot]    = ticket;
        m_Queue.push_back( QueuedWork{ .Ticket = ticket, .Slot = slot, .Release = {} } );
        lock.unlock();
        m_Wake.notify_one();
        return Common::MakeSuccess( true );
    }

    void VulkanFrameLoop::ThreadMain()
    {
#if DESERT_DEV_INSTRUMENTS
        OPTICK_THREAD( "Vulkan submit" );
#endif
        t_OnSubmitThread = true;
        std::unique_lock<std::mutex> lock( m_Mutex );
        for ( ;; )
        {
            m_Wake.wait( lock, [this]() { return m_Stop || !m_Queue.empty(); } );
            if ( m_Queue.empty() )
                return; // stopping, and everything handed over has run
            QueuedWork work = std::move( m_Queue.front() );
            m_Queue.pop_front();
            const bool earlierFailed = !m_Failure.empty();
            lock.unlock();
            std::string failure;
            {
                // The submit thread's rows: the frame's submit and present, and the deferred releases, all off
                // the render thread.
                DESERT_PROFILE_SCOPE( "Vulkan submit" );
                if ( work.Release )
                {
                    DESERT_PROFILE_SCOPE( "Vulkan: deferred release" );
                    work.Release();
                    work.Release = nullptr; // what the release captured dies here too, on this thread
                }
                else
                {
                    failure = RunFrame( work.Slot, earlierFailed );
                }
            }
            lock.lock();
            if ( !failure.empty() && m_Failure.empty() )
                m_Failure = std::move( failure );
            m_HandledTicket = work.Ticket;
            m_Handled.notify_all();
        }
    }

    std::string VulkanFrameLoop::RunFrame( uint32_t slot, bool earlierFailed )
    {
        SlotWork&   work = m_Work[slot];
        std::string failure;
        bool        submitted = false;
        // After a failure nothing more is submitted: the failed frame's semaphores are in no known state, and
        // the next Submit / WaitSlot returns the failure, which ends the run (Application::Run).
        if ( !earlierFailed )
        {
            DESERT_PROFILE_SCOPE( "Vulkan: frame submit" );
            const Common::BoolResultStr result = ExecuteFrame( slot, work );
            submitted                          = result.IsSuccess();
            if ( !submitted )
                failure = result.GetError();
        }
        if ( work.Present )
        {
            // The present waits RenderComplete, which only the submission signals: a frame that was not
            // submitted gives its image up unpresented.
            if ( submitted )
                work.Present->Output->Present( *work.Present );
            else
                work.Present->Output->AbandonPresent( *work.Present );
            work.Present.reset();
        }
        return failure;
    }

    Common::BoolResultStr VulkanFrameLoop::ExecuteFrame( uint32_t slot, const SlotWork& work )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; nothing is submitted" );
        const std::vector<VulkanRdgSubmission>& frame  = work.Frame;
        const VulkanFrameOutput*                output = work.Output ? &*work.Output : nullptr;

        const auto device        = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        bool       waitedOnImage = false;
        // THE ASYNC-COMPUTE PIPE WAITS THE UPLOADS (UE: the async compute pipe waits on the graphics fence of
        // the work submitted before it). One-off uploads go to the graphics queue unwaited
        // (CommandBufferAllocator::RT_SubmitGraphicThenRelease); a graphics entry of this frame is ordered after
        // them by submission order, but an AsyncCompute entry on a separate compute queue is not, and the first
        // one may run before any graphics entry of the frame and read a buffer or image whose copy has not
        // landed. Read ONCE, at the hand-off on the frame's thread (Submit): every resource the frame recorded
        // had its upload submitted before that call, so the value covers them all. Only the FIRST async-compute
        // entry waits; the later ones follow it on the same queue. ALL_COMMANDS, not COMPUTE_SHADER: an async
        // segment also copies and reads indirect arguments, and at the head of the batch the wider stage costs
        // nothing.
        bool waitedOnUploads = work.UploadValue == 0; // nothing uploaded yet: nothing to wait
        // PREPARE every entry first, into storage that outlives the batches below (VkSubmitInfo and the timeline
        // info point into it). Kept across frames: an entry's vectors are re-assigned, not re-allocated.
        if ( m_Prepared.size() < frame.size() )
            m_Prepared.resize( frame.size() );
        for ( size_t i = 0; i < frame.size(); ++i )
        {
            const VulkanRdgSubmission& entry    = frame[i];
            PreparedSubmission&        prepared = m_Prepared[i];
            const bool                 last     = i + 1 == frame.size();
            prepared.Queue = entry.Queue != VK_NULL_HANDLE ? entry.Queue : device->GetGraphicsQueue();
            prepared.Waits.assign( entry.WaitSemaphores.begin(), entry.WaitSemaphores.end() );
            prepared.Stages.assign( entry.WaitStages.begin(), entry.WaitStages.end() );
            prepared.Signals.assign( entry.SignalSemaphores.begin(), entry.SignalSemaphores.end() );
            // The values of a submission that waits or signals a timeline: one per semaphore, ignored for the
            // binary ones. Starts from the entry's own (RDG-EXTERNAL points; empty when it has none).
            prepared.WaitValues.assign( entry.WaitValues.begin(), entry.WaitValues.end() );
            prepared.SignalValues.assign( entry.SignalValues.begin(), entry.SignalValues.end() );
            const bool timelines = !prepared.WaitValues.empty() || !prepared.SignalValues.empty();
            // The acquire semaphore is waited where the back buffer can first be touched: the first graphics entry
            // recorded after the late acquire. The entries before it (the scene's graphs) start without waiting on
            // the display.
            if ( output != nullptr && !waitedOnImage && entry.OnPipe == RDG::Pipe::Graphics &&
                 i >= output->FirstEntryAfterAcquire )
            {
                prepared.Waits.push_back( output->ImageAcquired );
                prepared.Stages.push_back( output->ImageFirstUse );
                if ( timelines )
                    prepared.WaitValues.push_back( 0 );
                waitedOnImage = true;
            }
            if ( !waitedOnUploads && entry.OnPipe == RDG::Pipe::AsyncCompute &&
                 prepared.Queue != device->GetGraphicsQueue() )
            {
                prepared.WaitValues.resize( prepared.Waits.size(), 0 );
                prepared.Waits.push_back( work.UploadTimeline );
                prepared.Stages.push_back( VK_PIPELINE_STAGE_ALL_COMMANDS_BIT );
                prepared.WaitValues.push_back( work.UploadValue );
                waitedOnUploads = true;
            }
            if ( last && output != nullptr )
            {
                prepared.Signals.push_back( output->RenderComplete );
                if ( timelines )
                    prepared.SignalValues.push_back( 0 );
            }
            prepared.Timeline =
                 VkTimelineSemaphoreSubmitInfo{ .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            prepared.HasTimeline = !prepared.WaitValues.empty() || !prepared.SignalValues.empty();
            if ( prepared.HasTimeline )
            {
                prepared.WaitValues.resize( prepared.Waits.size(), 0 );
                prepared.SignalValues.resize( prepared.Signals.size(), 0 );
                prepared.Timeline.waitSemaphoreValueCount = static_cast<uint32_t>( prepared.WaitValues.size() );
                prepared.Timeline.pWaitSemaphoreValues    = prepared.WaitValues.data();
                prepared.Timeline.signalSemaphoreValueCount =
                     static_cast<uint32_t>( prepared.SignalValues.size() );
                prepared.Timeline.pSignalSemaphoreValues = prepared.SignalValues.data();
            }
        }
        // Submit refused an output with no waiter already; asked again on what was actually prepared.
        if ( output != nullptr && !waitedOnImage )
            return Common::MakeFormattedError<bool>(
                 "no graphics submission at or after entry {} of {} waits the acquired image",
                 output->FirstEntryAfterAcquire, frame.size() );

        // ONE vkQueueSubmit PER RUN OF ENTRIES ON THE SAME QUEUE, not one per entry (PRESENT-COST). The frame is
        // split at every graph and every graph segment, so a frame has many entries, and each vkQueueSubmit is a
        // driver call plus a round of the device's queue mutex, which upload workers also take. A run passes its
        // entries in order as the batches of ONE call, and the batches of one call start in submission order
        // exactly as separate calls do (UE batches a queue's pending command lists into one submit the same way).
        // The run ends where the queue changes, so an entry that waits a semaphore another queue signals is still
        // submitted after the submission that signals it.
        for ( size_t first = 0; first < frame.size(); )
        {
            size_t end = first + 1;
            while ( end < frame.size() && m_Prepared[end].Queue == m_Prepared[first].Queue )
                ++end;
            const bool holdsLast = end == frame.size();

            m_SubmitInfos.clear();
            for ( size_t i = first; i < end; ++i )
            {
                const PreparedSubmission& prepared = m_Prepared[i];
                VkSubmitInfo              submitInfo{};
                submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.pNext                = prepared.HasTimeline ? &prepared.Timeline : nullptr;
                submitInfo.waitSemaphoreCount   = static_cast<uint32_t>( prepared.Waits.size() );
                submitInfo.pWaitSemaphores      = prepared.Waits.data();
                submitInfo.pWaitDstStageMask    = prepared.Stages.data();
                submitInfo.commandBufferCount   = static_cast<uint32_t>( frame[i].CommandBuffers.size() );
                submitInfo.pCommandBuffers      = frame[i].CommandBuffers.data();
                submitInfo.signalSemaphoreCount = static_cast<uint32_t>( prepared.Signals.size() );
                submitInfo.pSignalSemaphores    = prepared.Signals.data();
                m_SubmitInfos.push_back( submitInfo );
            }

            // RESET IMMEDIATELY BEFORE THE SUBMISSION THAT SIGNALS IT. WaitSlot waited this fence for the slot's
            // previous frame (after this thread had submitted it); resetting any earlier would leave a fence that
            // nothing will signal behind a frame that failed between the two. The fence goes with the run that
            // holds the frame's graphics tail: it is signalled once every batch of that call and everything
            // submitted to the queue before it has completed.
            if ( holdsLast )
            {
                // THIS RESULT USED TO GO STRAIGHT ON THE FLOOR, and it is the first line of the crash that
                // motivated the device-lost gate: "vkResetFences(): pFences[0] is in use. (a VK_ERROR_DEVICE_LOST
                // has occurred, the fence must be destroyed)". Reading it turns further illegal calls into one
                // stop.
                const VkResult reset = vkResetFences( m_Device, 1, &m_Fences[slot] );
                if ( reset != VK_SUCCESS )
                {
                    (void)NoteIfDeviceLost( reset, "vkResetFences", __FILE__, __LINE__ );
                    return Common::MakeFormattedError<bool>( "vkResetFences of frame slot {} failed: {}", slot,
                                                             VkResultToString( reset ) );
                }
            }

            const VkFence  signal    = holdsLast ? m_Fences[slot] : VK_NULL_HANDLE;
            const VkResult submitted = device->SubmitToQueue(
                 m_Prepared[first].Queue, static_cast<uint32_t>( m_SubmitInfos.size() ), m_SubmitInfos.data(),
                 signal );
            if ( submitted != VK_SUCCESS )
            {
                (void)NoteIfDeviceLost( submitted, "vkQueueSubmit", __FILE__, __LINE__ );
                // RDG-EXTERNAL: the points this run and the later entries would have signalled are signalled from
                // the host, so another API waiting on one (an enqueued OIDN filter) is released.
                for ( size_t rest = first; rest < frame.size(); ++rest )
                    m_QueueObjects.SignalOnHost( frame[rest].ExternalSignals );
                return Common::MakeFormattedError<bool>( "vkQueueSubmit of frame entries {}..{} of {} failed: {}",
                                                         first, end - 1, frame.size(),
                                                         VkResultToString( submitted ) );
            }
            first = end;
        }
        return Common::MakeSuccess( true );
    }

    void VulkanFrameLoop::WaitHandled( std::unique_lock<std::mutex>& lock, uint64_t ticket )
    {
        if ( m_HandledTicket >= ticket )
            return;
        // The frame thread waiting for the submit thread: normally zero (the slot's last hand-off was a whole
        // frame ago); a row of its own so a submit thread that falls behind is named.
        DESERT_PROFILE_WAIT( "Vulkan: wait for submit thread" );
        m_Handled.wait( lock, [this, ticket]() { return m_HandledTicket >= ticket; } );
    }

    Common::BoolResultStr VulkanFrameLoop::WaitSlot( uint32_t slot )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; no frame slot is waited." );
        if ( slot >= m_Fences.size() || m_Fences[slot] == VK_NULL_HANDLE )
            return Common::MakeFormattedError<bool>( "frame slot {} has no fence (vkCreateFence failed)", slot );

        // THE SLOT'S LAST FRAME LEFT THE SUBMIT THREAD FIRST. Its fence is reset there, immediately before its
        // submission; waited before that, the fence would still be the signalled one of the frame before and the
        // wait would return while the slot's objects are in use. A frame the thread failed is never signalled:
        // its failure is returned instead of a wait that would not end.
        {
            std::unique_lock<std::mutex> lock( m_Mutex );
            WaitHandled( lock, m_SlotTicket[slot] );
            if ( !m_Failure.empty() )
                return Common::MakeFormattedError<bool>( "the submit thread failed a frame: {}", m_Failure );
        }
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device was lost on the submit thread; no frame slot is waited." );

        // A row of its own (FPS-CPU-2): the frames-in-flight throttle is the one CPU wait every steady frame
        // may pay; it is the GPU being behind, not CPU work, and the profile must say which.
        DESERT_PROFILE_WAIT( "Vulkan: frame slot fence wait" );
        const VkResult waited = vkWaitForFences( m_Device, 1, &m_Fences[slot], VK_TRUE, UINT64_MAX );
        if ( waited != VK_SUCCESS )
        {
            // The ONE place the CPU learns that a submitted frame finished, so a device that died mid-frame
            // reports it here first and nowhere else.
            (void)NoteIfDeviceLost( waited, "vkWaitForFences", __FILE__, __LINE__ );
            return Common::MakeFormattedError<bool>( "vkWaitForFences of frame slot {} failed: {}", slot,
                                                     VkResultToString( waited ) );
        }
        return Common::MakeSuccess( true );
    }

    void VulkanFrameLoop::Drain()
    {
        if ( t_OnSubmitThread )
            return; // the thread cannot wait for itself
        std::unique_lock<std::mutex> lock( m_Mutex );
        WaitHandled( lock, m_NextTicket );
    }

    void VulkanFrameLoop::DrainSubmitThread()
    {
        if ( t_OnSubmitThread )
            return;
        const std::scoped_lock active( ActiveLoopMutex() );
        if ( g_ActiveLoop != nullptr )
            g_ActiveLoop->Drain();
    }

    bool VulkanFrameLoop::EnqueueRelease( std::function<void()>& release )
    {
        {
            const std::scoped_lock lock( m_Mutex );
            if ( m_Stop )
                return false;
            const uint64_t ticket = ++m_NextTicket;
            m_Queue.push_back( QueuedWork{ .Ticket = ticket, .Slot = 0, .Release = std::move( release ) } );
        }
        m_Wake.notify_one();
        return true;
    }

    void VulkanFrameLoop::DeferRelease( std::function<void()> release )
    {
        {
            const std::scoped_lock active( ActiveLoopMutex() );
            if ( g_ActiveLoop != nullptr && g_ActiveLoop->EnqueueRelease( release ) )
                return;
        }
        release(); // no loop runs, or it is stopping: the caller's thread pays
    }
} // namespace Desert::Graphic::API::Vulkan
