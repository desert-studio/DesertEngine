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

        static VkWriteDescriptorSet GetSampler2DWDS( VulkanMaterialBackend* materialBackend, const uint32_t frame,
                                                     const uint32_t set, const uint32_t dstBinding,
                                                     const uint32_t               descriptorCount,
                                                     const VkDescriptorImageInfo* pImageInfo );

        static VkWriteDescriptorSet GetSamplerCubeWDS( VulkanMaterialBackend* materialBackend,
                                                       const uint32_t frame, const uint32_t set,
                                                       const uint32_t dstBinding, const uint32_t descriptorCount,
                                                       const VkDescriptorImageInfo* pImageInfo );

        // Volume sampler (`sampler3D`). Unlike the 2D and cube variants there is deliberately NO fallback
        // texture: the fallbacks are 2D images, and a 2D view bound to a sampler3D does not fail — it
        // samples garbage. Callers must therefore validate the image info before calling.
        static VkWriteDescriptorSet GetSampler3DWDS( VulkanMaterialBackend* materialBackend, const uint32_t frame,
                                                     const uint32_t set, const uint32_t dstBinding,
                                                     const uint32_t               descriptorCount,
                                                     const VkDescriptorImageInfo* pImageInfo );

        static VkWriteDescriptorSet GetStorageWDS( VulkanMaterialBackend* materialBackend, const uint32_t frame,
                                                   const uint32_t set, const uint32_t dstBinding,
                                                   const uint32_t               descriptorCount,
                                                   const VkDescriptorImageInfo* pImageInfo );
    };
} // namespace Desert::Graphic::API::Vulkan