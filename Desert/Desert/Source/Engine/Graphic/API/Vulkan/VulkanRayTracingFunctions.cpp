#include "VulkanRayTracingFunctions.hpp"

namespace Desert::Graphic::API::Vulkan
{
    bool RayTracingDeviceFunctions::Complete() const
    {
        return CreateAccelerationStructure != nullptr && DestroyAccelerationStructure != nullptr &&
               GetAccelerationStructureBuildSizes != nullptr && CmdBuildAccelerationStructures != nullptr &&
               GetAccelerationStructureDeviceAddress != nullptr && GetBufferDeviceAddress != nullptr;
    }

    RayTracingDeviceFunctions LoadRayTracingDeviceFunctions( const DeviceCaps&         caps,
                                                             const DeviceProcResolver& resolve )
    {
        RayTracingDeviceFunctions functions;
        if ( !caps.Has( Capability::AccelerationStructure ) || !caps.Has( Capability::BufferDeviceAddress ) )
            return functions;

        functions.CreateAccelerationStructure = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
             resolve( "vkCreateAccelerationStructureKHR" ) );
        functions.DestroyAccelerationStructure = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
             resolve( "vkDestroyAccelerationStructureKHR" ) );
        functions.GetAccelerationStructureBuildSizes =
             reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                  resolve( "vkGetAccelerationStructureBuildSizesKHR" ) );
        functions.CmdBuildAccelerationStructures = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
             resolve( "vkCmdBuildAccelerationStructuresKHR" ) );
        functions.GetAccelerationStructureDeviceAddress =
             reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                  resolve( "vkGetAccelerationStructureDeviceAddressKHR" ) );
        // Core at 1.2 (the engine's minimum); the KHR alias is the same entry point on a device that reached
        // bufferDeviceAddress through the extension route instead.
        PFN_vkVoidFunction address = resolve( "vkGetBufferDeviceAddress" );
        if ( address == nullptr )
            address = resolve( "vkGetBufferDeviceAddressKHR" );
        functions.GetBufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>( address );
        return functions;
    }
} // namespace Desert::Graphic::API::Vulkan
