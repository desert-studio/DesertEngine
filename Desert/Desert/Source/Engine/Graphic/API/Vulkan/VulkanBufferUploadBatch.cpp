#include <Engine/Graphic/API/Vulkan/VulkanBufferUploadBatch.hpp>

#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanFrameLoop.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUploadRing.hpp>

#include <Common/Core/Logger.hpp>
#include <Common/Core/Profiler.hpp>

#include <cstring>
#include <variant>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        // The batch's own submission passes through the hand-off hooks that flush it: not a second flush.
        thread_local bool t_Flushing = false;
    } // namespace

    VulkanBufferUploadBatch& VulkanBufferUploadBatch::Get() noexcept
    {
        static VulkanBufferUploadBatch s_Batch;
        return s_Batch;
    }

    Common::BoolResultStr VulkanBufferUploadBatch::Write( VulkanAllocator& allocator, const VkBuffer dst,
                                                          const uint64_t offset, const void* data,
                                                          const uint64_t size )
    {
        if ( size == 0 )
            return Common::MakeSuccess( true );
        if ( dst == VK_NULL_HANDLE )
            return Common::MakeError<bool>( "device buffer write before the buffer was created" );
        DESERT_PROFILE_SCOPE( "Upload batch: write" );

        Copy                        copy{ .Dst = dst, .DstOffset = offset, .Size = size };
        std::shared_ptr<const void> lease;
        VmaAllocation               dedicated   = nullptr;
        auto                        reservation = VulkanUploadRing::Get().Reserve( size );
        if ( auto* const slice = std::get_if<VulkanUploadRing::Slice>( &reservation ) )
        {
            std::memcpy( slice->Mapped, data, size );
            copy.Src       = slice->Buffer;
            copy.SrcOffset = slice->Offset;
            lease          = std::move( slice->Lease );
        }
        else
        {
            // The ring is full (or the bytes exceed it): a staging buffer of this write's own, released with the
            // batch. Counted in the stats; the ring logs its Full itself.
            VkBufferCreateInfo info{};
            info.sType         = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size          = size;
            info.usage         = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer   staging = VK_NULL_HANDLE;
            const auto allocated =
                 allocator.RT_AllocateBuffer( "DeviceBuffer_staging", info, VMA_MEMORY_USAGE_CPU_TO_GPU, staging );
            if ( !allocated.IsSuccess() )
                return Common::MakeError<bool>( allocated.GetError() );
            dedicated            = allocated.GetValue();
            MappedMemory mapping = allocator.MapMemory( dedicated );
            const auto   wrote   = mapping.Write( data, size );
            if ( !wrote.IsSuccess() )
            {
                mapping.Unmap();
                allocator.RT_DestroyBuffer( staging, dedicated );
                return Common::MakeFormattedError<bool>( "device buffer staging write: {}", wrote.GetError() );
            }
            copy.Src = staging;
        }

        const std::scoped_lock lock( m_Mutex );
        m_Pending.Allocator = &allocator;
        m_Pending.Copies.push_back( copy );
        if ( lease )
            m_Pending.Leases.push_back( std::move( lease ) );
        if ( dedicated != nullptr )
        {
            m_Pending.Dedicated.emplace_back( copy.Src, dedicated );
            ++m_Stats.DedicatedCopies;
        }
        ++m_Stats.Writes;
        m_Stats.Bytes += size;
        return Common::MakeSuccess( true );
    }

    Common::BoolResultStr VulkanBufferUploadBatch::Flush()
    {
        if ( t_Flushing || VulkanFrameLoop::OnSubmitThread() )
            return Common::MakeSuccess( true );
        {
            const std::scoped_lock lock( m_Mutex );
            if ( m_Pending.Empty() )
                return Common::MakeSuccess( true );
        }
        DESERT_PROFILE_SCOPE( "Upload batch: flush" );
        // Held until the batch is queued: a hand-off on another thread that could read these copies waits here
        // rather than reaching the queue before them.
        const std::scoped_lock flushing( m_FlushMutex );
        t_Flushing = true;
        struct Reset
        {
            ~Reset()
            {
                t_Flushing = false;
            }
        } reset;

        Pending batch;
        {
            const std::scoped_lock lock( m_Mutex );
            std::swap( batch, m_Pending );
            if ( !batch.Empty() )
                ++m_Stats.Flushes;
        }
        if ( batch.Empty() )
            return Common::MakeSuccess( true );

        VulkanAllocator* const owner = batch.Allocator;
        auto                   dedicated =
             std::make_shared<std::vector<std::pair<VkBuffer, VmaAllocation>>>( std::move( batch.Dedicated ) );
        auto leases  = std::make_shared<std::vector<std::shared_ptr<const void>>>( std::move( batch.Leases ) );
        auto release = [owner, dedicated, leases]
        {
            for ( const auto& [buffer, memory] : *dedicated )
                owner->RT_ReleaseStaging( buffer, memory, Common::MakeSuccess( VK_SUCCESS ) );
            dedicated->clear();
            leases->clear();
        };

        auto command = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !command.IsSuccess() )
        {
            release();
            return Common::MakeFormattedError<bool>( "upload batch of {} copies not recorded: {}",
                                                     batch.Copies.size(), command.GetError() );
        }
        const VkCommandBuffer cmd = command.GetValue();

        // THE BARRIERS ARE THE WHOLE DEPENDENCY. Before: ALL_COMMANDS/MEMORY_WRITE -> TRANSFER, so every earlier
        // submission on the queue - a frame still reading a range a recycled arena slot reuses, an earlier write
        // to it - is done before the copies write. After: TRANSFER_WRITE -> ALL_COMMANDS/MEMORY_READ|WRITE, so
        // every later submission sees the bytes, whatever reads them (vertex input, index fetch, a storage read,
        // a BLAS build, a copy out of the arena).
        const VkMemoryBarrier before = { .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                              &before, 0, nullptr, 0, nullptr );
        // A range written twice in one batch (a buffer rewritten before the frame that flushes it) keeps the
        // order of its writes: a TRANSFER -> TRANSFER barrier before the copy that overlaps one since the last.
        std::size_t sinceBarrier = 0;
        for ( std::size_t i = 0; i < batch.Copies.size(); ++i )
        {
            const Copy& copy = batch.Copies[i];
            for ( std::size_t j = sinceBarrier; j < i; ++j )
            {
                const Copy& earlier = batch.Copies[j];
                if ( earlier.Dst == copy.Dst && earlier.DstOffset < copy.DstOffset + copy.Size &&
                     copy.DstOffset < earlier.DstOffset + earlier.Size )
                {
                    const VkMemoryBarrier ordered = { .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                                      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                                      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
                    vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                          1, &ordered, 0, nullptr, 0, nullptr );
                    sinceBarrier = i;
                    break;
                }
            }
            const VkBufferCopy region{
                 .srcOffset = copy.SrcOffset, .dstOffset = copy.DstOffset, .size = copy.Size };
            vkCmdCopyBuffer( cmd, copy.Src, copy.Dst, 1, &region );
        }
        const VkMemoryBarrier after = { .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                              &after, 0, nullptr, 0, nullptr );

        const auto submitted = CommandBufferAllocator::GetInstance().RT_SubmitGraphicThenRelease( cmd, release );
        if ( !submitted.IsSuccess() )
            return Common::MakeFormattedError<bool>( "upload batch of {} copies not submitted: {}",
                                                     batch.Copies.size(), submitted.GetError() );
        return Common::MakeSuccess( true );
    }

    VulkanBufferUploadBatch::Stats VulkanBufferUploadBatch::GetStats() const
    {
        const std::scoped_lock lock( m_Mutex );
        return m_Stats;
    }
} // namespace Desert::Graphic::API::Vulkan
