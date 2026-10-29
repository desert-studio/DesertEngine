#pragma once

#include <Engine/Graphic/SharedGpuMemory.hpp>

#include <memory>
#include <string_view>

namespace Desert::Graphic::API::Vulkan
{
    // The Vulkan half of SharedGpuMemory.hpp (VulkanRendererAPI::CreateSharedBuffer / CreateSharedTimeline).
    //
    // BUFFER: a VkBuffer (STORAGE | TRANSFER_SRC | TRANSFER_DST, EXCLUSIVE) created with
    // VkExternalMemoryBufferCreateInfo, bound to a DEDICATED device-local vkAllocateMemory with
    // VkExportMemoryAllocateInfo (not VMA: an exported allocation must be its own memory object, and CUDA imports
    // the whole object). The handle: vkGetMemoryWin32HandleKHR (one NT handle, closed with the buffer) or
    // vkGetMemoryFdKHR (a fresh descriptor per ExportHandle; the importer owns it). Destroyed through the
    // allocator's frames-in-flight queue (RT_DestroyLater), so a frame still in flight keeps its memory.
    //
    // TIMELINE: a VkSemaphore with VkSemaphoreTypeCreateInfo{TIMELINE, 0} and VkExportSemaphoreCreateInfo; the
    // handle from vkGetSemaphoreWin32HandleKHR / vkGetSemaphoreFdKHR.
    //
    // The handle type is the catalog's (CapabilityCatalog::Sharing): Win32 or Fd; anything else is refused by
    // name. The extension entry points are loaded with vkGetDeviceProcAddr (VK_KHR_external_memory_win32 / _fd,
    // VK_KHR_external_semaphore_win32 / _fd, enabled by DeviceCapsProbe when present).
    [[nodiscard]] Common::ResultStr<std::shared_ptr<SharedGpuBuffer>>
    CreateVulkanSharedBuffer( std::string_view name, uint64_t bytes );
    [[nodiscard]] Common::ResultStr<std::shared_ptr<SharedGpuTimeline>>
    CreateVulkanSharedTimeline( std::string_view name );
} // namespace Desert::Graphic::API::Vulkan
