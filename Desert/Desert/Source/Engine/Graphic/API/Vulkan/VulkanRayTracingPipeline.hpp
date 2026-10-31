#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanDescriptorSetLayout.hpp>
#include <Engine/Graphic/RayTracingPipeline.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingHitGroupTable.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <vector>

// The Vulkan ray tracing pipeline (Graphic/RayTracingPipeline.hpp): one vkCreateRayTracingPipelinesKHR of the
// program's RayGen + Miss and one triangle hit group per hit cell (closest hit, any hit when the cell has one),
// its descriptor set layouts (the program's reflection, every binding visible to every ray tracing stage: the hit
// stages read the program's view block), and its PERSISTENT shader binding tables (UE:
// FRayTracingShaderBindingTable, kept per scene/pipeline and updated by dirty record): one table + material row
// buffer per (frame slot, trace of that pipeline in the frame), each kept across frames with a copy of the bytes
// last written into it; a trace writes only the records and rows that differ from that copy, so a still scene
// writes nothing once every slot has been written. A slot is written only by the frame of its own frame slot,
// after that slot's fence: never while the GPU reads it. Group order: 0 = ray generation, 1 = miss, 2 + i = hit
// group i.
namespace Desert::Graphic::API::Vulkan
{
    class VulkanRayTracingPipeline final : public RayTracingPipeline
    {
    public:
        explicit VulkanRayTracingPipeline( RayTracingPipelineSpecification spec );
        ~VulkanRayTracingPipeline() override;
        VulkanRayTracingPipeline( const VulkanRayTracingPipeline& )            = delete;
        VulkanRayTracingPipeline& operator=( const VulkanRayTracingPipeline& ) = delete;

        // Builds the layouts, the pipeline and reads the group handles. An error names the program, the hit
        // cell and what refused; the object is then unusable (GetVkPipeline() == VK_NULL_HANDLE).
        [[nodiscard]] Common::BoolResultStr Build();

        [[nodiscard]] const RayTracingPipelineSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard]] VkPipeline GetVkPipeline() const
        {
            return m_Pipeline;
        }
        [[nodiscard]] VkPipelineLayout GetVkPipelineLayout() const
        {
            return m_Layout;
        }
        [[nodiscard]] const std::vector<DescriptorSetLayoutRef>& GetLayouts() const
        {
            return m_SetLayouts;
        }
        [[nodiscard]] VkShaderStageFlags GetPushConstantStages() const
        {
            return m_PushStages;
        }

        // RECORD: this trace's shader binding table (the ray generation and miss handles, then one hit record per
        // @p table record: its group's handle + its RayTracingHitRecordData, MaterialRow relocated to the device
        // address of its row in the slot's row buffer), then vkCmdTraceRaysKHR over @p width x @p height. The
        // pipeline and its sets must be bound on @p cmd. The table and rows land in the slot of (this frame slot,
        // this pipeline's n-th trace of the frame); only the records and rows that differ from what that slot
        // holds are written (RayTracingSbtWrites counts them). A slot grows by capacity: the outgrown buffers go
        // through the allocator's frame-delayed deletion, and the grown slot is written whole.
        [[nodiscard]] Common::BoolResultStr RecordTraceRays( VkCommandBuffer                            cmd,
                                                             const RayTracing::RayTracingHitGroupTable& table,
                                                             uint32_t width, uint32_t height );

    private:
        struct HostBuffer
        {
            VkBuffer        Buffer     = VK_NULL_HANDLE;
            VmaAllocation   Allocation = nullptr;
            VkDeviceAddress Address    = 0;
        };
        [[nodiscard]] Common::ResultStr<HostBuffer> CreateHostBuffer( const char* tag, VkDeviceSize bytes,
                                                                      VkBufferUsageFlags usage ) const;
        void                                        Release( HostBuffer& buffer ) const;
        void                                        Destroy();

        // One persistent table the GPU reads in one trace, and the bytes last written into it (the compare base
        // of the next trace landing in this slot).
        struct TableSlot
        {
            HostBuffer             Sbt;
            HostBuffer             Rows;
            VkDeviceSize           SbtCapacity = 0; // table bytes the buffer holds after Base
            VkDeviceSize           RowCapacity = 0; // row bytes the buffer holds
            VkDeviceSize           Base = 0;    // the table's start: Sbt.Address rounded up to the base alignment
            std::vector<uint8_t>   Written;     // the table as written, from its start
            std::vector<glm::vec4> RowsWritten; // the rows as written
        };
        // The slot of this trace: m_Slots[frame slot][n-th trace of this pipeline in the frame], created on
        // demand.
        [[nodiscard]] TableSlot& SlotOfThisTrace();
        // Grows @p slot to hold @p sbtBytes of table and @p rowBytes of rows; a grown buffer starts empty (its
        // Written / RowsWritten cleared, so the trace writes it whole).
        [[nodiscard]] Common::BoolResultStr Reserve( TableSlot& slot, VkDeviceSize sbtBytes,
                                                     VkDeviceSize rowBytes );

        RayTracingPipelineSpecification     m_Specification;
        std::vector<DescriptorSetLayoutRef> m_SetLayouts;
        VkPipelineLayout                    m_Layout     = VK_NULL_HANDLE;
        VkPipeline                          m_Pipeline   = VK_NULL_HANDLE;
        VkShaderStageFlags                  m_PushStages = 0;
        std::vector<uint8_t>                m_Handles; // every group's handle, group order
        // VkPhysicalDeviceRayTracingPipelinePropertiesKHR, read at Build.
        uint32_t              m_HandleSize      = 0;
        uint32_t              m_HandleAlignment = 0;
        uint32_t              m_BaseAlignment   = 0;
        uint32_t              m_MaxStride       = 0;
        PFN_vkCmdTraceRaysKHR m_CmdTraceRays    = nullptr;
        std::mutex                          m_SlotsMutex; // a pipeline may be traced from two graphs' recordings
        std::vector<std::vector<TableSlot>> m_Slots;      // [frame slot][trace in the frame]
        uint64_t                            m_SlotsFrame    = UINT64_MAX; // absolute frame of m_TracesInFrame
        uint32_t                            m_TracesInFrame = 0;
        std::vector<uint8_t>                m_Table; // this trace's table, built before the compare
    };
} // namespace Desert::Graphic::API::Vulkan
