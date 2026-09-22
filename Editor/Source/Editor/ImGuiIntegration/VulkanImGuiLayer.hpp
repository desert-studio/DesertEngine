#pragma once

#include <Editor/ImGuiIntegration/ImGuiLayer.hpp>
#include <Common/Core/Timestep.hpp>

#include <vulkan/vulkan.h>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanImGui final : public Desert::ImGui::ImGuiLayer
    {
    public:
        virtual Common::BoolResultStr OnAttach() override;
        virtual Common::BoolResultStr OnDetach() override;
        virtual Common::BoolResultStr OnUpdate( const Common::Timestep& ts ) override;
        virtual void               OnEvent( Common::Event& event ) override;
        virtual void               Begin() override;
        virtual void               End() override;

        virtual Common::BoolResultStr OnUIRender() override
        {
            return BOOLSUCCESS;
        }

    private:
        VkDescriptorPool m_ImguiPool = VK_NULL_HANDLE;
    };
} // namespace Desert::Graphic::API::Vulkan
