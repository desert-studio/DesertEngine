#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace Desert::Graphic::API::Vulkan
{
    // THE QUEUE-FAMILY SHARING RULE OF EVERY BUFFER AND IMAGE THE ENGINE CREATES (UE VulkanRHI:
    // r.Vulkan.AllowConcurrentBuffer / r.Vulkan.AllowConcurrentImage, both on by default: on a device with more
    // than one active queue family a resource is VK_SHARING_MODE_CONCURRENT across all of them).
    //
    // WHY CONCURRENT AND NOT ONLY THE GRAPH'S OWNERSHIP TRANSFERS: the render graph emits a release / acquire pair
    // at every graphics -> async compute -> graphics crossing of a resource it SEES (RDGCompile.cpp, TrackUse
    // crossPipes and CollectFinalBarriers). It cannot see a read it was never told about: a bindless texture
    // (BindlessTextureTable: uploaded images reached as BindlessTextures[slot], never declared), a buffer reached
    // through a device address, or an upload a compute one-off reads outside any graph. On an EXCLUSIVE resource
    // each of those is a read on a family that does not own it, which a semaphore does not fix (it orders
    // execution, it transfers nothing) and whose contents the spec leaves undefined. So the device-wide rule is
    // CONCURRENT, and the graph's pairs are realised per resource by VulkanRdgBackend::RecordBarriers: on an
    // EXCLUSIVE resource (the swapchain image, the only one) a queue family ownership transfer; on a CONCURRENT
    // one the same two halves without family indices (the release carries the layout transition on the source
    // queue, the acquire makes it visible on the destination queue in the layout it is now in).
    //
    // The families are every DISTINCT family the logical device created a queue for (VulkanLogicalDevice::
    // Create: graphics, plus compute and transfer where they are other families). One family: EXCLUSIVE, and
    // nothing changes on a device that has no second queue (MoltenVK, most mobile).
    struct VulkanResourceSharing
    {
        uint32_t                FamilyCount = 1;
        std::array<uint32_t, 3> Families{};

        [[nodiscard]] static VulkanResourceSharing FromFamilies( uint32_t graphics, uint32_t compute,
                                                                 uint32_t transfer )
        {
            VulkanResourceSharing sharing;
            sharing.Families[0] = graphics;
            for ( const uint32_t family : { compute, transfer } )
            {
                bool seen = false;
                for ( uint32_t i = 0; i < sharing.FamilyCount; ++i )
                    seen = seen || sharing.Families[i] == family;
                if ( !seen )
                    sharing.Families[sharing.FamilyCount++] = family;
            }
            return sharing;
        }

        [[nodiscard]] bool IsConcurrent() const
        {
            return FamilyCount > 1;
        }

        [[nodiscard]] bool Contains( uint32_t family ) const
        {
            for ( uint32_t i = 0; i < FamilyCount; ++i )
                if ( Families[i] == family )
                    return true;
            return false;
        }

        // The create info points at THIS object's family array: it must outlive the vkCreate* call (the device's
        // copy does, VulkanPhysicalDevice::GetResourceSharing).
        void ApplyTo( VkImageCreateInfo& info ) const
        {
            info.sharingMode           = IsConcurrent() ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
            info.queueFamilyIndexCount = IsConcurrent() ? FamilyCount : 0u;
            info.pQueueFamilyIndices   = IsConcurrent() ? Families.data() : nullptr;
        }
        void ApplyTo( VkBufferCreateInfo& info ) const
        {
            info.sharingMode           = IsConcurrent() ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
            info.queueFamilyIndexCount = IsConcurrent() ? FamilyCount : 0u;
            info.pQueueFamilyIndices   = IsConcurrent() ? Families.data() : nullptr;
        }
    };
} // namespace Desert::Graphic::API::Vulkan
