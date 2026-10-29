#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanDescriptorSetLayout.hpp>
#include <Engine/Graphic/RayTracingPipeline.hpp>
#include <Engine/Graphic/Systems/RayTracing/RayTracingHitGroupTable.hpp>

#include <VulkanAllocator/vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <vector>

// The Vulkan ray tracing pipeline (Graphic/RayTracingPipeline.hpp): one vkCreateRayTracingPipelinesKHR of the
// program's RayGen + Miss and one triangle hit group per hit cell (closest hit, any hit when the cell has one),
// its descriptor set layouts (the program's reflection, every binding visible to every ray tracing stage: the hit
// stages read the program's view block), and the shader binding table written per trace from the frame's
// RayTracingHitGroupTable. Group order: 0 = ray generation, 1 = miss, 2 + i = hit group i.
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
        // address of its row in this trace's row buffer), then vkCmdTraceRaysKHR over @p width x @p height. The
        // pipeline and its sets must be bound on @p cmd. Both buffers are this trace's own: the previous trace's
        // are released through the allocator's frame-delayed deletion, so a frame in flight keeps reading its own.
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
        HostBuffer            m_Sbt;
        HostBuffer            m_Rows;
    };
} // namespace Desert::Graphic::API::Vulkan
