#include <Engine/Graphic/API/Vulkan/VulkanDescriptorSetLayout.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>

namespace Desert::Graphic::API::Vulkan
{
    VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(
         VkDevice device, uint32_t set, const std::vector<VkDescriptorSetLayoutBinding>& bindings,
         std::string owner )
         : VulkanDescriptorSetLayout( device, set, bindings, std::move( owner ), {}, 0 )
    {
    }

    VulkanDescriptorSetLayout::VulkanDescriptorSetLayout(
         VkDevice device, uint32_t set, const std::vector<VkDescriptorSetLayoutBinding>& bindings,
         std::string owner, const std::vector<VkDescriptorBindingFlags>& bindingFlags,
         VkDescriptorSetLayoutCreateFlags createFlags )
         : m_Device( device ), m_Set( set ), m_Bindings( bindings ), m_Owner( std::move( owner ) )
    {
        for ( const auto& binding : m_Bindings )
            m_DescriptorCount += binding.descriptorCount;

        DESERT_VERIFY( bindingFlags.empty() || bindingFlags.size() == m_Bindings.size(),
                       "Shader '{}': set {} has {} bindings and {} binding flags", m_Owner, m_Set,
                       m_Bindings.size(), bindingFlags.size() );
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
             .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
             .bindingCount  = static_cast<uint32_t>( bindingFlags.size() ),
             .pBindingFlags = bindingFlags.data() };
        const VkDescriptorSetLayoutCreateInfo createInfo{
             .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
             .pNext        = bindingFlags.empty() ? nullptr : &flagsInfo,
             .flags        = createFlags,
             .bindingCount = static_cast<uint32_t>( m_Bindings.size() ),
             .pBindings    = m_Bindings.data() };

        if ( vkCreateDescriptorSetLayout( m_Device, &createInfo, nullptr, &m_Layout ) != VK_SUCCESS )
        {
            // Named, with the numbers, rather than left as a null handle to surface later as an
            // unrelated allocation failure three call sites away.
            LOG_ERROR( "Shader '{}': descriptor set layout {} ({} bindings, {} descriptors) could not be "
                       "created",
                       m_Owner, m_Set, bindings.size(), m_DescriptorCount );
            m_Layout = VK_NULL_HANDLE;
        }
    }

    VulkanDescriptorSetLayout::~VulkanDescriptorSetLayout()
    {
        if ( m_Layout != VK_NULL_HANDLE )
            vkDestroyDescriptorSetLayout( m_Device, m_Layout, nullptr );
    }

    std::vector<VkDescriptorSetLayout> RawHandles( const std::vector<DescriptorSetLayoutRef>& layouts )
    {
        std::vector<VkDescriptorSetLayout> handles;
        handles.reserve( layouts.size() );
        for ( const auto& layout : layouts )
            handles.push_back( layout ? layout->Handle() : VK_NULL_HANDLE );
        return handles;
    }
} // namespace Desert::Graphic::API::Vulkan
