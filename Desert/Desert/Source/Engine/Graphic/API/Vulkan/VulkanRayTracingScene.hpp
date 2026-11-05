#pragma once

#include <Engine/Graphic/Systems/RayTracing/RayTracingSceneTarget.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>

// THE VULKAN TLAS OF ONE VIEW (RayTracingSceneTarget). Owns the VkAccelerationStructureKHR and the buffer backing
// it; both live across frames and are recreated only when this frame's instance count needs more storage than the
// current one has (capacity grows to the next power of two, so a scene that adds one instance a frame does not
// recreate every frame). Built with ALLOW_UPDATE: a frame whose rows only moved updates it in place (the update
// keeps the last build's hierarchy and refits its bounds) instead of building it again. The storage is handed to
// each frame's graph as an external carrying its own state; the build itself is recorded by the graph pass
// (RayTracingSceneRenderer.hpp) into the pass's command buffer, and is the storage's only once that graph ended.
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
        [[nodiscard]] Common::ResultStr<bool> EnsureRows( RayTracing::SceneRowBuffer buffer,
                                                          uint64_t                   bytes ) override;
        [[nodiscard]] RDG::ExternalBuffer&    GetRows( const RayTracing::SceneRowBuffer buffer ) override
        {
            return m_Rows[RowIndex( buffer )].External;
        }
        [[nodiscard]] uint64_t GetRowMemoryBytes( const RayTracing::SceneRowBuffer buffer ) const override
        {
            return m_Rows[RowIndex( buffer )].Ledger.GetBytes();
        }
        [[nodiscard]] uint64_t GetRowGraphsEnded( const RayTracing::SceneRowBuffer buffer ) const override
        {
            return m_Rows[RowIndex( buffer )].GraphsEnded;
        }
        [[nodiscard]] Common::BoolResultStr RecordRowScatter( const RDG::PassContext&                  context,
                                                              const RDG::BufferBinding&                staging,
                                                              const RDG::BufferBinding&                rows,
                                                              std::span<const RayTracing::SceneRowRun> runs,
                                                              uint32_t rowBytes ) override;

        // What a descriptor write binds (VkWriteDescriptorSetAccelerationStructureKHR). Null before the first
        // Prepare.
        [[nodiscard]] VkAccelerationStructureKHR GetHandle() const
        {
            return m_Handle;
        }

    private:
        // One persistent row buffer (RayTracingSceneTarget::EnsureRows), apart from the storage: a TLAS recreated
        // for a larger scene keeps them.
        struct RowBuffer
        {
            VkBuffer            Buffer = VK_NULL_HANDLE;
            VmaAllocation       Memory = nullptr;
            uint64_t            Bytes  = 0; // the capacity
            RDG::ExternalBuffer External;
            ResourceOwnership   Ledger;          // reset by ReleaseRows
            uint64_t            GraphsEnded = 0; // GetRowGraphsEnded; ReleaseRows keeps it
        };
        static constexpr std::size_t kRowBufferCount =
             static_cast<std::size_t>( RayTracing::SceneRowBuffer::Count );
        // An out-of-range kind names the last buffer: EnsureRows refuses it before any caller can reach here.
        [[nodiscard]] static std::size_t RowIndex( const RayTracing::SceneRowBuffer buffer )
        {
            return std::min( static_cast<std::size_t>( buffer ), kRowBufferCount - 1u );
        }

        void Release();
        static void ReleaseRows( RowBuffer& rows );

        VkAccelerationStructureKHR            m_Handle        = VK_NULL_HANDLE;
        VkBuffer                              m_Storage       = VK_NULL_HANDLE;
        VmaAllocation                         m_StorageMemory = nullptr;
        VkDeviceSize                          m_StorageBytes  = 0;
        uint32_t                              m_Capacity      = 0; // instances the storage was sized for
        // THE BUILD COMMITS ON GRAPH END (UE: a GPU state is the submitted graph's, never its exec's).
        // m_RecordedInstances: the instance count of the build or update RecordBuild recorded into the current
        // graph, kNoBuild when none is pending. m_BuiltInstances: of the last one whose graph ended (the storage
        // external's RecordFinalState, which RDG calls only after EndGraph succeeded); kNoBuild before the first,
        // and after a recorded one whose graph was abandoned (Prepare finds it still pending): the renderer
        // counted that build as done, so the next frame builds (an update needs a source with the same count).
        static constexpr uint32_t              kNoBuild            = ~0u;
        uint32_t                               m_BuiltInstances    = kNoBuild;
        uint32_t                               m_RecordedInstances = kNoBuild;
        PFN_vkDestroyAccelerationStructureKHR m_Destroy       = nullptr;
        RDG::ExternalBuffer                   m_External;
        ResourceOwnership                     m_Ledger; // the storage's row; reset by Release
        std::array<RowBuffer, kRowBufferCount> m_Rows;
    };
} // namespace Desert::Graphic::API::Vulkan
