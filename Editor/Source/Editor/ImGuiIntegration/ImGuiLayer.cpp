#include <Editor/ImGuiIntegration/ImGuiLayer.hpp>

#include <Editor/ImGuiIntegration/VulkanImGuiLayer.hpp>

#include <Common/Core/Core.hpp>
#include <Engine/Graphic/RendererAPI.hpp>

namespace Desert::ImGui
{

    std::shared_ptr<Desert::ImGui::ImGuiLayer> ImGuiLayer::Create()
    {
        switch ( Graphic::RendererAPI::GetAPIType() )
        {
            case Graphic::RendererAPIType::None:
                return nullptr;
            case Graphic::RendererAPIType::Vulkan:
                return std::make_shared<Graphic::API::Vulkan::VulkanImGui>();
        }
        DESERT_VERIFY( false, "Unknown RenderingAPI" );
        return nullptr;
    }

} // namespace Desert::ImGui
