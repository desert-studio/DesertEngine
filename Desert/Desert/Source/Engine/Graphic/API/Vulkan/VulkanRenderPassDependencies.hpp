#pragma once

#include <vulkan/vulkan.h>

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
} // namespace Desert::Graphic::API::Vulkan
