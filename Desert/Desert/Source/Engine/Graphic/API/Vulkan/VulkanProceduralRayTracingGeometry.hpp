#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanRayTracingBuildInput.hpp>
#include <Engine/Graphic/Systems/RayTracing/ProceduralRayTracingGeometry.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <vector>

namespace Desert::Graphic
{
    class IndexBuffer;
    class VertexBuffer;
} // namespace Desert::Graphic

// THE VULKAN BLAS OF ONE PROCEDURAL (mesh, hidden mask) (ProceduralRayTracingGeometry): built from the mesh's own
// vertex buffer (kStaticMeshVertexLayout) and index buffer by the graph pass "RayTracingScene.DynamicGeometry",
// PREFER_FAST_TRACE, once per buffer upload. Modelled on VulkanSkinnedRayTracingGeometry without the positions
// buffer and the refit.
namespace Desert::Graphic::API::Vulkan
{
    class VulkanProceduralRayTracingGeometry final : public RayTracing::ProceduralRayTracingGeometry
    {
    public:
        VulkanProceduralRayTracingGeometry() = default;
        ~VulkanProceduralRayTracingGeometry() override;
        VulkanProceduralRayTracingGeometry( const VulkanProceduralRayTracingGeometry& )            = delete;
        VulkanProceduralRayTracingGeometry& operator=( const VulkanProceduralRayTracingGeometry& ) = delete;

        [[nodiscard]] Common::ResultStr<RayTracing::ProceduralBlasPrepared>
                                           Prepare( const RayTracing::RayTracingMeshInput& input ) override;
        [[nodiscard]] RDG::ExternalBuffer& GetStorage() override
        {
            return m_StorageExternal;
        }
        [[nodiscard]] Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                         const RDG::BufferBinding& vertices,
                                                         const RDG::BufferBinding* indices,
                                                         const RDG::BufferBinding& scratch,
                                                         uint64_t                  scratchOffset ) override;
        [[nodiscard]] uint64_t              GetDeviceAddress() const override
        {
            return m_DeviceAddress;
        }
        [[nodiscard]] uint64_t GetMemoryBytes() const override
        {
            return m_StorageBytes;
        }

    private:
        void Release();

        // What the current storage was built for: a change of any of them is a new BLAS.
        std::shared_ptr<VertexBuffer>                      m_SourceVertices;
        std::shared_ptr<IndexBuffer>                       m_Indices; // held: the build reads it by address
        std::vector<RayTracing::RayTracingGeometrySegment> m_Segments;
        VkDeviceAddress                                    m_IndexAddress = 0;
        VkDeviceSize                                       m_BuildScratch = 0;
        bool                                               m_Built        = false;

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
