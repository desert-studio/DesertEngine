#pragma once

#include <Editor/ImGuiIntegration/ImGuiLayer.hpp>

// BOOLSUCCESS lives here. It used to arrive by accident: this header was compiled only as part of the
// engine, whose premake force-includes pch.hpp, and pch.hpp opens with Common/Core/Core.hpp. The Editor
// has no forced prefix header, so the macro has to be asked for by name.
#include <Common/Core/Core.hpp>
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
        virtual void                  OnEvent( Common::Event& event ) override;
        virtual void                  Begin() override;
        virtual void                  End() override;

        virtual Common::BoolResultStr OnUIRender() override
        {
            return BOOLSUCCESS;
        }

    private:
        VkDescriptorPool m_ImguiPool = VK_NULL_HANDLE;
    };
} // namespace Desert::Graphic::API::Vulkan
