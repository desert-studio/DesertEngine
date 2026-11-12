#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFrameLoop.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/DeviceLost.hpp>

#include <Common/Core/Profiler.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <optional>
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
        // A reader on another queue waits this value ON THE GPU: an unsignalled one (m_UnsignalledUploadValues)
        // would be a wait nothing ends, so it waits the value below it, which orders it after the same uploads.
        return UploadSyncPoint{ .Timeline = m_UploadTimeline,
                                .Value =
                                     SignalTarget( m_UploadSubmittedValue.load( std::memory_order_acquire ) ) };
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

    namespace
    {
        // FPS-DEVLOST6: the installed one-off marker; the context is stored first and cleared last, so a reader
        // that sees the marker sees its context.
        std::atomic<CommandBufferAllocator::OneOffMarker> g_OneOffMarker{ nullptr };
        std::atomic<void*>                                g_OneOffContext{ nullptr };

        void MarkOneOff( VkCommandBuffer commandBuffer, VkQueue queue, const std::source_location& where ) noexcept
        {
            if ( const CommandBufferAllocator::OneOffMarker marker =
                      g_OneOffMarker.load( std::memory_order_acquire );
                 marker != nullptr )
                marker( g_OneOffContext.load( std::memory_order_acquire ), commandBuffer, queue, where );
        }
    } // namespace

    void CommandBufferAllocator::SetOneOffMarker( OneOffMarker marker, void* context ) noexcept
    {
        if ( marker == nullptr )
        {
            g_OneOffMarker.store( nullptr, std::memory_order_release );
            g_OneOffContext.store( nullptr, std::memory_order_release );
            return;
        }
        g_OneOffContext.store( context, std::memory_order_release );
        g_OneOffMarker.store( marker, std::memory_order_release );
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

        // FPS-DEVLOST6: the one-off's breadcrumb, after its commands (SetOneOffMarker).
        MarkOneOff( commandBuffer, queue, where );
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
        // IN QUEUE ORDER, ON EVERY QUEUE (VK-SUBMIT-THREAD step 2): the submit goes through the frame loop's
        // submit thread (VulkanFrameLoop::SubmitOrdered), FIFO with the frames and the unwaited uploads, and the
        // sync point is read THERE: an upload this thread handed over just before is submitted and published by
        // then, while read here it could still be queued and this reader would not wait for it.
        const VkCommandBuffer buffer    = commandBuffer;
        const VkResult        submitted = VulkanFrameLoop::SubmitOrdered(
             [this, queue, buffer, fence]() -> VkResult
             {
                 const UploadSyncPoint               uploads = NewestUploadSync();
                 const bool                          waitUpload = queue != m_GraphicsQueue && uploads.Value != 0;
                 const VkPipelineStageFlags          waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                 const VkTimelineSemaphoreSubmitInfo timelineInfo{
                             .sType                     = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                             .pNext                     = nullptr,
                             .waitSemaphoreValueCount   = 1,
                             .pWaitSemaphoreValues      = &uploads.Value,
                             .signalSemaphoreValueCount = 0,
                             .pSignalSemaphoreValues    = nullptr };
                 VkSubmitInfo submitInfo{};
                 submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                 submitInfo.commandBufferCount = 1;
                 submitInfo.pCommandBuffers    = &buffer;
                 if ( waitUpload )
                 {
                     submitInfo.pNext              = &timelineInfo;
                     submitInfo.waitSemaphoreCount = 1;
                     submitInfo.pWaitSemaphores    = &uploads.Timeline;
                     submitInfo.pWaitDstStageMask  = &waitStage;
                 }
                 return m_Device->SubmitToQueue( queue, 1, &submitInfo, fence );
             } );
        if ( submitted != VK_SUCCESS )
            return refuse( std::format( "vkQueueSubmit failed: {}", VkResultToString( submitted ) ), fence );

        {
            // The x column of these rows is the number of one-off CPU waits a frame (all of them, and this
            // call site's); a frame's budget is zero.
            DESERT_PROFILE_WAIT( "Vulkan: FlushOneShot fence wait" );
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
    CommandBufferAllocator::RT_SubmitCommandBufferGraphic( VkCommandBuffer      commandBuffer,
                                                           std::source_location where )
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
        // ever free it: a refusal that only returned would leak the buffer for the life of its pool. A buffer
        // that was never submitted is not pending, so releasing it is legal.
        const auto refuse = [&]( const std::string& reason ) -> Common::ResultStr<Submitted>
        {
            ReleaseOneShot( *submitted.Pool, submitted.Buffer );
            return Common::MakeError<Submitted>( reason );
        };
        if ( !Graphic::DeviceLost::AllowWork() )
            return refuse( "the device is lost; the one-off command buffer is dropped rather than submitted." );
        if ( m_UploadTimeline == VK_NULL_HANDLE )
            return refuse( "the upload timeline semaphore was never created (see the startup error); a graphics "
                           "one-off would be unordered against the compute and transfer queues." );

        // FPS-DEVLOST6: the one-off's breadcrumb, after its commands (SetOneOffMarker).
        MarkOneOff( commandBuffer, m_GraphicsQueue, where );
        if ( const VkResult ended = vkEndCommandBuffer( commandBuffer ); ended != VK_SUCCESS )
            return refuse( std::format( "vkEndCommandBuffer failed: {}", VkResultToString( ended ) ) );
        // QUEUED, NOT WAITED (VK-SUBMIT-THREAD step 3; UE 5.x: the render thread enqueues the upload as an RHI
        // command and goes on). The frame threads hand frames to the submit thread and go on recording, so an
        // upload for frame N+1 submitted from here at once could reach the queue before frame N's draws (a
        // streamed mip in TRANSFER_DST while N samples it, REVIEW-FPS13 R1): it is an entry of that thread, FIFO
        // with the frames. Step 2 made this thread wait there for its turn, i.e. behind frame N's submit AND
        // present; now it returns at once. The upload timeline value is reserved at the entry's place in the
        // queue (signal order) and is the submission's completion (IsComplete, RT_ReleaseSubmitted), which holds
        // for a submission still queued as well. Inline only when no loop runs or its thread has exited.
        const auto                    reserve = [this]() noexcept -> uint64_t { return ReserveUploadValue(); };
        const std::optional<uint64_t> queued  = VulkanFrameLoop::EnqueueUnwaited(
             reserve, [this, commandBuffer]( const uint64_t value ) noexcept -> std::string
             { return SubmitQueuedUpload( commandBuffer, value ); } );
        if ( queued.has_value() )
        {
            submitted.Value = *queued;
            return Common::MakeSuccess( submitted );
        }
        if ( const VkResult result = SubmitUploadInline( commandBuffer, submitted.Value ); result != VK_SUCCESS )
            return refuse( std::format( "vkQueueSubmit failed: {}", VkResultToString( result ) ) );
        return Common::MakeSuccess( submitted );
    }

    uint64_t CommandBufferAllocator::ReserveUploadValue() noexcept
    {
        const std::scoped_lock ordered( m_UploadSubmitMutex );
        return ++m_UploadReservedValue;
    }

    VkResult CommandBufferAllocator::SubmitUpload( const VkCommandBuffer commandBuffer,
                                                   const uint64_t        value ) const noexcept
    {
        const VkTimelineSemaphoreSubmitInfo timelineInfo{ .sType =
                                                               VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                          .pNext                     = nullptr,
                                                          .waitSemaphoreValueCount   = 0,
                                                          .pWaitSemaphoreValues      = nullptr,
                                                          .signalSemaphoreValueCount = 1,
                                                          .pSignalSemaphoreValues    = &value };
        const VkSubmitInfo                  submitInfo = { .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                                           .pNext                = &timelineInfo,
                                                           .waitSemaphoreCount   = 0,
                                                           .pWaitSemaphores      = nullptr,
                                                           .pWaitDstStageMask    = nullptr,
                                                           .commandBufferCount   = 1,
                                                           .pCommandBuffers      = &commandBuffer,
                                                           .signalSemaphoreCount = 1,
                                                           .pSignalSemaphores    = &m_UploadTimeline };
        return m_Device->SubmitToQueue( m_GraphicsQueue, 1, &submitInfo, VK_NULL_HANDLE );
    }

    std::string CommandBufferAllocator::SubmitQueuedUpload( const VkCommandBuffer commandBuffer,
                                                            const uint64_t        value ) noexcept
    {
        std::string failure;
        if ( const VkResult submittedNow = SubmitUpload( commandBuffer, value ); submittedNow != VK_SUCCESS )
        {
            (void)NoteIfDeviceLost( submittedNow, "vkQueueSubmit", __FILE__, __LINE__ );
            failure = std::format( "the queued upload of upload timeline value {} was refused: vkQueueSubmit "
                                   "failed: {}",
                                   value, VkResultToString( submittedNow ) );
            LOG_ERROR( "[CommandBuffers] {}", failure );
            // ITS VALUE IS STILL SIGNALLED, FROM THE HOST. The uploads queued after it hold the values after it,
            // so it cannot be given back, and its holder (IsComplete, RT_ReleaseSubmitted, a reader on another
            // queue) must not wait forever; the buffer was never submitted, so the holder's release of it stays
            // legal. A host signal may not pass a pending one (VUID-VkSemaphoreSignalInfo-value-03259): the
            // value before it, the newest that can still be pending, is waited first. This failure path only.
            // REACHED IS NOT DONE: recorded as refused BEFORE the signal, so a holder that sees the value reached
            // and reads what the copy was to write (a readback) gets the refusal, never the bytes it never wrote.
            {
                const std::scoped_lock ordered( m_UploadSubmitMutex );
                m_RefusedUploadValues.push_back( value );
            }
            VkResult signalled = VK_SUCCESS;
            if ( value > 1 )
                signalled = WaitUploadValue( value - 1 );
            if ( signalled == VK_SUCCESS )
            {
                const VkSemaphoreSignalInfo signal{ .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                                                    .pNext     = nullptr,
                                                    .semaphore = m_UploadTimeline,
                                                    .value     = value };
                signalled = vkSignalSemaphore( m_LogicalDevice, &signal );
            }
            if ( signalled != VK_SUCCESS )
            {
                // NOTHING WILL SIGNAL IT NOW (REVIEW G2): on a device that is not lost (an out-of-memory) the
                // timeline would stay below it, and every wait on it (a holder's, Destroy's UINT64_MAX wait, a
                // reader on another queue) would never end. Its buffer never ran and every upload before it is
                // ordered ahead of it, so it is marked DONE ONCE THE VALUE BELOW IT IS
                // (m_UnsignalledUploadValues), recorded before it is published; logged once, here.
                (void)NoteIfDeviceLost( signalled, "vkSignalSemaphore", __FILE__, __LINE__ );
                {
                    const std::scoped_lock ordered( m_UploadSubmitMutex );
                    m_UnsignalledUploadValues.push_back( value );
                    m_AnyUnsignalled.store( true, std::memory_order_release );
                }
                LOG_ERROR( "[CommandBuffers] the refused upload's value {} could not be signalled from the host: "
                           "{}; it counts as reached once value {} is, so no wait hangs on it.",
                           value, VkResultToString( signalled ), value - 1 );
            }
        }
        PublishUploadValue( value );
        return failure;
    }

    VkResult CommandBufferAllocator::SubmitUploadInline( const VkCommandBuffer commandBuffer,
                                                         uint64_t&             value ) noexcept
    {
        std::unique_lock<std::mutex> lock( m_UploadSubmitMutex );
        const auto                   noneQueued = [this]() noexcept -> bool
        { return m_UploadSubmittedValue.load( std::memory_order_relaxed ) == m_UploadReservedValue; };
        if ( !noneQueued() )
        {
            // A value reserved for a queued upload is not submitted yet, and this one, higher, may not pass it.
            // Only at the loop's registration (this caller found no loop, another one queued on the new loop
            // first); the submit thread publishes that value and wakes this. The wait releases the lock.
            DESERT_PROFILE_WAIT( "Vulkan: inline upload waits queued uploads" );
            m_UploadPublished.wait( lock, noneQueued );
        }
        value                 = m_UploadReservedValue + 1;
        const VkResult result = SubmitUpload( commandBuffer, value );
        if ( result != VK_SUCCESS )
            return result; // never reserved: the next upload takes the value, so none is skipped
        m_UploadReservedValue = value;
        m_UploadSubmittedValue.store( value, std::memory_order_release );
        return VK_SUCCESS;
    }

    void CommandBufferAllocator::PublishUploadValue( const uint64_t value ) noexcept
    {
        {
            const std::scoped_lock ordered( m_UploadSubmitMutex );
            m_UploadSubmittedValue.store( value, std::memory_order_release );
        }
        m_UploadPublished.notify_all();
    }

    uint64_t CommandBufferAllocator::UploadCompletedValue() const noexcept
    {
        // NO TIMELINE, NO CALL: CollectFinished reads this every frame, also with nothing in flight, and the
        // timeline is VK_NULL_HANDLE when vkCreateSemaphore refused (every graphics one-off is then refused by
        // name, nothing is in flight) and after Destroy; an invalid handle passed to the driver is undefined.
        if ( m_UploadTimeline == VK_NULL_HANDLE )
            return 0;
        uint64_t completed = 0;
        if ( vkGetSemaphoreCounterValue( m_LogicalDevice, m_UploadTimeline, &completed ) != VK_SUCCESS )
            return 0;
        // An unsignalled value right above the reached one is done as well (m_UnsignalledUploadValues): carried.
        if ( m_AnyUnsignalled.load( std::memory_order_acquire ) )
        {
            const std::scoped_lock ordered( m_UploadSubmitMutex );
            while ( std::binary_search( m_UnsignalledUploadValues.begin(), m_UnsignalledUploadValues.end(),
                                        completed + 1 ) )
                ++completed;
        }
        return completed;
    }

    uint64_t CommandBufferAllocator::SignalTarget( uint64_t value ) const noexcept
    {
        if ( !m_AnyUnsignalled.load( std::memory_order_acquire ) )
            return value;
        const std::scoped_lock ordered( m_UploadSubmitMutex );
        while ( value != 0 &&
                std::binary_search( m_UnsignalledUploadValues.begin(), m_UnsignalledUploadValues.end(), value ) )
            --value;
        return value;
    }

    VkResult CommandBufferAllocator::WaitUploadValue( const uint64_t value ) const noexcept
    {
        // IN SLICES, NOT ONE UINT64_MAX WAIT (REVIEW G2): a wait that began on a value whose host signal then
        // failed would never end; each slice re-reads the target, so it ends on the value below once the
        // failure is recorded. A steady wait ends on the signal, inside its first slice.
        for ( ;; )
        {
            const uint64_t target = SignalTarget( value );
            if ( target == 0 )
                return VK_SUCCESS; // the timeline starts reached at 0
            const VkSemaphoreWaitInfo wait{ .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                            .pNext          = nullptr,
                                            .flags          = 0,
                                            .semaphoreCount = 1,
                                            .pSemaphores    = &m_UploadTimeline,
                                            .pValues        = &target };
            const VkResult            waited = vkWaitSemaphores( m_LogicalDevice, &wait, kUploadWaitSliceNs );
            if ( waited != VK_TIMEOUT )
                return waited;
            // A loss the driver has not reported to this wait yet but another call latched ends it as well.
            if ( !Graphic::DeviceLost::AllowWork() )
                return VK_ERROR_DEVICE_LOST;
        }
    }

    bool CommandBufferAllocator::AnswerQueuedOnSubmitThread( const uint64_t value ) noexcept
    {
        const auto published = [this, value]() noexcept -> bool
        { return m_UploadSubmittedValue.load( std::memory_order_acquire ) >= value; };
        if ( !VulkanFrameLoop::OnSubmitThread() || published() )
            return true;
        // ON THE SUBMIT THREAD, NOT YET SUBMITTED: queued behind the entry this thread runs, and only this thread
        // will ever submit it. It runs the queue up to it here (UE: a wait on the RHI thread executes the
        // pending commands); the value is published there, and the host wait that follows ends on the GPU.
        if ( VulkanFrameLoop::AnswerOnSubmitThread( published ) )
            return true;
        if ( !m_SelfWaitRefusalLogged.exchange( true, std::memory_order_relaxed ) )
            LOG_ERROR( "[CommandBuffers] the submit thread waited for upload timeline value {}, which is neither "
                       "submitted nor queued behind it: the wait is refused (it would never end) and its one-off "
                       "buffer is left to its pool. Logged once.",
                       value );
        return false;
    }

    Common::BoolResultStr CommandBufferAllocator::WaitComplete( const Submitted& submitted )
    {
        if ( !AnswerQueuedOnSubmitThread( submitted.Value ) )
            return Common::MakeFormattedError<bool>( "upload timeline value {} was waited on the submit thread, "
                                                     "which cannot reach it; refused rather than waited forever",
                                                     submitted.Value );
        if ( const VkResult waited = WaitUploadValue( submitted.Value ); waited != VK_SUCCESS )
        {
            (void)NoteIfDeviceLost( waited, "vkWaitSemaphores", __FILE__, __LINE__ );
            return Common::MakeFormattedError<bool>( "the wait for upload timeline value {} failed: {}",
                                                     submitted.Value, VkResultToString( waited ) );
        }
        return BOOLSUCCESS;
    }

    bool CommandBufferAllocator::IsComplete( const Submitted& submitted ) const
    {
        // Reached: the submission and every upload before it completed (a signal's first scope is everything
        // submitted before it on the graphics queue; a refused one is host-signalled only after the value before
        // it). A submission still queued has a value above every one signalled so far.
        return UploadCompletedValue() >= submitted.Value;
    }

    bool CommandBufferAllocator::IsRefused( const Submitted& submitted ) const
    {
        const std::scoped_lock ordered( m_UploadSubmitMutex );
        for ( const uint64_t refused : m_RefusedUploadValues )
        {
            if ( refused == submitted.Value )
                return true;
        }
        return false;
    }

    void CommandBufferAllocator::RT_ReleaseSubmitted( const Submitted& submitted )
    {
        // The buffer may still be queued or executing only if the caller gives up early (teardown) or waits on
        // purpose (VulkanGpuBatch::Wait); waiting the value here is what makes releasing it legal. A value still
        // queued is waited as well: a host wait before its signal is submitted is legal for a timeline, and the
        // submit thread signals it without this thread. ON that thread, a submission queued behind the entry it
        // runs is submitted here first (AnswerQueuedOnSubmitThread); one it cannot reach is refused (logged
        // once) and its buffer left to the pool rather than freed while it might still be submitted.
        if ( !AnswerQueuedOnSubmitThread( submitted.Value ) )
            return;
        VK_CHECK_RESULT( WaitUploadValue( submitted.Value ) );
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
            // ONE read of the upload timeline for every entry (it was one fence query per entry). Read before
            // the lock: an entry pushed after it with a value at or below it is complete all the same.
            const uint64_t         completed = UploadCompletedValue();
            const std::scoped_lock inFlight( m_InFlightMutex );
            for ( auto it = m_InFlight.begin(); it != m_InFlight.end(); )
            {
                if ( it->Upload.Value <= completed )
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
            // Its value is reached, so the buffer is no longer pending: freed without another wait.
            ReleaseOneShot( *done.Upload.Pool, done.Upload.Buffer );
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
            RT_ReleaseSubmitted( upload.Upload ); // waits for its value
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
        // Every signal of the upload timeline is submitted by now (the frame loop has drained and is gone,
        // VulkanContext::Shutdown), and the newest one is waited here, which covers the submissions a holder
        // outside m_InFlight (a readback, a GPU batch) never released, so no queue still references it.
        if ( m_UploadTimeline != VK_NULL_HANDLE )
        {
            if ( const uint64_t newest = m_UploadSubmittedValue.load( std::memory_order_acquire ); newest != 0 )
                VK_CHECK_RESULT( WaitUploadValue( newest ) );
            vkDestroySemaphore( m_LogicalDevice, m_UploadTimeline, nullptr );
        }
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
