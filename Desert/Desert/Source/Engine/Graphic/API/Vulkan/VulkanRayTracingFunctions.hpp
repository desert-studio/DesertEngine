#pragma once

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include <vulkan/vulkan.h>

#include <functional>

namespace Desert::Graphic::API::Vulkan
{
    // The device-level entry points acceleration structures need. The loader exports only core symbols; the
    // VK_KHR_acceleration_structure ones exist only through vkGetDeviceProcAddr on a device that ENABLED the
    // extension (asking otherwise is undefined, and some drivers hand back a stub). So the table is filled
    // iff DeviceCaps carries AccelerationStructure (the probe enabled it together with bufferDeviceAddress,
    // which the AS row depends on) and is empty on every other device.
    struct RayTracingDeviceFunctions
    {
        PFN_vkCreateAccelerationStructureKHR           CreateAccelerationStructure           = nullptr;
        PFN_vkDestroyAccelerationStructureKHR          DestroyAccelerationStructure          = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR    GetAccelerationStructureBuildSizes    = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR        CmdBuildAccelerationStructures        = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR GetAccelerationStructureDeviceAddress = nullptr;
        // Core 1.2 (bufferDeviceAddress); loaded through the device like the rest so one table answers.
        PFN_vkGetBufferDeviceAddress GetBufferDeviceAddress = nullptr;

        // Every entry resolved. False on a device without AccelerationStructure, and false when a driver that
        // advertised the extension failed to resolve one (the caller logs and treats ray tracing as absent).
        [[nodiscard]] bool Complete() const;

        // The device address of @p buffer (created with SHADER_DEVICE_ADDRESS): what every AS build input names.
        [[nodiscard]] VkDeviceAddress BufferAddress( VkDevice device, VkBuffer buffer ) const
        {
            VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            info.buffer = buffer;
            return GetBufferDeviceAddress( device, &info );
        }
    };

    // An exclusive buffer of @p size whose address a build input or a TLAS row can name.
    [[nodiscard]] inline VkBufferCreateInfo DeviceAddressBufferInfo( VkDeviceSize size, VkBufferUsageFlags usage )
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size        = size;
        info.usage       = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        return info;
    }

    // name -> entry point; vkGetDeviceProcAddr bound to the device at run time, a recorder in tests.
    using DeviceProcResolver = std::function<PFN_vkVoidFunction( const char* name )>;

    // Resolves nothing (and returns an empty table) unless caps has AccelerationStructure.
    [[nodiscard]] RayTracingDeviceFunctions LoadRayTracingDeviceFunctions( const DeviceCaps&         caps,
                                                                           const DeviceProcResolver& resolve );
} // namespace Desert::Graphic::API::Vulkan
