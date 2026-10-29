#pragma once

#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneTarget.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

// THE VULKAN TLAS OF ONE VIEW (RayTracingSceneTarget). Owns the VkAccelerationStructureKHR and the buffer backing
// it; both live across frames and are recreated only when this frame's instance count needs more storage than the
// current one has (capacity grows to the next power of two, so a scene that adds one instance a frame does not
// recreate every frame). Built with ALLOW_UPDATE: a frame whose rows only moved updates it in place (the update
// keeps the last build's hierarchy and refits its bounds) instead of building it again. The storage is handed to each frame's graph as an external carrying its own state; the
// build itself is recorded by the graph pass (RayTracingSceneRenderer.hpp) into the pass's command buffer.
namespace Desert::Graphic::API::Vulkan
{
    class VulkanRayTracingScene final : public RayTracing::RayTracingSceneTarget
    {
    public:
        VulkanRayTracingScene() = default;
        ~VulkanRayTracingScene() override;
        VulkanRayTracingScene( const VulkanRayTracingScene& )            = delete;
        VulkanRayTracingScene& operator=( const VulkanRayTracingScene& ) = delete;

        [[nodiscard]] Common::ResultStr<RayTracing::TlasBuildSizes> Prepare( uint32_t instanceCount,
                                                                             bool     update ) override;
        [[nodiscard]] RDG::ExternalBuffer&                          GetStorage() override
        {
            return m_External;
        }
        [[nodiscard]] uint64_t GetMemoryBytes() const override
        {
            return m_Ledger.GetBytes();
        }
        [[nodiscard]] Common::BoolResultStr RecordBuild( const RDG::PassContext&   context,
                                                         const RDG::BufferBinding& instances,
                                                         const RDG::BufferBinding& scratch,
                                                         uint32_t instanceCount, bool update ) override;

        // What a descriptor write binds (VkWriteDescriptorSetAccelerationStructureKHR). Null before the first
        // Prepare.
        [[nodiscard]] VkAccelerationStructureKHR GetHandle() const
        {
            return m_Handle;
        }

    private:
        void Release();

        VkAccelerationStructureKHR            m_Handle        = VK_NULL_HANDLE;
        VkBuffer                              m_Storage       = VK_NULL_HANDLE;
        VmaAllocation                         m_StorageMemory = nullptr;
        VkDeviceSize                          m_StorageBytes  = 0;
        uint32_t                              m_Capacity      = 0; // instances the storage was sized for
        // The instance count of the last build or update RecordBuild recorded into this storage; kNoBuild before
        // the first (an update needs a source with the same count).
        static constexpr uint32_t             kNoBuild        = ~0u;
        uint32_t                              m_BuiltInstances = kNoBuild;
        PFN_vkDestroyAccelerationStructureKHR m_Destroy       = nullptr;
        RDG::ExternalBuffer                   m_External;
        ResourceOwnership                     m_Ledger; // the storage's row; reset by Release
    };
} // namespace Desert::Graphic::API::Vulkan
