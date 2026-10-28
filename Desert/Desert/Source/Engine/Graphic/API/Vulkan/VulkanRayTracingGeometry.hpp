#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanGeometryAddress.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <span>
#include <vector>

// THE VULKAN BLAS OF A STATIC MESH. The build input is DescribeStaticMeshGeometry's segments mapped one to one
// onto VkAccelerationStructureGeometryKHR + VkAccelerationStructureBuildRangeInfoKHR
// (VulkanRayTracingBuildInput.hpp, pure, checked without a device); the object owns the acceleration structure and
// the buffer backing it.
namespace Desert::Graphic::API::Vulkan
{
    class VulkanRayTracingGeometry final : public RayTracing::RayTracingGeometryResource
    {
    public:
        // Builds the BLAS of @p input (DescribeStaticMeshBlas: a mesh's LOD-0 triangles, hidden submeshes left
        // out, masked sections' segments non-opaque) from its resident vertex and index buffers and waits for the
        // build (the upload path is blocking, like the buffer copies before it). Null result value: the device has
        // no acceleration structures, the buffers are not resident, or the input has no segment (a skinned mesh,
        // no triangle) — no BLAS by design. An error: the device can build one and the build failed.
        [[nodiscard]] static Common::ResultStr<std::shared_ptr<VulkanRayTracingGeometry>>
        BuildStatic( const RayTracing::RayTracingMeshInput& input );

        ~VulkanRayTracingGeometry() override;

        [[nodiscard]] uint64_t GetDeviceAddress() const override
        {
            return m_DeviceAddress;
        }
        // The storage's ledger row is the one statement of what this BLAS costs (ResourceOwnership::GetBytes).
        [[nodiscard]] uint64_t GetMemoryBytes() const override
        {
            return m_Ledger.GetBytes();
        }
        // The hit's attribute streams at their CURRENT addresses (VulkanGeometryAddress.hpp: a pool arena that grew
        // since the build moved them).
        [[nodiscard]] RayTracing::RayTracingGeometryLayout GetMetadataLayout() const override
        {
            return RayTracing::MeshAttributeLayout( CurrentGeometryAddress( m_Vertices ),
                                                    CurrentGeometryAddress( m_Indices ), m_Segments, false );
        }
        [[nodiscard]] VkAccelerationStructureKHR GetHandle() const
        {
            return m_Handle;
        }
        // Says who holds the BLAS's ledger row: the mesh service claims a mesh's BLAS for the mesh asset, like
        // the mesh's buffers (MeshGpuUploader). A variant BLAS is never claimed and stays with the attribution
        // scope it was built in (the scene renderer's RT block).
        void ClaimOwnership( const ResourceOwner owner, const Common::AssetHandle asset )
        {
            m_Ledger.Claim( owner, asset );
        }

        VulkanRayTracingGeometry( const VulkanRayTracingGeometry& )            = delete;
        VulkanRayTracingGeometry& operator=( const VulkanRayTracingGeometry& ) = delete;
        VulkanRayTracingGeometry()                                             = default;

    private:
        VkAccelerationStructureKHR            m_Handle        = VK_NULL_HANDLE;
        VkBuffer                              m_Storage       = VK_NULL_HANDLE;
        VmaAllocation                         m_StorageMemory = nullptr;
        VkDeviceAddress                       m_DeviceAddress = 0;
        PFN_vkDestroyAccelerationStructureKHR m_Destroy       = nullptr;
        ResourceOwnership                     m_Ledger; // the storage's row; released with this object
        // What the BLAS was built from (GetMetadataLayout); the mesh holds the buffers as long as it holds this.
        std::vector<RayTracing::RayTracingGeometrySegment> m_Segments;
        VertexBufferRange                                  m_Vertices;
        IndexBufferRange                                   m_Indices;
    };
} // namespace Desert::Graphic::API::Vulkan
