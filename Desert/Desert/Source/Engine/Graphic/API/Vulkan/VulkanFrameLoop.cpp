#include <Engine/Graphic/API/Vulkan/VulkanFrameLoop.hpp>

#include <Engine/Core/EngineContext.hpp>
#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <Common/Core/Profiler.hpp>

#include <algorithm>

namespace Desert::Graphic::API::Vulkan
{
    VulkanFrameLoop::VulkanFrameLoop( VkDevice device, uint32_t graphicsFamily,
                                      std::optional<uint32_t> computeFamily, uint32_t frameSlots )
         : m_Device( device ), m_QueueObjects( device, graphicsFamily, computeFamily, frameSlots ),
           m_Fences( std::max( 1u, frameSlots ), VK_NULL_HANDLE )
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
    }

    VulkanFrameLoop::~VulkanFrameLoop()
    {
        for ( VkFence fence : m_Fences )
        {
            if ( fence != VK_NULL_HANDLE )
                vkDestroyFence( m_Device, fence, nullptr );
        }
    }

    Common::BoolResultStr VulkanFrameLoop::BeginSlot( uint32_t slot )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; no frame slot is begun." );
        if ( Common::BoolResultStr waited = WaitSlot( slot ); !waited )
            return waited;
        return m_QueueObjects.BeginFrameSlot( slot );
    }

    Common::BoolResultStr VulkanFrameLoop::Submit( uint32_t slot, std::span<const VulkanRdgSubmission> frame,
                                                   const VulkanFrameOutput* output )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; nothing is submitted" );
        if ( frame.empty() || frame.back().OnPipe != RDG::Pipe::Graphics )
            return Common::MakeError( "the frame's last submission is not its graphics tail" );
        if ( slot >= m_Fences.size() || m_Fences[slot] == VK_NULL_HANDLE )
            return Common::MakeFormattedError<bool>( "frame slot {} has no fence (vkCreateFence failed)", slot );

        const auto device        = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() );
        bool       waitedOnImage = false;
        // THE ASYNC-COMPUTE PIPE WAITS THE UPLOADS (UE: the async compute pipe waits on the graphics fence of
        // the work submitted before it). One-off uploads go to the graphics queue unwaited
        // (CommandBufferAllocator::RT_SubmitGraphicThenRelease); a graphics entry of this frame is ordered after
        // them by submission order, but an AsyncCompute entry on a separate compute queue is not, and the first
        // one may run before any graphics entry of the frame and read a buffer or image whose copy has not
        // landed. Read ONCE, here: every resource the frame recorded had its upload submitted before this call,
        // so this value covers them all. Only the FIRST async-compute entry waits; the later ones follow it on
        // the same queue. ALL_COMMANDS, not COMPUTE_SHADER: an async segment also copies and reads indirect
        // arguments, and at the head of the batch the wider stage costs nothing.
        const CommandBufferAllocator::UploadSyncPoint uploads =
             CommandBufferAllocator::GetInstance().NewestUploadSync();
        bool waitedOnUploads = uploads.Value == 0; // nothing uploaded yet: nothing to wait
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
            if ( output != nullptr && !waitedOnImage && entry.OnPipe == RDG::Pipe::Graphics )
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
                prepared.Waits.push_back( uploads.Timeline );
                prepared.Stages.push_back( VK_PIPELINE_STAGE_ALL_COMMANDS_BIT );
                prepared.WaitValues.push_back( uploads.Value );
                waitedOnUploads = true;
            }
            if ( last && output != nullptr )
            {
                prepared.Signals.push_back( output->RenderComplete );
                if ( timelines )
                    prepared.SignalValues.push_back( 0 );
            }
            prepared.Timeline    = VkTimelineSemaphoreSubmitInfo{ .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            prepared.HasTimeline = !prepared.WaitValues.empty() || !prepared.SignalValues.empty();
            if ( prepared.HasTimeline )
            {
                prepared.WaitValues.resize( prepared.Waits.size(), 0 );
                prepared.SignalValues.resize( prepared.Signals.size(), 0 );
                prepared.Timeline.waitSemaphoreValueCount   = static_cast<uint32_t>( prepared.WaitValues.size() );
                prepared.Timeline.pWaitSemaphoreValues      = prepared.WaitValues.data();
                prepared.Timeline.signalSemaphoreValueCount = static_cast<uint32_t>( prepared.SignalValues.size() );
                prepared.Timeline.pSignalSemaphoreValues    = prepared.SignalValues.data();
            }
        }

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
                submitInfo.commandBufferCount   = 1;
                submitInfo.pCommandBuffers      = &frame[i].CommandBuffer;
                submitInfo.signalSemaphoreCount = static_cast<uint32_t>( prepared.Signals.size() );
                submitInfo.pSignalSemaphores    = prepared.Signals.data();
                m_SubmitInfos.push_back( submitInfo );
            }

            // RESET IMMEDIATELY BEFORE THE SUBMISSION THAT SIGNALS IT. BeginSlot waited this fence for the slot's
            // previous frame; resetting any earlier would leave a fence that nothing will signal behind a frame
            // that failed between the two, and the slot's next wait would never return. The fence goes with the
            // run that holds the frame's graphics tail: it is signalled once every batch of that call and
            // everything submitted to the queue before it has completed.
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

            const VkResult submitted =
                 device->SubmitToQueue( m_Prepared[first].Queue, static_cast<uint32_t>( m_SubmitInfos.size() ),
                                        m_SubmitInfos.data(), holdsLast ? m_Fences[slot] : VK_NULL_HANDLE );
            if ( submitted != VK_SUCCESS )
            {
                (void)NoteIfDeviceLost( submitted, "vkQueueSubmit", __FILE__, __LINE__ );
                // RDG-EXTERNAL: the points this run and the later entries would have signalled are signalled from
                // the host, so another API waiting on one (an enqueued OIDN filter) is released.
                for ( size_t rest = first; rest < frame.size(); ++rest )
                    m_QueueObjects.SignalOnHost( frame[rest].ExternalSignals );
                return Common::MakeFormattedError<bool>( "vkQueueSubmit of frame entries {}..{} of {} failed: {}",
                                                         first, end - 1, frame.size(), VkResultToString( submitted ) );
            }
            first = end;
        }
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanFrameLoop::WaitSlot( uint32_t slot )
    {
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError( "the device is lost; no frame slot is waited." );
        if ( slot >= m_Fences.size() || m_Fences[slot] == VK_NULL_HANDLE )
            return Common::MakeFormattedError<bool>( "frame slot {} has no fence (vkCreateFence failed)", slot );

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
} // namespace Desert::Graphic::API::Vulkan
