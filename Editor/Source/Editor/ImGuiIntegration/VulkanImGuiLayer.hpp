#pragma once

#include <Engine/Graphic/DisplayOutput.hpp>
#include <Engine/Graphic/Materials/MaterialExecutor.hpp>
#include <Engine/Graphic/Pipeline.hpp>
#include <Engine/Graphic/ShaderBindingLayoutCache.hpp>

#include <array>
#include <cstddef>
#include <memory>

#include <Editor/ImGuiIntegration/ImGuiLayer.hpp>

// BOOLSUCCESS lives here. It used to arrive by accident: this header was compiled only as part of the
// engine, whose build force-includes pch.hpp, and pch.hpp opens with Common/Core/Core.hpp. The Editor
// has no forced prefix header, so the macro has to be asked for by name.
#include <Common/Core/Core.hpp>
#include <Common/Core/Timestep.hpp>

#include <vulkan/vulkan.h>

struct ImDrawData;

namespace Desert::Graphic::API::Vulkan
{
    class VulkanImGui final : public Desert::ImGui::ImGuiLayer
    {
    public:
        VulkanImGui();
        ~VulkanImGui();

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

        // THE INTERFACE'S DRAW BUFFERS (UE FSlateDrawBuffer, FSlateRenderer::NumDrawBuffers = 3). End() copies the
        // frame's ImGui draw data into a free one on the game thread and hands it to the render task, which
        // records the interface graph from that copy and frees the buffer when it is done. ImGui's own draw data
        // lives only until the next ImGui::NewFrame; the copy does not, so the interface graph may record while
        // the game thread builds the next frame. Every ImGui allocation (the copied lists) happens on the game
        // thread.
        struct InterfaceDrawBuffer;
        static constexpr std::size_t kInterfaceDrawBuffers = 3;
        [[nodiscard]] InterfaceDrawBuffer& AcquireDrawBuffer();
        // The interface graph from a draw buffer's copy: on the render task, behind the frame's views (End).
        [[nodiscard]] Common::BoolResultStr RecordInterface( ImDrawData& interfaceData );
        std::array<std::unique_ptr<InterfaceDrawBuffer>, kInterfaceDrawBuffers> m_DrawBuffers;
    };
} // namespace Desert::Graphic::API::Vulkan
