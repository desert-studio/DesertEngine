// The acceleration-structure entry points are resolved through the device exactly where the device enabled the
// extension (RT-SCENE): VulkanLogicalDevice fills its table with LoadRayTracingDeviceFunctions right after
// creation. A resolver records the names asked for; no device is involved.

#include <Engine/Graphic/API/Vulkan/VulkanRayTracingFunctions.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace Desert::Graphic::API::Vulkan;

namespace
{
    DeviceCaps CapsWith( std::initializer_list<Capability> present )
    {
        DeviceCaps caps;
        for ( const Capability capability : present )
            caps.Rows[static_cast<std::size_t>( capability )].Present = true;
        return caps;
    }

    void Stub()
    {
    }
} // namespace

// Red if the table is loaded unconditionally: vkGetDeviceProcAddr for an extension the device did not enable is
// undefined behaviour, so on a device without the AS row no acceleration-structure name may be asked for. The core
// buffer-address entry IS loaded there (the GPU skin cache's raster consumers fetch by address without ray
// tracing).
TEST( RayTracingFunctions, OnlyTheBufferAddressIsResolvedOnADeviceWithoutAccelerationStructure )
{
    std::set<std::string> asked;
    const auto functions = LoadRayTracingDeviceFunctions( CapsWith( { Capability::BufferDeviceAddress } ),
                                                          [&asked]( const char* name ) -> PFN_vkVoidFunction
                                                          {
                                                              asked.insert( name );
                                                              return &Stub;
                                                          } );
    EXPECT_EQ( asked, ( std::set<std::string>{ "vkGetBufferDeviceAddress" } ) );
    EXPECT_NE( functions.GetBufferDeviceAddress, nullptr );
    EXPECT_FALSE( functions.Complete() );
    EXPECT_EQ( functions.CmdBuildAccelerationStructures, nullptr );
}

// Red if a device without buffer device addresses is asked for anything at all.
TEST( RayTracingFunctions, NothingIsResolvedOnADeviceWithoutBufferDeviceAddress )
{
    std::set<std::string> asked;
    const auto            functions = LoadRayTracingDeviceFunctions( CapsWith( {} ),
                                                                     [&asked]( const char* name ) -> PFN_vkVoidFunction
                                                                     {
                                                              asked.insert( name );
                                                              return &Stub;
                                                          } );
    EXPECT_TRUE( asked.empty() );
    EXPECT_EQ( functions.GetBufferDeviceAddress, nullptr );
}

// Red if an entry the BLAS/TLAS builds call is dropped from the load (it would stay null and crash at the first
// build) or is asked for under a misspelt name (the driver returns null for it).
TEST( RayTracingFunctions, EveryEntryPointIsResolvedByItsVulkanNameWhereTheExtensionIsEnabled )
{
    std::set<std::string> asked;
    const auto            functions = LoadRayTracingDeviceFunctions(
         CapsWith( { Capability::BufferDeviceAddress, Capability::AccelerationStructure } ),
         [&asked]( const char* name ) -> PFN_vkVoidFunction
         {
             asked.insert( name );
             return &Stub;
         } );
    EXPECT_EQ( asked, ( std::set<std::string>{
                           "vkCreateAccelerationStructureKHR", "vkDestroyAccelerationStructureKHR",
                           "vkGetAccelerationStructureBuildSizesKHR", "vkCmdBuildAccelerationStructuresKHR",
                           "vkGetAccelerationStructureDeviceAddressKHR", "vkGetBufferDeviceAddress" } ) );
    EXPECT_TRUE( functions.Complete() );
}

// Red if Complete() ignores an entry: a driver that resolves all but one must be refused by device creation, not
// run with a null pointer in the table.
TEST( RayTracingFunctions, OneUnresolvedEntryLeavesTheTableIncomplete )
{
    const auto functions = LoadRayTracingDeviceFunctions(
         CapsWith( { Capability::BufferDeviceAddress, Capability::AccelerationStructure } ),
         []( const char* name ) -> PFN_vkVoidFunction
         { return std::string( name ) == "vkGetAccelerationStructureDeviceAddressKHR" ? nullptr : &Stub; } );
    EXPECT_FALSE( functions.Complete() );
}
