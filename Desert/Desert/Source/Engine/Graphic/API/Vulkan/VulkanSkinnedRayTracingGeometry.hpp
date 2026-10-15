#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingSkinCache.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

namespace Desert::Graphic
{
    class IndexBuffer;
    class VertexBuffer;
} // namespace Desert::Graphic

// THE VULKAN SKIN-CACHE ENTRY OF ONE SKINNED PRIMITIVE (SkinnedRayTracingGeometry): the cached positions buffer
// the "SkinCache: Positions" pass writes and the BLAS built from it. Both live across frames; they are recreated
// only when the mesh's buffers or the hidden mask change, and every other frame the BLAS is REFIT in place (UE
// FRayTracingDynamicGeometryUpdateParams with bAllowUpdate: ALLOW_UPDATE + PREFER_FAST_BUILD, MODE_UPDATE with
// src == dst). The build is recorded by the graph pass "RayTracingScene.DynamicGeometry"
// (RayTracingSkinCache.cpp).
namespace Desert::Graphic::API::Vulkan
{
    class VulkanSkinnedRayTracingGeometry final : public RayTracing::SkinnedRayTracingGeometry
    {
    public:
        VulkanSkinnedRayTracingGeometry() = default;
        ~VulkanSkinnedRayTracingGeometry() override;
        VulkanSkinnedRayTracingGeometry( const VulkanSkinnedRayTracingGeometry& )            = delete;
        VulkanSkinnedRayTracingGeometry& operator=( const VulkanSkinnedRayTracingGeometry& ) = delete;

        [[nodiscard]] Common::ResultStr<RayTracing::SkinnedBlasPrepared>
                                           Prepare( const SkinnedMesh& mesh, uint64_t hiddenSubmeshes ) override;
        [[nodiscard]] RDG::ExternalBuffer& GetPositions() override
        {
            return m_PositionsExternal;
        }
        [[nodiscard]] RDG::ExternalBuffer& GetStorage() override
        {
            return m_StorageExternal;
        }
        [[nodiscard]] Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                         const RDG::BufferBinding& positions,
                                                         const RDG::BufferBinding& scratch,
                                                         uint64_t                  scratchOffset ) override;
        [[nodiscard]] uint64_t              GetDeviceAddress() const override
        {
            return m_DeviceAddress;
        }
        [[nodiscard]] RayTracing::RayTracingGeometryLayout GetMetadataLayout() const override
        {
            return RayTracing::MeshAttributeLayout( m_VertexAddress, m_IndexAddress, m_Segments, true );
        }
        [[nodiscard]] uint64_t GetMemoryBytes() const override
        {
            return m_StorageBytes;
        }
        [[nodiscard]] uint64_t GetPositionsBytes() const override
        {
            return m_PositionsBytes;
        }

    private:
        void Release();

        // What the current storage was built for: a change of any of them is a full build of a new BLAS.
        std::shared_ptr<VertexBuffer> m_SourceVertices;
        std::shared_ptr<IndexBuffer>  m_Indices; // held: the BLAS build reads it by address
        uint64_t                      m_HiddenSubmeshes = 0;
        uint32_t                      m_VertexCount     = 0;

        std::vector<RayTracing::RayTracingGeometrySegment> m_Segments;
        VkDeviceAddress m_VertexAddress = 0; // the mesh's own vertex buffer: the hit's attribute stream
        VkDeviceAddress                                    m_IndexAddress  = 0;
        VkDeviceSize                                       m_BuildScratch  = 0;
        VkDeviceSize                                       m_UpdateScratch = 0;
        bool m_Built = false; // a build was recorded into the storage: the next one may refit it

        VkBuffer            m_Positions       = VK_NULL_HANDLE;
        VmaAllocation       m_PositionsMemory = nullptr;
        VkDeviceSize        m_PositionsBytes  = 0;
        RDG::ExternalBuffer m_PositionsExternal;
        ResourceOwnership   m_PositionsLedger;

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
