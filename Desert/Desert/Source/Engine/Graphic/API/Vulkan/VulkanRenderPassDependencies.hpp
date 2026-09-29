#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <span>
#include <vector>

namespace Desert::Graphic::API::Vulkan
{
    // The external subpass dependencies of every single-subpass render pass the engine creates: the
    // framebuffer's clear and load passes (VulkanFramebuffer), the swapchain's present pass (VulkanSwapChain)
    // and the render graph's passes (CreateRdgRenderPass). Render-pass compatibility (Vulkan spec, "Render Pass
    // Compatibility") compares the dependencies as well as the attachments: a pipeline built against one of
    // these passes may draw inside another one only if both took their dependencies from here
    // (VUID-vkCmdDraw-renderPass-02684 otherwise).
    //
    // Colour: srcStageMask covers both previous colour-attachment writes AND previous fragment-shader reads of
    // the image (e.g. Tonemap sampling JFA_Output from the prior frame); without FRAGMENT_SHADER_BIT a pass
    // begun by the engine only waits for COLOR_ATTACHMENT_OUTPUT, a write-after-read hazard. dstAccessMask
    // includes COLOR_ATTACHMENT_READ so the same dependency is right for a pass begun with LOAD_OP_LOAD (the
    // load reads the existing content): the clear and load passes must share these exact dependencies.
    // A present target matches the swapchain's present pass exactly.
    inline std::vector<VkSubpassDependency> SinglePassDependencies( bool hasColour, bool hasDepth,
                                                                    bool presentTarget )
    {
        std::vector<VkSubpassDependency> dependencies;
        if ( hasColour )
        {
            VkSubpassDependency& dependency = dependencies.emplace_back();
            dependency.srcSubpass           = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass           = 0;
            if ( presentTarget )
            {
                dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                dependency.srcAccessMask = 0;
                dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            }
            else
            {
                dependency.srcStageMask =
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                dependency.dstAccessMask =
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            }
        }
        if ( hasDepth )
        {
            VkSubpassDependency& dependency = dependencies.emplace_back();
            dependency.srcSubpass           = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass           = 0;
            dependency.srcStageMask =
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dependency.dstStageMask =
                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dependency.dstAccessMask =
                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }
        return dependencies;
    }

    // THE single-subpass render pass of the engine: VulkanFramebuffer's clear and load passes and the render
    // graph's passes (CreateRdgRenderPass) are all created here, so the subpass (colour, resolve and depth
    // references) and the dependencies are built one way and a pipeline built against one of them is
    // compatible with the others on the same formats and sample count. @p resolves is empty, or has one entry
    // per colour (VK_ATTACHMENT_UNUSED for a colour that is not resolved).
    inline VkResult CreateSinglePassRenderPass( VkDevice device, std::span<const VkAttachmentDescription> attachments,
                                                std::span<const VkAttachmentReference> colours,
                                                std::span<const VkAttachmentReference> resolves,
                                                const VkAttachmentReference* depth, bool presentTarget,
                                                VkRenderPass& renderPass )
    {
        VkSubpassDescription subpass    = {};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = static_cast<uint32_t>( colours.size() );
        subpass.pColorAttachments       = colours.empty() ? nullptr : colours.data();
        subpass.pResolveAttachments     = resolves.empty() ? nullptr : resolves.data();
        subpass.pDepthStencilAttachment = depth;

        const bool hasColour = std::any_of( colours.begin(), colours.end(), []( const VkAttachmentReference& ref )
                                            { return ref.attachment != VK_ATTACHMENT_UNUSED; } );
        const std::vector<VkSubpassDependency> dependencies =
             SinglePassDependencies( hasColour, depth != nullptr, presentTarget );

        VkRenderPassCreateInfo info = {};
        info.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount        = static_cast<uint32_t>( attachments.size() );
        info.pAttachments           = attachments.data();
        info.subpassCount           = 1;
        info.pSubpasses             = &subpass;
        info.dependencyCount        = static_cast<uint32_t>( dependencies.size() );
        info.pDependencies          = dependencies.data();
        return vkCreateRenderPass( device, &info, nullptr, &renderPass );
    }
} // namespace Desert::Graphic::API::Vulkan
