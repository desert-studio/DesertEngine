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
        Common::BoolResultStr OnAttach() override;
        Common::BoolResultStr OnDetach() override;
        Common::BoolResultStr OnUpdate( const Common::Timestep& ts ) override;
        void                  OnEvent( Common::Event& event ) override;
        void                  Begin() override;
        void                  End() override;

        Common::BoolResultStr OnUIRender() override
        {
            return BOOLSUCCESS;
        }

    private:
        VkDescriptorPool m_ImguiPool = VK_NULL_HANDLE;
        // The canonical render-graph render pass of the back buffer's format (CreateRdgRenderPass): the ImGui
        // backend builds its pipeline against it, so the pipeline draws inside the graph node of End(), whose
        // render pass the graph backend builds for the same format. Owned here for the backend's lifetime.
        VkRenderPass m_ImguiRenderPass = VK_NULL_HANDLE;
    };
} // namespace Desert::Graphic::API::Vulkan
