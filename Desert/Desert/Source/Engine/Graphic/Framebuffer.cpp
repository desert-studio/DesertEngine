#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{
    std::shared_ptr<Framebuffer> Framebuffer::Create( const FramebufferSpecification& spec )
    {
        return Renderer::GetInstance().Rhi().CreateFramebuffer( spec );
    }

} // namespace Desert::Graphic
