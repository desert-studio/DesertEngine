#pragma once

#include <Engine/Graphic/API/Vulkan/CommandBufferAllocator.hpp>
#include <Engine/Graphic/API/Vulkan/MeshBufferUsage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

#include <Common/Core/ResultStr.hpp>

#include <vulkan/vulkan.h>

// THE DEVICE SIDE OF A BufferUsage::Pooled BUFFER (GeometryPool arenas): shared by VulkanVertexBuffer and
// VulkanIndexBuffer, which differ only in the mesh role. Every transfer is a blocking one-off copy on the graphics
// queue, the pattern of the static mesh upload these buffers replace.
namespace Desert::Graphic::API::Vulkan
{
    // A device-local buffer of the mesh role, also a transfer source (a copy may read a mesh out of it).
    [[nodiscard]] inline Common::ResultStr<VmaAllocation> RT_CreatePooledBuffer( VulkanAllocator&     allocator,
                                                                                 const MeshBufferRole role,
                                                                                 const uint64_t       size,
                                                                                 const char* tag, VkBuffer& out )
    {
        VkBufferCreateInfo info{};
        info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size        = size;
        info.usage       = MeshBufferUsage( role, allocator.GetDeviceCaps() ) | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        return allocator.RT_AllocateBuffer( tag, info, VMA_MEMORY_USAGE_GPU_ONLY, out );
    }

    // THE ONE UPLOAD PATH OF A DEVICE-LOCAL BUFFER (a GeometryPool arena range, a static vertex or index buffer):
    // copies @p size bytes of @p data to @p dst at @p offset through a staging buffer, SUBMITTED AND NOT WAITED FOR
    // (FPS-CPU-3; UE's RHI upload batching: the loading thread never sleeps on the GPU). @p data is copied into the
    // staging buffer before this returns, so the caller may drop it at once. The staging buffer is released once
    // the copy's fence is seen signalled (CommandBufferAllocator::CollectFinished, every frame).
    // THE BARRIERS ARE THE WHOLE DEPENDENCY. The first: ALL_COMMANDS/MEMORY_WRITE -> TRANSFER, so every earlier
    // submission on the queue - a frame still reading the range a recycled arena slot reuses, an earlier write
    // to it - is done before the copy writes. The second: TRANSFER_WRITE -> ALL_COMMANDS/MEMORY_READ|WRITE, so
    // every later submission sees the bytes, whatever reads them (vertex input, index fetch, a storage read, a
    // BLAS build, a copy out of the arena). Readers on another queue go through FlushOneShot, which waits the
    // in-flight uploads first.
    [[nodiscard]] inline Common::BoolResultStr RT_UploadDeviceBuffer( VulkanAllocator& allocator, const VkBuffer dst,
                                                                      const uint64_t offset, const void* data,
                                                                      const uint64_t size )
    {
        if ( size == 0 )
            return Common::MakeSuccess( true );
        if ( dst == VK_NULL_HANDLE )
            return Common::MakeError<bool>( "device buffer write before the buffer was created" );
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
        const VmaAllocation stagingMemory = allocated.GetValue();
        {
            MappedMemory mapping = allocator.MapMemory( stagingMemory );
            const auto   wrote   = mapping.Write( data, size );
            if ( !wrote.IsSuccess() )
            {
                mapping.Unmap();
                allocator.RT_DestroyBuffer( staging, stagingMemory );
                return Common::MakeFormattedError<bool>( "device buffer staging write: {}", wrote.GetError() );
            }
        }
        auto command = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !command.IsSuccess() )
        {
            allocator.RT_DestroyBuffer( staging, stagingMemory );
            return Common::MakeError<bool>( command.GetError() );
        }
        const VkCommandBuffer cmd = command.GetValue();

        const VkMemoryBarrier before = { .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before,
                              0, nullptr, 0, nullptr );
        VkBufferCopy region{};
        region.dstOffset = offset;
        region.size      = size;
        vkCmdCopyBuffer( cmd, staging, dst, 1, &region );
        const VkMemoryBarrier after = { .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
        vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0,
                              nullptr, 0, nullptr );

        VulkanAllocator* const owner     = &allocator;
        const auto             submitted = CommandBufferAllocator::GetInstance().RT_SubmitGraphicThenRelease(
             cmd, [owner, staging, stagingMemory]
             { owner->RT_ReleaseStaging( staging, stagingMemory, Common::MakeSuccess( VK_SUCCESS ) ); } );
        if ( !submitted.IsSuccess() )
            return Common::MakeFormattedError<bool>( "device buffer copy not submitted: {}", submitted.GetError() );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::API::Vulkan
