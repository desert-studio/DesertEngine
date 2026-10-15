#pragma once

#include <vulkan/vulkan.hpp>

#include <cstdint>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanMaterialBackend;
    class DescriptorSetBuilder
    {
    public:
        static VkWriteDescriptorSet GetUniformWDS( VulkanMaterialBackend* materialBackend, const uint32_t frame,
                                                   const uint32_t set, const uint32_t dstBinding,
                                                   const uint32_t                descriptorCount,
                                                   const VkDescriptorBufferInfo* pBufferInfo );

        static VkWriteDescriptorSet GetStorageWDS( VulkanMaterialBackend* materialBackend, const uint32_t frame,
                                                   const uint32_t set, const uint32_t dstBinding,
                                                   const uint32_t                descriptorCount,
                                                   const VkDescriptorBufferInfo* pBufferInfo );

        // A separate texture (VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE): the info's view and layout, no sampler. There is
        // no combined image sampler in this engine; the texture's sampler state is a separate SAMPLER write.
        static VkWriteDescriptorSet GetSampledImageWDS( VulkanMaterialBackend* materialBackend,
                                                        const uint32_t frame, const uint32_t set,
                                                        const uint32_t               dstBinding,
                                                        const VkDescriptorImageInfo* pImageInfo );

        // A separate sampler state (VK_DESCRIPTOR_TYPE_SAMPLER): the info's sampler alone.
        static VkWriteDescriptorSet GetSamplerStateWDS( VulkanMaterialBackend* materialBackend,
                                                        const uint32_t frame, const uint32_t set,
                                                        const uint32_t               dstBinding,
                                                        const VkDescriptorImageInfo* pImageInfo );

        static VkWriteDescriptorSet GetStorageWDS( VulkanMaterialBackend* materialBackend, const uint32_t frame,
                                                   const uint32_t set, const uint32_t dstBinding,
                                                   const uint32_t               descriptorCount,
                                                   const VkDescriptorImageInfo* pImageInfo );
    };
} // namespace Desert::Graphic::API::Vulkan