#include <Engine/Graphic/API/Vulkan/VulkanUtils/WriteDescriptorSetBuilder.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanMaterialBackend.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/Renderer.hpp>

namespace Desert::Graphic::API::Vulkan
{

    VkWriteDescriptorSet DescriptorSetBuilder::GetUniformWDS( VulkanMaterialBackend* materialBackend,
                                                              const uint32_t frame, const uint32_t set,
                                                              const uint32_t                dstBinding,
                                                              const uint32_t                descriptorCount,
                                                              const VkDescriptorBufferInfo* pBufferInfo )
    {
        VkDescriptorSet descriptorSet = materialBackend->GetDescriptorSet( frame, set );
        if ( descriptorSet == VK_NULL_HANDLE )
        {
            VkWriteDescriptorSet writeDescriptor = {};
            writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            return writeDescriptor;
        }

        VkWriteDescriptorSet writeDescriptor = {};
        writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writeDescriptor.dstSet               = descriptorSet;
        writeDescriptor.dstBinding           = dstBinding;
        writeDescriptor.descriptorCount      = descriptorCount;
        writeDescriptor.pBufferInfo          = pBufferInfo;

        return writeDescriptor;
    }

    VkWriteDescriptorSet DescriptorSetBuilder::GetStorageWDS( VulkanMaterialBackend* materialBackend,
                                                              const uint32_t frame, const uint32_t set,
                                                              const uint32_t                dstBinding,
                                                              const uint32_t                descriptorCount,
                                                              const VkDescriptorBufferInfo* pBufferInfo )
    {
        VkDescriptorSet descriptorSet = materialBackend->GetDescriptorSet( frame, set );
        if ( descriptorSet == VK_NULL_HANDLE )
        {
            VkWriteDescriptorSet writeDescriptor = {};
            writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            return writeDescriptor;
        }

        VkWriteDescriptorSet writeDescriptor = {};
        writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writeDescriptor.dstSet               = descriptorSet;
        writeDescriptor.dstBinding           = dstBinding;
        writeDescriptor.descriptorCount      = descriptorCount;
        writeDescriptor.pBufferInfo          = pBufferInfo;

        return writeDescriptor;
    }

    namespace
    {
        VkWriteDescriptorSet ImageInfoWDS( VulkanMaterialBackend* materialBackend, const uint32_t frame,
                                           const uint32_t set, const uint32_t dstBinding,
                                           const VkDescriptorImageInfo* pImageInfo, VkDescriptorType type )
        {
            VkWriteDescriptorSet writeDescriptor = {};
            writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            VkDescriptorSet descriptorSet        = materialBackend->GetDescriptorSet( frame, set );
            if ( descriptorSet == VK_NULL_HANDLE )
            {
                writeDescriptor.descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
                return writeDescriptor;
            }
            writeDescriptor.descriptorType  = type;
            writeDescriptor.dstSet          = descriptorSet;
            writeDescriptor.dstBinding      = dstBinding;
            writeDescriptor.descriptorCount = 1;
            writeDescriptor.pImageInfo      = pImageInfo;
            return writeDescriptor;
        }
    } // namespace

    VkWriteDescriptorSet DescriptorSetBuilder::GetSampledImageWDS( VulkanMaterialBackend* materialBackend,
                                                                   const uint32_t frame, const uint32_t set,
                                                                   const uint32_t               dstBinding,
                                                                   const VkDescriptorImageInfo* pImageInfo )
    {
        return ImageInfoWDS( materialBackend, frame, set, dstBinding, pImageInfo, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE );
    }

    VkWriteDescriptorSet DescriptorSetBuilder::GetSamplerStateWDS( VulkanMaterialBackend* materialBackend,
                                                                   const uint32_t frame, const uint32_t set,
                                                                   const uint32_t               dstBinding,
                                                                   const VkDescriptorImageInfo* pImageInfo )
    {
        return ImageInfoWDS( materialBackend, frame, set, dstBinding, pImageInfo, VK_DESCRIPTOR_TYPE_SAMPLER );
    }

    VkWriteDescriptorSet DescriptorSetBuilder::GetStorageWDS( VulkanMaterialBackend* materialBackend,
                                                              const uint32_t frame, const uint32_t set,
                                                              const uint32_t               dstBinding,
                                                              const uint32_t               descriptorCount,
                                                              const VkDescriptorImageInfo* pImageInfo )
    {
        VkDescriptorSet descriptorSet = materialBackend->GetDescriptorSet( frame, set );
        if ( descriptorSet == VK_NULL_HANDLE )
        {
            VkWriteDescriptorSet writeDescriptor = {};
            writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            return writeDescriptor;
        }

        VkWriteDescriptorSet writeDescriptor = {};
        writeDescriptor.sType                = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptor.descriptorType       = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writeDescriptor.dstSet               = descriptorSet;
        writeDescriptor.dstBinding           = dstBinding;
        writeDescriptor.descriptorCount      = descriptorCount;
        writeDescriptor.pImageInfo           = pImageInfo;
        return writeDescriptor;
    }

} // namespace Desert::Graphic::API::Vulkan
