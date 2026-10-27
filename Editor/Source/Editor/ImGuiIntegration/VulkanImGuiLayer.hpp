#pragma once

#include <Engine/Graphic/DisplayOutput.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <memory>

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

        // HDR-OUTPUT: THE UI COMPOSITE. The interface is drawn into a half-float target (kUICompositeFormat, one
        // format whatever the display, so the backend's pipeline never has to be rebuilt when the output
        // changes), then encoded into the back buffer by the display's encode: SwapchainBlit on SDR (a copy,
        // quantised once), DisplayEncodeScRGB on scRGB (extended sRGB -> linear; UI on paper white). Rebuilt
        // when the swapchain's composite framebuffer or the output changes.
        [[nodiscard]] Common::BoolResultStr EnsureDisplayEncode();
        std::shared_ptr<Graphic::Framebuffer>        m_EncodeFramebuffer;
        Common::Scalability::DisplayOutput            m_EncodeOutput = Common::Scalability::DisplayOutput::SDR_sRGB;
        std::shared_ptr<Graphic::GraphicsPipeline>   m_EncodePipeline;
        std::unique_ptr<Graphic::MaterialExecutor>   m_EncodeExecutor;
        Graphic::ShaderBindingLayoutCache            m_EncodeLayout;
    };
} // namespace Desert::Graphic::API::Vulkan
