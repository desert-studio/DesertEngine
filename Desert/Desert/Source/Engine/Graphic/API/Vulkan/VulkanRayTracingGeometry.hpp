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
        // out, masked sections' segments non-opaque) from its resident vertex and index buffers. The build is
        // submitted, NOT waited for: barriers on the one graphics queue order it after the geometry's upload and
        // before every later submission (the frame's TLAS build, every ray query); its scratch buffer is freed
        // once the fence is seen signalled (CommandBufferAllocator::RT_SubmitGraphicThenRelease). Null result
        // value: the device has
        // no acceleration structures, the buffers are not resident, or the input has no segment (a skinned mesh,
        // no triangle) — no BLAS by design. An error: the device can build one and the build failed.
        [[nodiscard]] static Common::ResultStr<std::shared_ptr<VulkanRayTracingGeometry>>
        BuildStatic( const RayTracing::RayTracingMeshInput& input );

        // THE OUTPUT BUFFERS OF ONE SEGMENT'S OPACITY MICROMAP BAKE (RT-OMM-SER; OpacityMicromapBake.shader writes
        // them by address, BuildStatic reads them through RayTracingMeshInput::Micromaps): data (64 B per
        // triangle at kOpacityMicromapLevel), VkMicromapTriangleEXT rows (8 B) and the uint32 per-triangle index.
        // Owner holds the three until its last reference drops; they are then released on the frame-retire ring,
        // so a bake or a build still in flight never reads a freed buffer. An error: the device has no micromap
        // functions (the caller reads the catalog first) or an allocation failed.
        [[nodiscard]] static Common::ResultStr<RayTracing::RayTracingSegmentOpacityMicromap>
        AllocateOpacityMicromapBake( uint32_t segment, uint32_t triangleCount );

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
        // The opacity micromaps the BLAS was built with (RayTracingMeshInput::Micromaps): each must outlive every
        // trace of the BLAS (VK_EXT_opacity_micromap), so it is released with it, on the same frame ring.
        struct Micromap
        {
            VkMicromapEXT     Handle        = VK_NULL_HANDLE;
            VkBuffer          Storage       = VK_NULL_HANDLE;
            VmaAllocation     StorageMemory = nullptr;
            ResourceOwnership Ledger;
        };
        std::vector<Micromap>    m_Micromaps;
        PFN_vkDestroyMicromapEXT m_DestroyMicromap = nullptr;
    };
} // namespace Desert::Graphic::API::Vulkan
