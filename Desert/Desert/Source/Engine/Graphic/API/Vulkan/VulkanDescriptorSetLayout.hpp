#pragma once

// A descriptor set layout that OWNS its handle and is handed out by shared reference.
//
// WHY THIS EXISTS. A VkDescriptorSetLayout is not just an argument to vkCreatePipelineLayout and
// vkAllocateDescriptorSets — it is the contract those two agree on. The engine used to keep the
// layouts as bare handles inside VulkanShader and destroy them in place on every recompile, while a
// pipeline layout built from them, the material's descriptor sets and a compute pipeline's in-frame
// ring each captured that handle at a DIFFERENT moment. A hot reload between any two of those moments
// left the pipeline bound to one contract and the descriptor set allocated from another, which the
// validation layer reports as
//
//     VkDescriptorSetLayout from VkPipelineLayout has 8 total descriptors,
//     but VkDescriptorSetLayout, trying to bind, has 9 total descriptors
//
// — observed for real when a shader gained a binding while the editor was running, followed by
// "dstSet has been destroyed" and a pipeline referencing a layout that no longer existed.
//
// The fix is ownership, not call order: a layout is immutable once built, every consumer keeps a
// strong reference to the exact layout it used, and a recompile REPLACES the shader's references
// instead of destroying handles other objects are still standing on. A pipeline built yesterday keeps
// working against yesterday's contract until it is rebuilt, which is the honest behaviour — the
// alternative is a pipeline whose layout has been pulled out from under it.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanDescriptorSetLayout final
    {
    public:
        // Creates the layout. @p owner is the shader name, carried only so a mismatch can name itself.
        VulkanDescriptorSetLayout( VkDevice device, uint32_t set,
                                   const std::vector<VkDescriptorSetLayoutBinding>& bindings, std::string owner );
        // The same with per-binding flags (VkDescriptorSetLayoutBindingFlagsCreateInfo, one per binding) and
        // layout create flags: the bindless table's update-after-bind layout.
        VulkanDescriptorSetLayout( VkDevice device, uint32_t set,
                                   const std::vector<VkDescriptorSetLayoutBinding>& bindings, std::string owner,
                                   const std::vector<VkDescriptorBindingFlags>& bindingFlags,
                                   VkDescriptorSetLayoutCreateFlags             createFlags );
        ~VulkanDescriptorSetLayout();

        // Neither copyable nor movable: the handle's lifetime is this object's lifetime, and the whole
        // point is that the two cannot be separated.
        VulkanDescriptorSetLayout( const VulkanDescriptorSetLayout& )            = delete;
        VulkanDescriptorSetLayout& operator=( const VulkanDescriptorSetLayout& ) = delete;

        VkDescriptorSetLayout Handle() const
        {
            return m_Layout;
        }

        uint32_t Set() const
        {
            return m_Set;
        }

        // The sum of every binding's descriptorCount — the same "total descriptors" the validation
        // layer prints when it compares a bound set against a pipeline layout. Kept so a mismatch can
        // be reported in the layer's own vocabulary instead of "something differs".
        uint32_t DescriptorCount() const
        {
            return m_DescriptorCount;
        }

        const std::string& Owner() const
        {
            return m_Owner;
        }

        /**
         * The bindings this layout was built from, kept so a descriptor POOL can be sized against the
         * same contract the sets are allocated from. Sizing a pool from the shader's current reflection
         * instead is the same class of mistake as allocating from its current layout: after a recompile
         * the two describe different shaders, and the allocation fails (or, worse, succeeds short).
         */
        const std::vector<VkDescriptorSetLayoutBinding>& Bindings() const
        {
            return m_Bindings;
        }

    private:
        VkDevice                                  m_Device          = VK_NULL_HANDLE;
        VkDescriptorSetLayout                     m_Layout          = VK_NULL_HANDLE;
        uint32_t                                  m_Set             = 0;
        uint32_t                                  m_DescriptorCount = 0;
        std::vector<VkDescriptorSetLayoutBinding> m_Bindings;
        std::string                               m_Owner;
    };

    // How every consumer holds a layout. `const` because a built layout is a finished contract: the
    // only legal change is to build a new one and let the old one die when its last user does.
    using DescriptorSetLayoutRef = std::shared_ptr<const VulkanDescriptorSetLayout>;

    // The raw handles, in set order, for the Vk*CreateInfo that wants an array of them. The caller
    // must keep the refs it took these from alive for as long as the object it builds — which is why
    // every caller stores the vector of refs, not just this.
    std::vector<VkDescriptorSetLayout> RawHandles( const std::vector<DescriptorSetLayoutRef>& layouts );
} // namespace Desert::Graphic::API::Vulkan
