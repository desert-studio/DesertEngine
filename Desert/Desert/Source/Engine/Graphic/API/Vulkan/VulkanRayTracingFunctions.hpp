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
    };

    // name -> entry point; vkGetDeviceProcAddr bound to the device at run time, a recorder in tests.
    using DeviceProcResolver = std::function<PFN_vkVoidFunction( const char* name )>;

    // Resolves nothing (and returns an empty table) unless caps has AccelerationStructure.
    [[nodiscard]] RayTracingDeviceFunctions LoadRayTracingDeviceFunctions( const DeviceCaps&         caps,
                                                                           const DeviceProcResolver& resolve );
} // namespace Desert::Graphic::API::Vulkan
