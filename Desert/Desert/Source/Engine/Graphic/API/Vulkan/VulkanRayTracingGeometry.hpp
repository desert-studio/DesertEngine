#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <span>
#include <vector>

namespace Desert
{
    class Mesh;
}

// THE VULKAN BLAS OF A STATIC MESH. The build input is DescribeStaticMeshGeometry's segments mapped one to one
// onto VkAccelerationStructureGeometryKHR + VkAccelerationStructureBuildRangeInfoKHR
// (VulkanRayTracingBuildInput.hpp, pure, checked without a device); the object owns the acceleration structure and
// the buffer backing it.
namespace Desert::Graphic::API::Vulkan
{
    class VulkanRayTracingGeometry final : public RayTracing::RayTracingGeometryResource
    {
    public:
        // Builds the BLAS of @p mesh's LOD-0 triangles from its resident vertex and index buffers and waits for
        // the build (the upload path is blocking, like the buffer copies before it). Null result value: the
        // device has no acceleration structures, the mesh is skinned, or it has no triangle — no BLAS by design.
        // An error: the device can build one and the build failed.
        // @p hiddenSubmeshes (bit i = submesh i) are left out of the build: 0 for the mesh's own BLAS, the
        // instance's mask for the variant an instance hiding some of them traces.
        [[nodiscard]] static Common::ResultStr<std::shared_ptr<VulkanRayTracingGeometry>>
        BuildStatic( Mesh& mesh, uint64_t hiddenSubmeshes );

        ~VulkanRayTracingGeometry() override;

        [[nodiscard]] uint64_t GetDeviceAddress() const override
        {
            return m_DeviceAddress;
        }
        [[nodiscard]] uint64_t GetMemoryBytes() const override
        {
            return m_StorageBytes;
        }
        [[nodiscard]] RayTracing::RayTracingGeometryLayout GetMetadataLayout() const override
        {
            return RayTracing::MeshAttributeLayout( m_VertexAddress, m_IndexAddress, m_Segments, false );
        }
        [[nodiscard]] VkAccelerationStructureKHR GetHandle() const
        {
            return m_Handle;
        }

        VulkanRayTracingGeometry( const VulkanRayTracingGeometry& )            = delete;
        VulkanRayTracingGeometry& operator=( const VulkanRayTracingGeometry& ) = delete;
        VulkanRayTracingGeometry()                                             = default;

    private:
        VkAccelerationStructureKHR            m_Handle        = VK_NULL_HANDLE;
        VkBuffer                              m_Storage       = VK_NULL_HANDLE;
        VmaAllocation                         m_StorageMemory = nullptr;
        VkDeviceSize                          m_StorageBytes  = 0; // the allocation's size (stats overlay)
        VkDeviceAddress                       m_DeviceAddress = 0;
        PFN_vkDestroyAccelerationStructureKHR m_Destroy       = nullptr;
        ResourceOwnership                     m_Ledger; // the storage's row; released with this object
        // What the BLAS was built from (GetMetadataLayout); the mesh holds the buffers as long as it holds this.
        std::vector<RayTracing::RayTracingGeometrySegment> m_Segments;
        VkDeviceAddress                                    m_VertexAddress = 0;
        VkDeviceAddress                                    m_IndexAddress  = 0;
    };
} // namespace Desert::Graphic::API::Vulkan
