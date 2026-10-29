// A resident mesh's vertex and index buffers are BLAS build inputs exactly where the device can build one
// (RT-SCENE): MeshBufferUsage is the one rule VulkanVertexBuffer / VulkanIndexBuffer create them with.

#include <Engine/Graphic/API/Vulkan/MeshBufferUsage.hpp>

#include <gtest/gtest.h>

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

    constexpr VkBufferUsageFlags kBuildInput =
         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
} // namespace

// Red if the RT bits are added unconditionally: a usage bit of an extension the device did not enable is invalid.
TEST( MeshBufferUsage, ADeviceWithoutAccelerationStructuresKeepsTheRasterFlags )
{
    const DeviceCaps caps = CapsWith( { Capability::BufferDeviceAddress } );

    // VIRT-GEOM: bufferDeviceAddress alone adds SHADER_DEVICE_ADDRESS (the cluster passes read the pool spans by
    // address) and STORAGE on the index page (its cluster words), never the build-input bit.
    EXPECT_EQ( MeshBufferUsage( MeshBufferRole::Vertex, caps ),
               VkBufferUsageFlags{ VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT } );
    EXPECT_EQ( MeshBufferUsage( MeshBufferRole::Index, caps ),
               VkBufferUsageFlags{ VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT } );
    // Without bufferDeviceAddress the raster flags stay exactly as they were.
    const DeviceCaps none = CapsWith( {} );
    EXPECT_EQ( MeshBufferUsage( MeshBufferRole::Index, none ),
               VkBufferUsageFlags{ VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT } );
}

// Red if the RT bits are dropped (a BLAS build on these buffers would fail validation and have no address).
TEST( MeshBufferUsage, ADeviceWithAccelerationStructuresMakesBothBuffersBuildInputs )
{
    const DeviceCaps caps = CapsWith( { Capability::BufferDeviceAddress, Capability::AccelerationStructure } );

    const VkBufferUsageFlags vertex = MeshBufferUsage( MeshBufferRole::Vertex, caps );
    const VkBufferUsageFlags index  = MeshBufferUsage( MeshBufferRole::Index, caps );

    EXPECT_EQ( vertex & kBuildInput, kBuildInput );
    EXPECT_EQ( index & kBuildInput, kBuildInput );
    // ... and the raster roles stay.
    EXPECT_NE( vertex & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0u );
    EXPECT_NE( vertex & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0u );
    EXPECT_NE( index & VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 0u );
    EXPECT_EQ( index & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0u );
}
