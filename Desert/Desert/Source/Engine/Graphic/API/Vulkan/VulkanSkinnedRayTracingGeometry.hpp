#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanGeometryAddress.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>
#include <Engine/Graphic/Systems/RayTracing/SkinnedRayTracingGeometry.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

namespace Desert::Graphic
{
    class IndexBuffer;
    class VertexBuffer;
} // namespace Desert::Graphic

// THE VULKAN SKIN-CACHE ENTRY OF ONE SKINNED PRIMITIVE: its cached streams (VulkanSkinCacheStreams, written by
// "SkinCache: Skin") and the BLAS built from their current positions (VulkanSkinnedRayTracingGeometry). Both live
// across frames; the BLAS is recreated only when the mesh's buffers or the hidden mask change, and every other
// frame the BLAS is REFIT in place (UE FRayTracingDynamicGeometryUpdateParams with bAllowUpdate: ALLOW_UPDATE +
// PREFER_FAST_BUILD, MODE_UPDATE with src == dst). The build is recorded by the graph pass
// "RayTracingScene.DynamicGeometry" (RayTracingDynamicGeometry.hpp).
namespace Desert::Graphic::API::Vulkan
{
    // One entry's streams buffer (RayTracingSkinCache.hpp layout, kSkinCacheFloatsPerVertex floats per vertex):
    // STORAGE (skin pass write, raster fetch by device address) + AS build input (the BLAS reads its first 3V
    // floats). Recreated only when the vertex count changes.
    class VulkanSkinCacheStreams final : public RayTracing::SkinCacheStreams
    {
    public:
        VulkanSkinCacheStreams() = default;
        ~VulkanSkinCacheStreams() override;
        VulkanSkinCacheStreams( const VulkanSkinCacheStreams& )            = delete;
        VulkanSkinCacheStreams& operator=( const VulkanSkinCacheStreams& ) = delete;

        [[nodiscard]] Common::ResultStr<bool> Prepare( uint32_t vertexCount ) override;
        [[nodiscard]] RDG::ExternalBuffer&    GetBuffer() override
        {
            return m_External;
        }
        [[nodiscard]] uint64_t GetDeviceAddress() const override
        {
            return m_Address;
        }
        [[nodiscard]] uint64_t GetBytes() const override
        {
            return m_Ledger.GetBytes();
        }

    private:
        void Release();

        VkBuffer            m_Buffer      = VK_NULL_HANDLE;
        VmaAllocation       m_Memory      = nullptr;
        VkDeviceSize        m_Bytes       = 0;
        VkDeviceAddress     m_Address     = 0;
        uint32_t            m_VertexCount = 0;
        RDG::ExternalBuffer m_External;
        ResourceOwnership   m_Ledger;
    };

    class VulkanSkinnedRayTracingGeometry final : public RayTracing::SkinnedRayTracingGeometry
    {
    public:
        VulkanSkinnedRayTracingGeometry() = default;
        ~VulkanSkinnedRayTracingGeometry() override;
        VulkanSkinnedRayTracingGeometry( const VulkanSkinnedRayTracingGeometry& )            = delete;
        VulkanSkinnedRayTracingGeometry& operator=( const VulkanSkinnedRayTracingGeometry& ) = delete;

        [[nodiscard]] Common::ResultStr<RayTracing::SkinnedBlasPrepared>
                                           Prepare( const RayTracing::RayTracingMeshInput& input ) override;
        [[nodiscard]] RDG::ExternalBuffer& GetStorage() override
        {
            return m_StorageExternal;
        }
        [[nodiscard]] Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                         const RDG::BufferBinding& positions,
                                                         const RDG::BufferBinding* indices,
                                                         const RDG::BufferBinding& scratch,
                                                         uint64_t                  scratchOffset ) override;
        [[nodiscard]] uint64_t              GetDeviceAddress() const override
        {
            return m_DeviceAddress;
        }
        // The addresses are read now: a GeometryPool growth moves the mesh's buffers between frames.
        [[nodiscard]] RayTracing::RayTracingGeometryLayout GetMetadataLayout() const override;
        // The storage's ledger row is the one statement of what this BLAS costs (ResourceOwnership::GetBytes).
        [[nodiscard]] uint64_t GetMemoryBytes() const override
        {
            return m_StorageLedger.GetBytes();
        }

    private:
        void Release();

        // What the current storage was built for: a change of any of them is a full build of a new BLAS.
        // The mesh's spans (held: the build and the hit's attribute fetch read them by address).
        VertexBufferRange m_Vertices;
        IndexBufferRange  m_Indices;
        uint32_t          m_VertexCount = 0;

        std::vector<RayTracing::RayTracingGeometrySegment> m_Segments;
        VkDeviceSize    m_BuildScratch  = 0;
        VkDeviceSize    m_UpdateScratch = 0;
        bool            m_Built = false; // a build was recorded into the storage: the next one may refit it

        VkAccelerationStructureKHR            m_Handle        = VK_NULL_HANDLE;
        VkBuffer                              m_Storage       = VK_NULL_HANDLE;
        VmaAllocation                         m_StorageMemory = nullptr;
        VkDeviceSize                          m_StorageBytes  = 0;
        VkDeviceAddress                       m_DeviceAddress = 0;
        PFN_vkDestroyAccelerationStructureKHR m_Destroy       = nullptr;
        RDG::ExternalBuffer                   m_StorageExternal;
        ResourceOwnership                     m_StorageLedger;
    };
} // namespace Desert::Graphic::API::Vulkan
