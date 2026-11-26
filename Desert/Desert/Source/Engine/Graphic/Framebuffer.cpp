#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

#include <Engine/Graphic/API/Vulkan/VulkanFramebuffer.hpp>

namespace Desert::Graphic
{
    std::shared_ptr<Framebuffer> Framebuffer::Create( const FramebufferSpecification& spec )
    {
        switch ( RendererAPI::GetAPIType() )
        {
            case RendererAPIType::None:
                return nullptr;
            case RendererAPIType::Vulkan:
            {
                const auto framebuffer = std::make_shared<API::Vulkan::VulkanFramebuffer>( spec );
             //   FramebufferLibrary::s_Framebuffers.push_back( framebuffer );

                return framebuffer;
            }
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return nullptr;
    }

    auto FramebufferLibrary::GetFramebuffers() -> const auto&
    {
        return s_Framebuffers;
    }

    std::vector<std::shared_ptr<Graphic::Framebuffer>> FramebufferLibrary::s_Framebuffers;
}
