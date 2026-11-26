#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{
    std::shared_ptr<Framebuffer> Framebuffer::Create( const FramebufferSpecification& spec )
    {
        return Renderer::GetInstance().Rhi().CreateFramebuffer( spec );
    }

    auto FramebufferLibrary::GetFramebuffers() -> const auto&
    {
        return s_Framebuffers;
    }

    std::vector<std::shared_ptr<Graphic::Framebuffer>> FramebufferLibrary::s_Framebuffers;
}
