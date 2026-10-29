#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <Common/Core/Profiler.hpp>

#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    CommandBufferAllocator::CommandBufferAllocator( const std::shared_ptr<VulkanLogicalDevice>& device )
         : m_GraphicsFamily( device->GetPhysicalDevice()->GetGraphicsFamily() ),
           m_ComputeFamily( device->GetPhysicalDevice()->GetComputeFamily() ),
           m_TransferOpsFamily( device->GetPhysicalDevice()->GetTransferFamily() ),
           m_GraphicsQueue( device->m_GraphicsQueue ), m_ComputeQueue( device->m_ComputeQueue ),
           m_TransferOpsQueue( device->m_TransferQueue ), m_LogicalDevice( device->m_LogicalDevice ),
           m_Device( device.get() )
    {
        // The upload timeline (NewestUploadSync). timelineSemaphore is a Required device capability
        // (DeviceCaps.cpp), so a refusal here is a driver failure: logged, and every graphics one-off submit
        // then refuses by name rather than leaving another queue unordered behind the uploads.
        VkSemaphoreTypeCreateInfo timeline{ .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                            .pNext         = nullptr,
                                            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                            .initialValue  = 0 };
        const VkSemaphoreCreateInfo info{
             .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timeline, .flags = 0 };
        if ( const VkResult created = vkCreateSemaphore( m_LogicalDevice, &info, nullptr, &m_UploadTimeline );
             created != VK_SUCCESS )
        {
            m_UploadTimeline = VK_NULL_HANDLE;
            LOG_ERROR( "[CommandBuffers] vkCreateSemaphore (upload timeline) failed: {}; graphics one-off "
                       "submissions will be refused.",
                       VkResultToString( created ) );
        }
    }

    CommandBufferAllocator::UploadSyncPoint CommandBufferAllocator::NewestUploadSync() const
    {
        return UploadSyncPoint{ .Timeline = m_UploadTimeline,
                                .Value    = m_UploadSubmittedValue.load( std::memory_order_acquire ) };
    }

    Common::ResultStr<VkCommandBuffer> CommandBufferAllocator::AllocateOneShot( uint32_t family, bool begin )
    {
        // A REFUSAL HERE IS SAFE ONLY BECAUSE EVERY CALLER NOW READS IT. `Common::ResultStr::GetValue()`
        // hands back a default-constructed T on a failed result, which for a VkCommandBuffer is
        // VK_NULL_HANDLE — and recording into VK_NULL_HANDLE is undefined behaviour, not a no-op.
        if ( !Graphic::DeviceLost::AllowWork() )
            return Common::MakeError<VkCommandBuffer>( "the device is lost; no command buffer is allocated." );

        const std::thread::id self = std::this_thread::get_id();
        OneShotPool*          pool = nullptr;
        // Buffers other threads released into this thread's pool; freed below by this, the owning thread.
        std::vector<VkCommandBuffer> pendingFree;
        {
            const std::scoped_lock pools( m_PoolMutex );
            auto&                  slot = m_ThreadPools[{ self, family }];
            if ( !slot )
            {
                // TRANSIENT: every buffer from this pool is recorded once, submitted once and freed.
                const VkCommandPoolCreateInfo poolInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                                           .pNext = nullptr,
                                                           .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                                           .queueFamilyIndex = family };
                VkCommandPool                 handle   = VK_NULL_HANDLE;
                if ( const VkResult created = vkCreateCommandPool( m_LogicalDevice, &poolInfo, nullptr, &handle );
                     created != VK_SUCCESS )
                {
                    m_ThreadPools.erase( std::pair{ self, family } );
                    return Common::MakeFormattedError<VkCommandBuffer>(
                         "vkCreateCommandPool for queue family {} failed: {}", family,
                         VkResultToString( created ) );
                }
                slot = std::make_unique<OneShotPool>( OneShotPool{ .Handle = handle, .Owner = self } );
            }
            pool = slot.get();
            pendingFree.swap( pool->PendingFree );
        }

        // Outside the lock: only this thread ever calls into this pool, which is the synchronisation the
        // spec asks for. m_PoolMutex covers the bookkeeping, never Vulkan work on a pool.
        if ( !pendingFree.empty() )
            vkFreeCommandBuffers( m_LogicalDevice, pool->Handle, static_cast<uint32_t>( pendingFree.size() ),
                                  pendingFree.data() );

        const VkCommandBufferAllocateInfo allocateInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                           .pNext = nullptr,
                                                           .commandPool        = pool->Handle,
                                                           .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                           .commandBufferCount = 1 };
        VkCommandBuffer                   commandBuffer = VK_NULL_HANDLE;
        VK_RETURN_RESULT_IF_FALSE_TYPE(
             VkCommandBuffer, vkAllocateCommandBuffers( m_LogicalDevice, &allocateInfo, &commandBuffer ) );

        if ( begin )
        {
            const VkCommandBufferBeginInfo beginInfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                         .pNext = nullptr,
                                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                                         .pInheritanceInfo = nullptr };
            if ( const VkResult begun = vkBeginCommandBuffer( commandBuffer, &beginInfo ); begun != VK_SUCCESS )
            {
                vkFreeCommandBuffers( m_LogicalDevice, pool->Handle, 1, &commandBuffer );
                return Common::MakeFormattedError<VkCommandBuffer>( "vkBeginCommandBuffer failed: {}",
                                                                    VkResultToString( begun ) );
            }
        }

        const std::scoped_lock pools( m_PoolMutex );
        m_OneShotPools[commandBuffer] = pool;
        return Common::MakeSuccess( commandBuffer );
    }

    void CommandBufferAllocator::ReleaseOneShot( OneShotPool& pool, VkCommandBuffer commandBuffer )
    {
        // Owner is immutable, so comparing it needs no lock. The owner is inside this call, so it is not
        // recording into another buffer of the pool: freeing here is synchronised by construction.
        if ( pool.Owner == std::this_thread::get_id() )
        {
            vkFreeCommandBuffers( m_LogicalDevice, pool.Handle, 1, &commandBuffer );
            return;
        }
        // The owner may be recording into this pool right now; calling into it from here is exactly the race
        // this class exists to prevent. The owner frees the buffer on its next allocation, or Destroy() takes
        // it with the pool.
        const std::scoped_lock pools( m_PoolMutex );
        pool.PendingFree.push_back( commandBuffer );
    }

    Common::ResultStr<VkCommandBuffer>
    CommandBufferAllocator::RT_GetCommandBufferCompute( bool begin /*= false */ )
    {
        return AllocateOneShot( m_ComputeFamily, begin );
    }

    Common::ResultStr<VkCommandBuffer>
    CommandBufferAllocator::RT_AllocateCommandBufferGraphic( bool begin /*= false */ )
    {
        return AllocateOneShot( m_GraphicsFamily, begin );
    }

    Common::ResultStr<VkCommandBuffer>
    CommandBufferAllocator::RT_AllocateCommandBufferTransferOps( bool begin /*= false */ )
    {
        return AllocateOneShot( m_TransferOpsFamily, begin );
    }

    const char* CommandBufferAllocator::WaitSiteName( const std::source_location& where )
    {
        std::string_view file = where.file_name();
        if ( const size_t slash = file.find_last_of( "/\\" ); slash != std::string_view::npos )
            file.remove_prefix( slash + 1 );
        std::string            name = std::format( "FlushOneShot wait: {}:{}", file, where.line() );
        const std::scoped_lock pools( m_PoolMutex );
        return m_WaitSites.insert( std::move( name ) ).first->c_str();
    }

    Common::ResultStr<VkResult> CommandBufferAllocator::FlushOneShot( VkCommandBuffer             commandBuffer,
                                                                      VkQueue                     queue,
                                                                      const std::source_location& where )
    {
        OneShotPool* pool = nullptr;
        {
            const std::scoped_lock pools( m_PoolMutex );
            const auto             entry = m_OneShotPools.find( commandBuffer );
            if ( entry == m_OneShotPools.end() )
            {
                // A buffer this allocator did not hand out, or one already flushed. Freeing it against a
                // guessed pool is undefined; saying so is the only honest answer, and it names the handle.
                //
                // LOGGED AS WELL AS RETURNED, and that is not belt-and-braces: most call sites throw this
                // result away, so a refusal that only travelled in the return value would be a dropped
                // upload with nothing anywhere to say it happened.
                LOG_ERROR( "[CommandBuffers] flush of command buffer {} refused: it was not allocated here, or "
                           "was already flushed. Nothing was submitted.",
                           static_cast<const void*>( commandBuffer ) );
                return Common::MakeFormattedError<VkResult>(
                     "command buffer {} was not allocated by CommandBufferAllocator (or was already flushed); "
                     "it cannot be submitted and freed here.",
                     static_cast<const void*>( commandBuffer ) );
            }
            pool = entry->second;
            m_OneShotPools.erase( entry );
        }

        // EVERY PATH BELOW RELEASES THE BUFFER: its row is gone from m_OneShotPools, so nothing else would.
        // On a refusal the buffer was never submitted, so it is not pending and releasing it is legal; on
        // success the fence wait has proved the GPU is done with it.
        const auto refuse = [&]( std::string reason, VkFence fence ) -> Common::ResultStr<VkResult>
        {
            if ( fence != VK_NULL_HANDLE )
                vkDestroyFence( m_LogicalDevice, fence, nullptr );
            ReleaseOneShot( *pool, commandBuffer );
            return Common::MakeError<VkResult>( std::move( reason ) );
        };

        // This is a SUBMIT AND A BLOCKING WAIT — the one-off upload path every texture, mesh and mipmap chain
        // goes through. On a lost device the submit cannot execute and the wait cannot ever be signalled.
        if ( !Graphic::DeviceLost::AllowWork() )
            return refuse( "the device is lost; the one-off command buffer is dropped rather than submitted.",
                           VK_NULL_HANDLE );

        if ( const VkResult ended = vkEndCommandBuffer( commandBuffer ); ended != VK_SUCCESS )
            return refuse( std::format( "vkEndCommandBuffer failed: {}", VkResultToString( ended ) ),
                           VK_NULL_HANDLE );

        const VkFenceCreateInfo fenceInfo = {
             .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0 };
        VkFence fence = VK_NULL_HANDLE;
        if ( const VkResult created = vkCreateFence( m_LogicalDevice, &fenceInfo, nullptr, &fence );
             created != VK_SUCCESS )
            return refuse( std::format( "vkCreateFence failed: {}", VkResultToString( created ) ),
                           VK_NULL_HANDLE );

        // ORDERING ACROSS QUEUES. An upload is submitted to the graphics queue and not waited for
        // (RT_SubmitGraphicThenRelease); a later submit to the SAME queue is ordered after it by submission
        // order, but a one-off on the compute or transfer queue is not, and may read the resource before its
        // copy lands. UE orders such a reader behind the upload's sync point: this submit waits the upload
        // timeline on the GPU (NewestUploadSync), the same wait the frame's async-compute pipe makes. Same
        // VkQueue (one family, as on MoltenVK): submission order already covers it.
        const UploadSyncPoint      uploads    = NewestUploadSync();
        const bool                 waitUpload = queue != m_GraphicsQueue && uploads.Value != 0;
        const VkPipelineStageFlags waitStage  = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        const VkTimelineSemaphoreSubmitInfo timelineInfo{ .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                          .pNext = nullptr,
                                                          .waitSemaphoreValueCount   = 1,
                                                          .pWaitSemaphoreValues      = &uploads.Value,
                                                          .signalSemaphoreValueCount = 0,
                                                          .pSignalSemaphoreValues    = nullptr };
        const VkSubmitInfo submitInfo = { .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                          .pNext                = waitUpload ? &timelineInfo : nullptr,
                                          .waitSemaphoreCount   = waitUpload ? 1u : 0u,
                                          .pWaitSemaphores      = waitUpload ? &uploads.Timeline : nullptr,
                                          .pWaitDstStageMask    = waitUpload ? &waitStage : nullptr,
                                          .commandBufferCount   = 1,
                                          .pCommandBuffers      = &commandBuffer,
                                          .signalSemaphoreCount = 0,
                                          .pSignalSemaphores    = nullptr };
        if ( const VkResult submitted = m_Device->SubmitToQueue( queue, 1, &submitInfo, fence );
             submitted != VK_SUCCESS )
            return refuse( std::format( "vkQueueSubmit failed: {}", VkResultToString( submitted ) ), fence );

        {
            // The x column of these rows is the number of one-off CPU waits a frame (all of them, and this
            // call site's); a frame's budget is zero.
            DESERT_PROFILE_SCOPE( "Vulkan: FlushOneShot fence wait" );
#if DESERT_DEV_INSTRUMENTS
            const char* const site = WaitSiteName( where );
            DESERT_PROFILE_SCOPE_DYNAMIC( site );
#else
            (void)where;
#endif
            VK_CHECK_RESULT( vkWaitForFences( m_LogicalDevice, 1, &fence, VK_TRUE, UINT64_MAX ) );
        }
        vkDestroyFence( m_LogicalDevice, fence, nullptr );
        ReleaseOneShot( *pool, commandBuffer );
        return Common::MakeSuccess( VK_SUCCESS );
    }

    Common::ResultStr<VkResult>
    CommandBufferAllocator::RT_FlushCommandBufferCompute( VkCommandBuffer commandBuffer, std::source_location where )
    {
        return FlushOneShot( commandBuffer, m_ComputeQueue, where );
    }

    Common::ResultStr<VkResult>
    CommandBufferAllocator::RT_FlushCommandBufferGraphic( VkCommandBuffer commandBuffer, std::source_location where )
    {
        return FlushOneShot( commandBuffer, m_GraphicsQueue, where );
    }

    Common::ResultStr<VkResult>
    CommandBufferAllocator::RT_FlushCommandBufferTransferOps( VkCommandBuffer commandBuffer, std::source_location where )
    {
        return FlushOneShot( commandBuffer, m_TransferOpsQueue, where );
    }

    Common::ResultStr<CommandBufferAllocator::Submitted>
    CommandBufferAllocator::RT_SubmitCommandBufferGraphic( VkCommandBuffer commandBuffer )
    {
        // m_OneShotPools is shared with the threads that upload (see m_PoolMutex): the lookup and the erase run
        // under it; the buffer itself is released through ReleaseOneShot, to its owning thread's pool.
        Submitted submitted{};
        {
            const std::scoped_lock pools( m_PoolMutex );
            const auto             entry = m_OneShotPools.find( commandBuffer );
            if ( entry == m_OneShotPools.end() )
                return Common::MakeFormattedError<Submitted>(
                     "command buffer {} was not allocated by CommandBufferAllocator (or was already submitted); "
                     "it cannot be submitted here.",
                     static_cast<const void*>( commandBuffer ) );
            submitted = Submitted{ .Buffer = commandBuffer, .Pool = entry->second };
            m_OneShotPools.erase( entry );
        }

        // EVERY REFUSAL BELOW RELEASES THE BUFFER. Its row is gone from m_OneShotPools, so nothing else would
        // ever free it: a refusal that only returned would leak the buffer (and the fence, once created)
        // for the life of its pool. A buffer that was never submitted is not pending, so releasing it is legal.
        const auto refuse = [&]( const std::string& reason ) -> Common::ResultStr<Submitted>
        {
            if ( submitted.Fence != VK_NULL_HANDLE )
                vkDestroyFence( m_LogicalDevice, submitted.Fence, nullptr );
            ReleaseOneShot( *submitted.Pool, submitted.Buffer );
            return Common::MakeError<Submitted>( reason );
        };
        if ( !Graphic::DeviceLost::AllowWork() )
            return refuse( "the device is lost; the one-off command buffer is dropped rather than submitted." );
        if ( m_UploadTimeline == VK_NULL_HANDLE )
            return refuse( "the upload timeline semaphore was never created (see the startup error); a graphics "
                           "one-off would be unordered against the compute and transfer queues." );

        if ( const VkResult ended = vkEndCommandBuffer( commandBuffer ); ended != VK_SUCCESS )
            return refuse( std::format( "vkEndCommandBuffer failed: {}", VkResultToString( ended ) ) );
        const VkFenceCreateInfo fenceInfo = {
             .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0 };
        if ( const VkResult created = vkCreateFence( m_LogicalDevice, &fenceInfo, nullptr, &submitted.Fence );
             created != VK_SUCCESS )
        {
            submitted.Fence = VK_NULL_HANDLE;
            return refuse( std::format( "vkCreateFence failed: {}", VkResultToString( created ) ) );
        }
        // SIGNALS THE UPLOAD TIMELINE (NewestUploadSync) with the next value. The pick and the submit are one
        // step under m_UploadSubmitMutex, so values reach the queue in increasing order whichever thread
        // uploads; the value is published only once the submit returned, so no reader waits on a value whose
        // signal was never submitted. A refused submit publishes nothing, and the next one reuses the value.
        const std::scoped_lock ordered( m_UploadSubmitMutex );
        const uint64_t         value = m_UploadSubmittedValue.load( std::memory_order_relaxed ) + 1;
        const VkTimelineSemaphoreSubmitInfo timelineInfo{ .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                          .pNext = nullptr,
                                                          .waitSemaphoreValueCount   = 0,
                                                          .pWaitSemaphoreValues      = nullptr,
                                                          .signalSemaphoreValueCount = 1,
                                                          .pSignalSemaphoreValues    = &value };
        const VkSubmitInfo submitInfo = { .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                          .pNext                = &timelineInfo,
                                          .waitSemaphoreCount   = 0,
                                          .pWaitSemaphores      = nullptr,
                                          .pWaitDstStageMask    = nullptr,
                                          .commandBufferCount   = 1,
                                          .pCommandBuffers      = &commandBuffer,
                                          .signalSemaphoreCount = 1,
                                          .pSignalSemaphores    = &m_UploadTimeline };
        if ( const VkResult result = m_Device->SubmitToQueue( m_GraphicsQueue, 1, &submitInfo, submitted.Fence );
             result != VK_SUCCESS )
            return refuse( std::format( "vkQueueSubmit failed: {}", VkResultToString( result ) ) );
        m_UploadSubmittedValue.store( value, std::memory_order_release );
        return Common::MakeSuccess( submitted );
    }

    bool CommandBufferAllocator::IsComplete( const Submitted& submitted ) const
    {
        return vkGetFenceStatus( m_LogicalDevice, submitted.Fence ) == VK_SUCCESS;
    }

    void CommandBufferAllocator::RT_ReleaseSubmitted( const Submitted& submitted )
    {
        // The buffer may still be executing only if the caller gives up early (teardown); waiting here is
        // what makes releasing it legal.
        VK_CHECK_RESULT( vkWaitForFences( m_LogicalDevice, 1, &submitted.Fence, VK_TRUE, UINT64_MAX ) );
        vkDestroyFence( m_LogicalDevice, submitted.Fence, nullptr );
        ReleaseOneShot( *submitted.Pool, submitted.Buffer );
    }

    Common::BoolResultStr CommandBufferAllocator::RT_SubmitGraphicThenRelease( VkCommandBuffer       commandBuffer,
                                                                               std::function<void()> afterGpu )
    {
        (void)CollectFinished();
        auto submitted = RT_SubmitCommandBufferGraphic( commandBuffer );
        if ( !submitted.IsSuccess() )
        {
            if ( afterGpu )
                afterGpu();
            return Common::MakeError<bool>( submitted.GetError() );
        }
        const std::scoped_lock inFlight( m_InFlightMutex );
        m_InFlight.push_back( InFlight{ submitted.GetValue(), std::move( afterGpu ) } );
        return BOOLSUCCESS;
    }

    std::size_t CommandBufferAllocator::CollectFinished()
    {
        // Taken out under the lock, released outside it: a release may free a buffer into its pool
        // (m_PoolMutex) or destroy a staging copy, and neither belongs under m_InFlightMutex.
        std::vector<InFlight> finished;
        {
            const std::scoped_lock inFlight( m_InFlightMutex );
            for ( auto it = m_InFlight.begin(); it != m_InFlight.end(); )
            {
                if ( IsComplete( it->Upload ) )
                {
                    finished.push_back( std::move( *it ) );
                    it = m_InFlight.erase( it );
                }
                else
                    ++it;
            }
        }
        for ( InFlight& done : finished )
        {
            RT_ReleaseSubmitted( done.Upload );
            if ( done.AfterGpu )
                done.AfterGpu();
        }
        return finished.size();
    }

    void CommandBufferAllocator::WaitAndReleaseInFlight()
    {
        std::vector<InFlight> all;
        {
            const std::scoped_lock inFlight( m_InFlightMutex );
            all.swap( m_InFlight );
        }
        for ( InFlight& upload : all )
        {
            RT_ReleaseSubmitted( upload.Upload ); // waits for its fence
            if ( upload.AfterGpu )
                upload.AfterGpu();
        }
    }

    std::size_t CommandBufferAllocator::OutstandingOneShotCount() const
    {
        const std::scoped_lock pools( m_PoolMutex );
        return m_OneShotPools.size();
    }

    void CommandBufferAllocator::Destroy()
    {
        if ( m_LogicalDevice == VK_NULL_HANDLE )
            return;

        // Before the pools go: the in-flight buffers are freed into them, and their staging copies released
        // while the VMA allocator still exists (VulkanContext::Shutdown destroys it after this call).
        WaitAndReleaseInFlight();
        // Every signal of the upload timeline was in a submission WaitAndReleaseInFlight just waited, and the
        // frame loop has drained (VulkanContext::Shutdown), so no queue still references it.
        if ( m_UploadTimeline != VK_NULL_HANDLE )
            vkDestroySemaphore( m_LogicalDevice, m_UploadTimeline, nullptr );
        m_UploadTimeline = VK_NULL_HANDLE;

        const std::scoped_lock pools( m_PoolMutex );

        // A buffer still out means a caller took one and never flushed it back. Said out loud rather than
        // swallowed: the pools below take it with them either way, so silence here would hide a recording
        // path that never submits — which is a dropped upload, not only a leak.
        if ( !m_OneShotPools.empty() )
        {
            LOG_ERROR( "[CommandBuffers] {} one-off command buffer(s) were allocated and never flushed; "
                       "whatever they recorded was never submitted.",
                       m_OneShotPools.size() );
            m_OneShotPools.clear();
        }

        // Destroying a pool frees every command buffer allocated from it, including those still queued in
        // PendingFree for a thread that never allocated again.
        for ( const auto& [key, pool] : m_ThreadPools )
            vkDestroyCommandPool( m_LogicalDevice, pool->Handle, nullptr );
        m_ThreadPools.clear();

        m_LogicalDevice = VK_NULL_HANDLE;
    }
} // namespace Desert::Graphic::API::Vulkan
