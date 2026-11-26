#include "SwapChain.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::Graphic
{
    std::shared_ptr<SwapChain> SwapChain::Create( const GLFWwindow* window )
    {
        return Renderer::GetInstance().Rhi().CreateSwapChain( window );
    }
} // namespace Desert::Graphic
