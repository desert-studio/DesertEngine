#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanBufferUploadBatch.hpp>
#include <Engine/Graphic/API/Vulkan/MeshBufferUsage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

#include <Common/Core/ResultStr.hpp>

#include <vulkan/vulkan.h>

// THE DEVICE SIDE OF A BufferUsage::Pooled BUFFER (GeometryPool arenas): shared by VulkanVertexBuffer and
// VulkanIndexBuffer, which differ only in the mesh role. Every transfer is a copy recorded into the
// frame's upload batch (RT_UploadDeviceBuffer below).
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
    // copies @p size bytes of @p data into the frame's upload batch (VulkanBufferUploadBatch: a slice of the
    // stream-in staging ring, one recorded copy), which the next hand-off to the queue submits with every other
    // write of the frame in ONE unwaited submission (WP-OPT-UPLOAD-BATCH; UE FRHICommandList::UpdateBuffer).
    // @p data is copied before this returns, so the caller may drop it at once. Nothing here waits for the GPU or
    // builds a submission: a frame or one-off that reads the bytes is handed over after the batch (queue order),
    // and the batch's barriers make them visible to it.
    [[nodiscard]] inline Common::BoolResultStr RT_UploadDeviceBuffer( VulkanAllocator& allocator, const VkBuffer dst,
                                                                      const uint64_t offset, const void* data,
                                                                      const uint64_t size )
    {
        return VulkanBufferUploadBatch::Get().Write( allocator, dst, offset, data, size );
    }
} // namespace Desert::Graphic::API::Vulkan
