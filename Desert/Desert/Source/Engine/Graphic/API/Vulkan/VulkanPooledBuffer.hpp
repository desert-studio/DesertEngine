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

    // Copies @p size bytes of @p data to @p dst at @p offset through a staging buffer.
    [[nodiscard]] inline Common::BoolResultStr RT_WritePooledBuffer( VulkanAllocator& allocator,
                                                                     const VkBuffer dst, const uint64_t offset,
                                                                     const void* data, const uint64_t size )
    {
        if ( size == 0 )
            return Common::MakeSuccess( true );
        if ( dst == VK_NULL_HANDLE )
            return Common::MakeError<bool>( "pooled buffer write before the buffer was created" );
        VkBufferCreateInfo info{};
        info.sType         = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size          = size;
        info.usage         = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer   staging = VK_NULL_HANDLE;
        const auto allocated =
             allocator.RT_AllocateBuffer( "PooledBuffer_staging", info, VMA_MEMORY_USAGE_CPU_TO_GPU, staging );
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
                return Common::MakeFormattedError<bool>( "pooled buffer staging write: {}", wrote.GetError() );
            }
        }
        auto command = CommandBufferAllocator::GetInstance().RT_AllocateCommandBufferGraphic( true );
        if ( !command.IsSuccess() )
        {
            allocator.RT_DestroyBuffer( staging, stagingMemory );
            return Common::MakeError<bool>( command.GetError() );
        }
        VkBufferCopy region{};
        region.dstOffset = offset;
        region.size      = size;
        vkCmdCopyBuffer( command.GetValue(), staging, dst, 1, &region );
        const auto flushed =
             CommandBufferAllocator::GetInstance().RT_FlushCommandBufferGraphic( command.GetValue() );
        allocator.RT_ReleaseStaging( staging, stagingMemory, flushed );
        if ( !flushed.IsSuccess() )
            return Common::MakeFormattedError<bool>( "pooled buffer copy not submitted: {}", flushed.GetError() );
        if ( flushed.GetValue() != VK_SUCCESS )
            return Common::MakeFormattedError<bool>( "pooled buffer copy failed (VkResult {})",
                                                     static_cast<int>( flushed.GetValue() ) );
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Graphic::API::Vulkan
