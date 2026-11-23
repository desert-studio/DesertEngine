#include "VulkanRayTracingFunctions.hpp"

namespace Desert::Graphic::API::Vulkan
{
    bool RayTracingDeviceFunctions::Complete() const
    {
        return CreateAccelerationStructure != nullptr && DestroyAccelerationStructure != nullptr &&
               GetAccelerationStructureBuildSizes != nullptr && CmdBuildAccelerationStructures != nullptr &&
               GetAccelerationStructureDeviceAddress != nullptr && GetBufferDeviceAddress != nullptr;
    }

    bool RayTracingDeviceFunctions::MicromapsComplete() const
    {
        return Complete() && CreateMicromap != nullptr && DestroyMicromap != nullptr &&
               GetMicromapBuildSizes != nullptr && CmdBuildMicromaps != nullptr;
    }

    RayTracingDeviceFunctions LoadRayTracingDeviceFunctions( const DeviceCaps&         caps,
                                                             const DeviceProcResolver& resolve )
    {
        RayTracingDeviceFunctions functions;
        if ( !caps.Has( Capability::BufferDeviceAddress ) )
            return functions;
        // Core at 1.2 (the engine's minimum); the KHR alias is the same entry point on a device that reached
        // bufferDeviceAddress through the extension route instead. Loaded WITHOUT acceleration structures too:
        // the GPU skin cache's raster consumers fetch its streams by address on a device that cannot trace.
        PFN_vkVoidFunction address = resolve( "vkGetBufferDeviceAddress" );
        if ( address == nullptr )
            address = resolve( "vkGetBufferDeviceAddressKHR" );
        functions.GetBufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>( address );
        if ( !caps.Has( Capability::AccelerationStructure ) )
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
        if ( !caps.Has( Capability::OpacityMicromap ) )
            return functions;
        functions.CreateMicromap =
             reinterpret_cast<PFN_vkCreateMicromapEXT>( resolve( "vkCreateMicromapEXT" ) );
        functions.DestroyMicromap =
             reinterpret_cast<PFN_vkDestroyMicromapEXT>( resolve( "vkDestroyMicromapEXT" ) );
        functions.GetMicromapBuildSizes =
             reinterpret_cast<PFN_vkGetMicromapBuildSizesEXT>( resolve( "vkGetMicromapBuildSizesEXT" ) );
        functions.CmdBuildMicromaps =
             reinterpret_cast<PFN_vkCmdBuildMicromapsEXT>( resolve( "vkCmdBuildMicromapsEXT" ) );
        // All or none: a half-resolved table would let a caller create a micromap it cannot build.
        if ( !functions.MicromapsComplete() )
        {
            functions.CreateMicromap        = nullptr;
            functions.DestroyMicromap       = nullptr;
            functions.GetMicromapBuildSizes = nullptr;
            functions.CmdBuildMicromaps     = nullptr;
        }
        return functions;
    }
} // namespace Desert::Graphic::API::Vulkan
