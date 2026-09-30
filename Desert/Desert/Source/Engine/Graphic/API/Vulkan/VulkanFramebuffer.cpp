#include <Engine/Graphic/API/Vulkan/VulkanFramebuffer.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanRenderPassDependencies.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDevice.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanImage.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanContext.hpp>

#include <Engine/Core/EngineContext.hpp>

namespace Desert::Graphic::API::Vulkan
{
    VulkanFramebuffer::VulkanFramebuffer( const FramebufferSpecification& spec ) 
        : m_FramebufferSpecification( spec ), m_Width( spec.Width ), m_Height( spec.Height )
    {
    }

    VulkanFramebuffer::~VulkanFramebuffer()
    {
        Release();
    }

    Common::BoolResultStr VulkanFramebuffer::RT_Invalidate()
    {
        Release();

        auto vkDevice = SP_CAST( VulkanLogicalDevice, EngineContext::GetInstance().GetDevice() )->GetVulkanLogicalDevice();

        std::vector<VkAttachmentDescription> attachmentDescriptions;
        std::vector<VkAttachmentReference>   colorAttachmentReferences;
        VkAttachmentReference                depthAttachmentReference = {};
        bool                                 hasDepth                 = false;

        auto toVkLoadOp = []( AttachmentLoad op ) -> VkAttachmentLoadOp
        {
            switch ( op )
            {
                case AttachmentLoad::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
                case AttachmentLoad::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                default:                       return VK_ATTACHMENT_LOAD_OP_CLEAR;
            }
        };
        auto toVkStoreOp = []( AttachmentStore op ) -> VkAttachmentStoreOp
        {
            return op == AttachmentStore::DontCare ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                                                   : VK_ATTACHMENT_STORE_OP_STORE;
        };

        // MSAA: base attachments render at N samples; every COLOR gets a companion single-sample
        // RESOLVE attachment (appended AFTER the base ones) the subpass resolves into at the end.
        // Downstream consumers read the resolved images (see below), so only this file knows MSAA
        // exists. Depth is not resolved — passes that sample depth are gated at the settings level.
        const uint32_t samples = m_FramebufferSpecification.Samples > 1 ? m_FramebufferSpecification.Samples : 1;
        const VkSampleCountFlagBits vkSamples = static_cast<VkSampleCountFlagBits>( samples );
        std::vector<VkAttachmentReference> resolveAttachmentReferences;

        uint32_t i = 0;
        for ( const auto& attachment : m_FramebufferSpecification.Attachments.Attachments )
        {
            const auto format = attachment.Format;
            if ( Graphic::Utils::IsDepthFormat( format ) )
            {
                VkAttachmentDescription& depthAttachment = attachmentDescriptions.emplace_back();
                depthAttachment.format         = GetImageVulkanFormat( format );
                depthAttachment.samples        = vkSamples;
                depthAttachment.loadOp         = toVkLoadOp( attachment.LoadOp );
                depthAttachment.storeOp        = toVkStoreOp( attachment.StoreOp );
                depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depthAttachment.initialLayout  = attachment.LoadOp == AttachmentLoad::Load
                                                     ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                     : VK_IMAGE_LAYOUT_UNDEFINED;
                depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

                depthAttachmentReference.attachment = i;
                depthAttachmentReference.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                hasDepth                            = true;
            }
            else
            {
                VkAttachmentDescription& colorAttachment = attachmentDescriptions.emplace_back();
                colorAttachment.format         = GetImageVulkanFormat( format );
                colorAttachment.samples        = vkSamples;
                colorAttachment.loadOp         = toVkLoadOp( attachment.LoadOp );
                colorAttachment.storeOp        = toVkStoreOp( attachment.StoreOp );
                colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                colorAttachment.initialLayout  = attachment.LoadOp == AttachmentLoad::Load
                                                     ? ( samples > 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL )
                                                     : VK_IMAGE_LAYOUT_UNDEFINED;
                colorAttachment.finalLayout    = samples > 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                colorAttachmentReferences.push_back( { i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } );
            }
            i++;
        }

        // Companion resolve attachments (MSAA only): single-sample, DONT_CARE on load (the resolve
        // overwrites every pixel), shader-readable at the end — exactly what downstream passes expect.
        if ( samples > 1 )
        {
            for ( const auto& attachment : m_FramebufferSpecification.Attachments.Attachments )
            {
                const auto format = attachment.Format;
                if ( Graphic::Utils::IsDepthFormat( format ) )
                    continue;
                VkAttachmentDescription& resolveAttachment = attachmentDescriptions.emplace_back();
                resolveAttachment.format         = GetImageVulkanFormat( format );
                resolveAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
                resolveAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                resolveAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
                resolveAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                resolveAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
                resolveAttachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                resolveAttachmentReferences.push_back( { i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } );
                i++;
            }
        }

        // The engine's one single-subpass builder (VulkanRenderPassDependencies.hpp), which the render graph's
        // passes use too: pipelines built against this pass draw in the graph's passes on these attachments,
        // multisampled ones with their resolves included.
        VK_CHECK_RESULT( CreateSinglePassRenderPass( vkDevice, attachmentDescriptions, colorAttachmentReferences,
                                                     resolveAttachmentReferences,
                                                     hasDepth ? &depthAttachmentReference : nullptr,
                                                     m_FramebufferSpecification.PresentTarget, m_RenderPass ) );

        // Build a second compatible render pass that uses LOAD_OP_LOAD for every attachment.
        // Render-graph passes use this so they accumulate into the framebuffer instead of
        // clearing the output of the previous pass.
        {
            auto makeLoadAttachment = []( VkAttachmentDescription src ) -> VkAttachmentDescription
            {
                src.loadOp        = VK_ATTACHMENT_LOAD_OP_LOAD;
                src.initialLayout = src.finalLayout; // layout must already be finalLayout at load time
                return src;
            };

            std::vector<VkAttachmentDescription> loadDescs;
            loadDescs.reserve( attachmentDescriptions.size() );
            for ( const auto& d : attachmentDescriptions )
                loadDescs.push_back( makeLoadAttachment( d ) );

            // The load RP MUST reuse the clear RP's exact subpass dependencies. Vulkan render-pass
            // compatibility (VUID-VkRenderPassBeginInfo-renderPass-00904) is checked by the validation layer
            // including the dependency structure, and the VkFramebuffer below is created with the clear RP;
            // beginning it with a load RP whose dependencies differ is flagged incompatible. Identical
            // dependencies keep the two render passes compatible so a pass (e.g. deferred lighting) can LOAD
            // and composite over the already-rendered forward scene. Only loadOp/initialLayout differ.
            VK_CHECK_RESULT( CreateSinglePassRenderPass(
                 vkDevice, loadDescs, colorAttachmentReferences, resolveAttachmentReferences,
                 hasDepth ? &depthAttachmentReference : nullptr, m_FramebufferSpecification.PresentTarget,
                 m_RenderPassLoad ) );
        }

        std::vector<VkImageView> attachments;
        for ( const auto& attachment : m_FramebufferSpecification.Attachments.Attachments )
        {
            const auto format = attachment.Format;
            Core::Formats::Image2DSpecification imageSpec = {
                 // Index the tag: a G-buffer's four attachments otherwise all carry the same name, which
                 // is exactly when a capture is hardest to read.
                 .Tag = m_FramebufferSpecification.DebugName + "_attachment" +
                        std::to_string( attachments.size() ),
                 .Width      = m_FramebufferSpecification.Width,
                 .Height     = m_FramebufferSpecification.Height,
                 .Format     = format,
                 .Mips       = 1,
                 .Samples    = samples,
                 .Usage      = Core::Formats::Image2DUsage::Attachment,
                 .Properties = Core::Formats::Sample };

            auto       image     = std::make_shared<VulkanImage2D>( imageSpec );
            const auto allocated = image->RT_Invalidate();
            // AN ATTACHMENT THAT DID NOT ALLOCATE CANNOT BE PUT IN A FRAMEBUFFER. Its ImageView is
            // VK_NULL_HANDLE, which is pushed into `attachments` two lines down and handed to
            // vkCreateFramebuffer — a validation error at best, and at worst a framebuffer that renders
            // into nothing. Refusing here names the attachment; before this the answer was dropped and
            // the first sign was a driver message about a handle nobody could trace to a spec.
            if ( !allocated.IsSuccess() )
                return Common::MakeFormattedError<bool>(
                     "framebuffer '{}': attachment {}x{} failed: {}", m_FramebufferSpecification.DebugName,
                     m_FramebufferSpecification.Width, m_FramebufferSpecification.Height, allocated.GetError() );

            if ( Graphic::Utils::IsDepthFormat( format ) )
                m_DepthAttachment = image;
            else if ( samples > 1 )
                m_MultisampleColorAttachments.push_back( image ); // render target; consumers get the resolve
            else
                m_ColorAttachments.push_back( image );

            attachments.push_back( image->GetResource().ImageView );
        }

        // Resolve images (MSAA only), appended in the SAME order the resolve attachment
        // descriptions were. These are what GetColorAttachmentImage() hands out — single-sample,
        // shader-readable — so the post stack / ImGui viewport never know MSAA happened.
        if ( samples > 1 )
        {
            for ( const auto& attachment : m_FramebufferSpecification.Attachments.Attachments )
            {
                const auto format = attachment.Format;
                if ( Graphic::Utils::IsDepthFormat( format ) )
                    continue;
                Core::Formats::Image2DSpecification resolveSpec = {
                     .Tag        = m_FramebufferSpecification.DebugName + "_resolve",
                     .Width      = m_FramebufferSpecification.Width,
                     .Height     = m_FramebufferSpecification.Height,
                     .Format     = format,
                     .Mips       = 1,
                     .Usage      = Core::Formats::Image2DUsage::Attachment,
                     .Properties = Core::Formats::Sample };

                auto       image     = std::make_shared<VulkanImage2D>( resolveSpec );
                const auto allocated = image->RT_Invalidate();
                if ( !allocated.IsSuccess() )
                    return Common::MakeFormattedError<bool>(
                         "framebuffer '{}': MSAA resolve attachment {}x{} failed: {}",
                         m_FramebufferSpecification.DebugName, m_FramebufferSpecification.Width,
                         m_FramebufferSpecification.Height, allocated.GetError() );
                m_ColorAttachments.push_back( image );
                attachments.push_back( image->GetResource().ImageView );
            }
        }

        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass      = m_RenderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>( attachments.size() );
        framebufferInfo.pAttachments    = attachments.data();
        framebufferInfo.width           = m_FramebufferSpecification.Width;
        framebufferInfo.height          = m_FramebufferSpecification.Height;
        framebufferInfo.layers          = 1;

        VK_CHECK_RESULT( vkCreateFramebuffer( vkDevice, &framebufferInfo, nullptr, &m_Framebuffer ) );

        m_Width = m_FramebufferSpecification.Width;
        m_Height = m_FramebufferSpecification.Height;

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanFramebuffer::Invalidate()
    {
        return RT_Invalidate();
    }

    Common::BoolResultStr VulkanFramebuffer::Release()
    {
        // At process teardown the swapchain's framebuffers can be destroyed AFTER the renderer context
        // and its allocator/device are already gone (~Application -> ~Window -> ~VulkanSwapChain ->
        // ~VulkanFramebuffer). Reaching for the allocator then dereferenced freed memory and segfaulted
        // on exit. When it's unavailable, just drop our handles — the GPU objects are freed with the
        // device anyway. During normal runtime (resize/recreate) the allocator is present, so we destroy.
        const auto ctx       = EngineContext::GetInstance().GetRendererContext();
        auto*      allocator = ctx ? SP_CAST( VulkanContext, ctx )->GetVulkanAllocator().get() : nullptr;

        if ( allocator )
        {
            if ( m_Framebuffer )    allocator->RT_DestroyFramebuffer( m_Framebuffer );
            if ( m_RenderPass )     allocator->RT_DestroyRenderPass( m_RenderPass );
            if ( m_RenderPassLoad ) allocator->RT_DestroyRenderPass( m_RenderPassLoad );
        }

        m_Framebuffer    = VK_NULL_HANDLE;
        m_RenderPass     = VK_NULL_HANDLE;
        m_RenderPassLoad = VK_NULL_HANDLE;
        m_ColorAttachments.clear();
        m_MultisampleColorAttachments.clear();
        m_DepthAttachment = nullptr;

        return BOOLSUCCESS;
    }

    Common::BoolResultStr VulkanFramebuffer::Resize( uint32_t width, uint32_t height )
    {
        m_FramebufferSpecification.Width = width;
        m_FramebufferSpecification.Height = height;
        return RT_Invalidate();
    }

    Common::BoolResultStr VulkanFramebuffer::SetSamples( const uint32_t samples )
    {
        if ( m_FramebufferSpecification.Samples == samples )
            return BOOLSUCCESS;
        m_FramebufferSpecification.Samples = samples;
        return RT_Invalidate();
    }

} // namespace Desert::Graphic::API::Vulkan
