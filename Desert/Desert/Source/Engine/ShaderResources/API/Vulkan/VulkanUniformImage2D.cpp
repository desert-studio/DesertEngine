#include <Engine/ShaderResources/API/Vulkan/VulkanUniformImage2D.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderGraph.hpp>

namespace Desert::ShaderResources::API::Vulkan
{

    VulkanUniformImage2D::VulkanUniformImage2D( const std::string_view debugName, uint32_t binding )
         : m_DebugName( debugName ), m_Binding( binding )
    {
    }

    VulkanUniformImage2D::~VulkanUniformImage2D()
    {
    }

    void VulkanUniformImage2D::SetImage2D( const Graphic::Image2D* image2D, Graphic::RDG::Access declared )
    {
        if ( image2D )
        {
            const auto& res              = ( (Graphic::API::Vulkan::VulkanImage2D*)image2D )->GetResource();
            m_DescriptorInfo.imageView   = res.ImageView;
            m_DescriptorInfo.sampler     = res.Sampler;
            m_DescriptorInfo.imageLayout =
                 Graphic::API::Vulkan::RdgVulkanLayout( Graphic::RDG::GetAccessState( declared ).Layout );
        }
        else
        {
            m_DescriptorInfo = {};
        }
    }

} // namespace Desert::ShaderResources::API::Vulkan
