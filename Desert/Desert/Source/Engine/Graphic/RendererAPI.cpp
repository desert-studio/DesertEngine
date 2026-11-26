#include <Engine/Graphic/RendererAPI.hpp>

namespace Desert::Graphic
{
    auto RendererAPI::GetAPIType() -> RendererAPIType
    {
        return s_RenderingAPI;
    }

    RendererAPIType RendererAPI::s_RenderingAPI = RendererAPIType::Vulkan;
}
