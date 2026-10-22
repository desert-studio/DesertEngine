#pragma once

#include <vulkan/vulkan.h>

#include <string_view>

#include <Engine/Graphic/RendererContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

namespace vkb
{
    struct Instance;
} // namespace vkb

namespace Desert::Graphic::API::Vulkan
{
    // The vkCreateGraphicsPipelines call running on THIS thread: its pipeline's name and colour blend state, so
    // the debug callback (VulkanContext.cpp) can tell a driver message ABOUT that create from the create's own
    // state. MoltenVK 1.4.2 reports "Blending is enabled for attachment with format X, which does not support
    // it." for every colour attachment whose format cannot blend, WITHOUT reading blendEnable
    // (MVKPipeline.mm:1990 `else if (mtlPixFmt && !supportsBlend)`; KhronosGroup/MoltenVK#2811, fixed upstream
    // f92b64c 2026-08-25 by adding `&& pCA->blendEnable`, no release carries it yet). The G-buffer's RG32_UINT
    // shading word is such a slot, never blended (Pipeline.hpp ColourAttachmentBlends). Held for the duration
    // of the create only; nests nothing (one create per thread at a time).
    class PipelineCreateScope
    {
    public:
        PipelineCreateScope( std::string_view name, const VkPipelineColorBlendStateCreateInfo* blend );
        ~PipelineCreateScope();
        PipelineCreateScope( const PipelineCreateScope& )            = delete;
        PipelineCreateScope& operator=( const PipelineCreateScope& ) = delete;

        struct Active
        {
            std::string_view                           Name;
            const VkPipelineColorBlendStateCreateInfo* Blend = nullptr;
        };
        // The create running on this thread, nullptr outside one.
        [[nodiscard]] static const Active* Current();

    private:
        Active m_Active;
    };

    class VulkanAllocator;
    class VulkanContext final : public RendererContext
    {
    public:
        VulkanContext( const std::shared_ptr<Window>& window );
        virtual ~VulkanContext() = default;

        virtual void Init() override;

        [[nodiscard]] virtual Common::BoolResultStr BeginFrame() const override;

        // EMPTY BY OBLIGATION, not by omission. `RendererContext::OnResize` is pure virtual, and on Vulkan
        // there is nothing at CONTEXT level that a resize touches: the swapchain owns every size-dependent
        // object and `VulkanSwapChain::RequestRebuild` is what the window actually calls. The names are commented
        // out so the signature still documents the contract.
        void OnResize( uint32_t /*width*/, uint32_t /*height*/ ) override
        {
        }
        virtual void Shutdown() override;

        [[nodiscard]] static const VkInstance& GetVulkanInstance()
        {
            return s_VulkanInstance;
        }

        // The vk-bootstrap view of the same instance; device selection (DeviceCapsProbe) is built from it.
        [[nodiscard]] static const vkb::Instance& GetBootstrapInstance();

        [[nodiscard]] Common::ResultStr<VkResult> CreateVKInstance();

        const auto& GetVulkanAllocator() const
        {
            return m_VulkanAllocator;
        }

    private:
        inline static VkInstance         s_VulkanInstance;
        VkDebugReportCallbackEXT         m_DebugReportCallback = VK_NULL_HANDLE;
        std::weak_ptr<Window>            m_Window;
        std::unique_ptr<VulkanAllocator> m_VulkanAllocator;
    };

} // namespace Desert::Graphic::API::Vulkan