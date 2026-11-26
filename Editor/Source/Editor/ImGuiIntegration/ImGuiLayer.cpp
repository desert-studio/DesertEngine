#include <Editor/ImGuiIntegration/ImGuiLayer.hpp>

#include <Editor/ImGuiIntegration/VulkanImGuiLayer.hpp>

#include <Common/Core/Core.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/Rhi/Rhi.hpp>

namespace Desert::ImGui
{

    std::shared_ptr<Desert::ImGui::ImGuiLayer> ImGuiLayer::Create()
    {
        // The editor's UI backend follows the process's rendering backend (RhiKind.hpp names this the one
        // switch on it outside Graphic/Rhi). A D3D12 process never reaches here: CreateRhi refuses d3d12 until
        // its renderer exists, and the refusal is stated, not a Vulkan layer on a D3D12 device.
        const Graphic::RhiKind kind = Graphic::Renderer::GetInstance().Rhi().Kind();
        switch ( kind )
        {
            case Graphic::RhiKind::Vulkan:
                return std::make_shared<Graphic::API::Vulkan::VulkanImGui>();
            case Graphic::RhiKind::D3D12:
                break;
        }
        LOG_ERROR( "[ImGui] no editor UI backend for rendering backend {}", Graphic::ToString( kind ) );
        return nullptr;
    }

} // namespace Desert::ImGui
