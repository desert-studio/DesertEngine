#include <Engine/Graphic/RendererContext.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{
    std::shared_ptr<RendererContext> RendererContext::Create( const std::shared_ptr<Window>& window )
    {
        return Renderer::GetInstance().Rhi().CreateRendererContext( window );
    }

} // namespace Desert::Graphic
