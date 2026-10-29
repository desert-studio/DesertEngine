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
        for ( size_t i = 0; i < frame.size(); ++i )
        {
            const VulkanRdgSubmission&        entry   = frame[i];
            const bool                        last    = i + 1 == frame.size();
            const VkQueue target = entry.Queue != VK_NULL_HANDLE ? entry.Queue : device->GetGraphicsQueue();
            std::vector<VkSemaphore>          waits   = entry.WaitSemaphores;
            std::vector<VkPipelineStageFlags> stages  = entry.WaitStages;
            std::vector<VkSemaphore>          signals = entry.SignalSemaphores;
            // The values of a submission that waits or signals a timeline: one per semaphore, ignored for the
            // binary ones. Starts from the entry's own (RDG-EXTERNAL points; empty when it has none).
            std::vector<uint64_t> waitValues   = entry.WaitValues;
            std::vector<uint64_t> signalValues = entry.SignalValues;
            const bool            timelines    = !waitValues.empty() || !signalValues.empty();
            if ( output != nullptr && !waitedOnImage && entry.OnPipe == RDG::Pipe::Graphics )
            {
                waits.push_back( output->ImageAcquired );
                stages.push_back( output->ImageFirstUse );
                if ( timelines )
                    waitValues.push_back( 0 );
                waitedOnImage = true;
            }
            VkTimelineSemaphoreSubmitInfo timelineInfo{ .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            if ( !waitedOnUploads && entry.OnPipe == RDG::Pipe::AsyncCompute &&
                 target != device->GetGraphicsQueue() )
            {
                waitValues.resize( waits.size(), 0 );
                waits.push_back( uploads.Timeline );
                stages.push_back( VK_PIPELINE_STAGE_ALL_COMMANDS_BIT );
                waitValues.push_back( uploads.Value );
                waitedOnUploads = true;
            }
            if ( last && output != nullptr )
            {
                signals.push_back( output->RenderComplete );
                if ( timelines )
                    signalValues.push_back( 0 );
            }
            if ( !waitValues.empty() || !signalValues.empty() )
            {
                waitValues.resize( waits.size(), 0 );
                signalValues.resize( signals.size(), 0 );
                timelineInfo.waitSemaphoreValueCount   = static_cast<uint32_t>( waitValues.size() );
                timelineInfo.pWaitSemaphoreValues      = waitValues.data();
                timelineInfo.signalSemaphoreValueCount = static_cast<uint32_t>( signalValues.size() );
                timelineInfo.pSignalSemaphoreValues    = signalValues.data();
            }

            // RESET IMMEDIATELY BEFORE THE SUBMISSION THAT SIGNALS IT. BeginSlot waited this fence for the slot's
            // previous frame; resetting any earlier would leave a fence that nothing will signal behind a frame
            // that failed between the two, and the slot's next wait would never return.
            if ( last )
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

            VkSubmitInfo submitInfo{};
            submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.pNext                = waitValues.empty() && signalValues.empty() ? nullptr : &timelineInfo;
            submitInfo.waitSemaphoreCount   = static_cast<uint32_t>( waits.size() );
            submitInfo.pWaitSemaphores      = waits.data();
            submitInfo.pWaitDstStageMask    = stages.data();
            submitInfo.commandBufferCount   = 1;
            submitInfo.pCommandBuffers      = &entry.CommandBuffer;
            submitInfo.signalSemaphoreCount = static_cast<uint32_t>( signals.size() );
            submitInfo.pSignalSemaphores    = signals.data();
            const VkResult submitted =
                 device->SubmitToQueue( target, 1, &submitInfo, last ? m_Fences[slot] : VK_NULL_HANDLE );
            if ( submitted != VK_SUCCESS )
            {
                (void)NoteIfDeviceLost( submitted, "vkQueueSubmit", __FILE__, __LINE__ );
                // RDG-EXTERNAL: the points this and the later entries would have signalled are signalled from the
                // host, so another API waiting on one (an enqueued OIDN filter) is released.
                for ( size_t rest = i; rest < frame.size(); ++rest )
                    m_QueueObjects.SignalOnHost( frame[rest].ExternalSignals );
                return Common::MakeFormattedError<bool>( "vkQueueSubmit of frame entry {} of {} failed: {}", i,
                                                         frame.size(), VkResultToString( submitted ) );
            }
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
        DESERT_PROFILE_SCOPE( "Vulkan: frame slot fence wait" );
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
