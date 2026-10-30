#pragma once

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include <vulkan/vulkan.h>

// THE usage of a mesh's device-local vertex and index buffers — one function, so the two buffer classes and
// the tests read the same rule.
//
// A RESIDENT MESH IS READ BY ADDRESS wherever the device has bufferDeviceAddress (core 1.2, its own DeviceCaps
// row, independent of ray tracing): the GI emissive triangle list names each emitter's vertices and indices by
// address (EmissiveLights.hpp) for the hardware AND the software tracer, so the buffers carry
// SHADER_DEVICE_ADDRESS under that row alone; the allocator is created with the device-address flag under the
// same row (VulkanAllocator.cpp). The mesh is ALSO A BLAS BUILD INPUT where the device can build one (UE
// FRayTracingGeometry reads the mesh's own vertex/index buffers, no copy): with acceleration_structure enabled the
// buffers add ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY. A usage bit of a feature that is not enabled is
// invalid, so each bit follows exactly its row.
namespace Desert::Graphic::API::Vulkan
{
    enum class MeshBufferRole
    {
        Vertex,
        Index,
    };

    [[nodiscard]] inline VkBufferUsageFlags MeshBufferUsage( MeshBufferRole role, const DeviceCaps& caps )
    {
        // STORAGE beside VERTEX: a static mesh's vertices are also what a compute pass reads them as - the
        // morph update blends a skinned mesh's base vertices into a component's morphed copy
        // (MeshRendererMorph.cpp, UE GPUMorphUpdateCS reading the base vertex buffer).
        VkBufferUsageFlags usage =
             role == MeshBufferRole::Vertex
                  ? VkBufferUsageFlags{ VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT }
                  : VkBufferUsageFlags{ VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        // VIRT-GEOM: the cluster passes read a virtual-geometry mesh's vertices, vertex refs and packed triangles
        // through the device addresses of its GeometryPool spans (GpuClusterDag), so wherever the device has
        // bufferDeviceAddress the pages carry SHADER_DEVICE_ADDRESS, and the index page (indices + cluster words)
        // STORAGE as well. A device without it has no Virtual Geometry (CapabilityCatalog «VirtualGeometry»).
        if ( caps.Has( Capability::BufferDeviceAddress ) )
        {
            usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            if ( role == MeshBufferRole::Index )
                usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        if ( caps.Has( Capability::AccelerationStructure ) )
            usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        return usage;
    }
} // namespace Desert::Graphic::API::Vulkan
