#pragma once

#include <vulkan/vulkan.h>

#include <Engine/Graphic/RendererContext.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanAllocator.hpp>

namespace vkb
{
    struct Instance;
} // namespace vkb

namespace Desert::Graphic::API::Vulkan
{
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