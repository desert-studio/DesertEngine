#pragma once

#include <Engine/Graphic/Framebuffer.hpp>

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <optional>

namespace Desert::Graphic::API::Vulkan
{
    class VulkanFramebuffer : public Framebuffer
    {
    public:
        VulkanFramebuffer( const FramebufferSpecification& spec );
        virtual ~VulkanFramebuffer() override;

        virtual const FramebufferSpecification GetSpecification() const override
        {
            return m_FramebufferSpecification;
        }

        virtual uint32_t GetFramebufferWidth() const override
        {
            return m_Width;
        }
        virtual uint32_t GetFramebufferHeight() const override
        {
            return m_Height;
        }

        auto GetVKFramebuffer() const
        {
            return m_Framebuffer;
        }

        auto GetVKRenderPass() const
        {
            return m_RenderPass;
        }

        // Returns a render pass with LOAD_OP_LOAD for all attachments.
        // Used by render-graph passes that accumulate into the framebuffer
        // without clearing the previous pass's output.
        auto GetVKRenderPassLoad() const
        {
            return m_RenderPassLoad;
        }

        uint32_t GetColorAttachmentCount() const override
        {
            return (uint32_t)(m_ColorAttachments.size() + m_ExternalColorAttachments.size());
        }

        uint32_t GetDepthAttachmentCount() const override
        {
            return ( m_DepthAttachment ||
                     ( m_ExternalDepthAttachment && m_ExternalDepthAttachment->Image.lock() ) )
                        ? 1U
                        : 0U;
        }

        Common::BoolResultStr Invalidate() override;
        Common::BoolResultStr Release() override;

        Common::BoolResultStr Resize( uint32_t width, uint32_t height ) override;

        virtual const std::shared_ptr<Image2D>& GetColorAttachmentImage( uint32_t index = 0 ) const override
        {
            return m_ColorAttachments.at( index );
        }

        virtual const std::shared_ptr<Image2D>& GetDepthAttachmentImage() const override
        {
            return m_DepthAttachment;
        }

        virtual const std::shared_ptr<Image2D>& GetMultisampleColorAttachmentImage( uint32_t index ) const override
        {
            return m_MultisampleColorAttachments.at( index );
        }

        const auto& GetClearValues() const
        {
            return m_ClearValues;
        }

        // --- Vulkan Specific ---
        NO_DISCARD Common::BoolResultStr RT_Invalidate();

    private:
        std::shared_ptr<Image2D>              m_DepthAttachment;
        std::vector<std::shared_ptr<Image2D>> m_ColorAttachments;
        // MSAA render targets (Samples > 1). The subpass resolves them into m_ColorAttachments,
        // which is all any consumer ever sees — kept here only to own their lifetime.
        std::vector<std::shared_ptr<Image2D>> m_MultisampleColorAttachments;

        struct ExternalAttachmentInfo
        {
            std::weak_ptr<Image2D> Image;
            AttachmentLoad         LoadOp;
        };

        std::optional<ExternalAttachmentInfo> m_ExternalDepthAttachment;
        std::vector<ExternalAttachmentInfo>   m_ExternalColorAttachments;

        FramebufferSpecification m_FramebufferSpecification;

        VkFramebuffer m_Framebuffer    = VK_NULL_HANDLE;
        VkRenderPass  m_RenderPass     = VK_NULL_HANDLE;
        VkRenderPass  m_RenderPassLoad = VK_NULL_HANDLE;

        uint32_t m_Width  = 0;
        uint32_t m_Height = 0;

        std::vector<VkClearValue> m_ClearValues;
    };
} // namespace Desert::Graphic::API::Vulkan
