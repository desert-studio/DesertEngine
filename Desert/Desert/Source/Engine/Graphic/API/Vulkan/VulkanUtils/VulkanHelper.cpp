#include <Engine/Graphic/API/Vulkan/VulkanUtils/VulkanHelper.hpp>

namespace Desert::Graphic::API::Vulkan
{

    const std::string VkResultToString( VkResult result )
    {
        switch ( result )
        {
            case VK_SUCCESS:                return "VK_SUCCESS";
            case VK_NOT_READY:              return "VK_NOT_READY";
            case VK_TIMEOUT:                return "VK_TIMEOUT";
            case VK_EVENT_SET:              return "VK_EVENT_SET";
            case VK_EVENT_RESET:            return "VK_EVENT_RESET";
            case VK_INCOMPLETE:             return "VK_INCOMPLETE";
            case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
            case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
            case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
            case VK_ERROR_DEVICE_LOST:       return "VK_ERROR_DEVICE_LOST";
            case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
            case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
            case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
            case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
            case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
            case VK_ERROR_TOO_MANY_OBJECTS:  return "VK_ERROR_TOO_MANY_OBJECTS";
            case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
            // BOTH ANSWER A FULL DESCRIPTOR POOL, and both used to print as "Unknown VkResult" -- which is
            // what a reader saw in place of the one fact that explains the whole failure. The pool chain
            // tests for these two by value (VulkanViewDescriptorPools), so a log line that cannot name them
            // hides the branch the code is actually taking.
            case VK_ERROR_OUT_OF_POOL_MEMORY:
                return "VK_ERROR_OUT_OF_POOL_MEMORY";
            case VK_ERROR_FRAGMENTED_POOL:
                return "VK_ERROR_FRAGMENTED_POOL";
            default:                        return "Unknown VkResult";
        }
    }

    bool NoteIfDeviceLost( VkResult result, const char* call, const char* file, int line )
    {
        if ( result != VK_ERROR_DEVICE_LOST )
            return false;

        // The SITE is the call plus the source position, because the first thing a reader asks after
        // "the device is gone" is "gone where?" — and the answer is nearly always a submit or a present,
        // which tells them immediately that it was asynchronous and therefore not this line's fault.
        std::string site = std::string( call ) + " (" + file + ":" + std::to_string( line ) + ")";
        (void)Graphic::DeviceLost::Report( site, VkResultToString( result ) );
        return true;
    }

    void Utils::InsertImageMemoryBarrier( VkCommandBuffer cmdbuffer, VkImage image,
                                               VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask,
                                               VkImageLayout oldImageLayout, VkImageLayout newImageLayout,
                                               VkPipelineStageFlags    srcStageMask,
                                               VkPipelineStageFlags    dstStageMask,
                                               VkImageSubresourceRange subresourceRange )
    {
        VkImageMemoryBarrier imageMemoryBarrier{};
        imageMemoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        imageMemoryBarrier.srcAccessMask    = srcAccessMask;
        imageMemoryBarrier.dstAccessMask    = dstAccessMask;
        imageMemoryBarrier.oldLayout        = oldImageLayout;
        imageMemoryBarrier.newLayout        = newImageLayout;
        imageMemoryBarrier.image            = image;
        imageMemoryBarrier.subresourceRange = subresourceRange;

        vkCmdPipelineBarrier( cmdbuffer, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr, 1,
                              &imageMemoryBarrier );
    }

    void Utils::InsertImageMemoryBarrier( VkCommandBuffer cmdBuf, VkImage Image, VkFormat Format,
                                          VkImageLayout OldLayout, VkImageLayout NewLayout, uint32_t layers,
                                          uint32_t mipLevels )
    {
        VkImageMemoryBarrier barrier = { .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .pNext               = NULL,
                                         .srcAccessMask       = 0,
                                         .dstAccessMask       = 0,
                                         .oldLayout           = OldLayout,
                                         .newLayout           = NewLayout,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image               = Image,
                                         .subresourceRange =
                                              VkImageSubresourceRange{ .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                                                                       .baseMipLevel   = 0,
                                                                       .levelCount     = mipLevels,
                                                                       .baseArrayLayer = 0,
                                                                       .layerCount     = layers } };

        VkPipelineStageFlags sourceStage      = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        // THE ASPECT IS A PROPERTY OF THE FORMAT AND OF NOTHING ELSE. The depth-only formats are tested
        // FIRST and the layout is not consulted at all: this used to read
        // `NewLayout == DEPTH_STENCIL_ATTACHMENT_OPTIMAL || <a packed format>`, which is fine while every
        // depth attachment is packed and becomes a validation error the moment one is not — a D32_SFLOAT
        // image barriered with DEPTH|STENCIL, because the layout enum happens to carry the word stencil.
        if ( ( Format == VK_FORMAT_D32_SFLOAT ) || ( Format == VK_FORMAT_D16_UNORM ) )
        {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        else if ( ( Format == VK_FORMAT_D32_SFLOAT_S8_UINT ) || ( Format == VK_FORMAT_D24_UNORM_S8_UINT ) ||
                  ( Format == VK_FORMAT_D16_UNORM_S8_UINT ) ||
                  NewLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL )
        {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        else
        {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }

        if ( OldLayout == VK_IMAGE_LAYOUT_UNDEFINED && NewLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL )
        {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage           = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage      = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if ( OldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && NewLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL )
        {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage           = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage      = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else if ( OldLayout == VK_IMAGE_LAYOUT_UNDEFINED && NewLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL )
        {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage           = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage      = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        // Fallback stages are already set to ALL_COMMANDS

        vkCmdPipelineBarrier( cmdBuf, sourceStage, destinationStage, 0, 0, NULL, 0, NULL, 1, &barrier );
    }

    Common::ResultStr<VkImageView> Utils::CreateImageView( VkDevice device, VkImage image, VkFormat format,
                                                        VkImageAspectFlags aspectFlags, VkImageViewType viewType,
                                                        uint32_t layerCount, uint32_t mipLeveles )
    {
        VkImageViewCreateInfo viewInfo = { .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                           .pNext            = VK_NULL_HANDLE,
                                           .image            = image,
                                           .viewType         = viewType,
                                           .format           = format,
                                           .components       = { .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                                                                 .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                                                                 .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                                                                 .a = VK_COMPONENT_SWIZZLE_IDENTITY },
                                           .subresourceRange = { .aspectMask     = aspectFlags,
                                                                 .baseMipLevel   = 0,
                                                                 .levelCount     = mipLeveles,
                                                                 .baseArrayLayer = 0,
                                                                 .layerCount     = layerCount } };

        VkImageView imageView;
        VK_RETURN_RESULT_IF_FALSE_TYPE( VkImageView, vkCreateImageView( device, &viewInfo, VK_NULL_HANDLE, &imageView ) );
        return Common::MakeSuccess( imageView );
    }

    void VulkanLoadDebugUtilsExtensions( VkInstance instance )
    {
        fpSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)( vkGetInstanceProcAddr(
             instance, "vkSetDebugUtilsObjectNameEXT" ) );
        if ( fpSetDebugUtilsObjectNameEXT == nullptr )
            fpSetDebugUtilsObjectNameEXT =
                 []( VkDevice /*device*/, const VkDebugUtilsObjectNameInfoEXT* /*pNameInfo*/ )
            { return VK_SUCCESS; };

        fpCmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT)( vkGetInstanceProcAddr(
             instance, "vkCmdBeginDebugUtilsLabelEXT" ) );
        if ( fpCmdBeginDebugUtilsLabelEXT == nullptr )
            fpCmdBeginDebugUtilsLabelEXT = []( VkCommandBuffer, const VkDebugUtilsLabelEXT* ) {};

        fpCmdEndDebugUtilsLabelEXT =
             (PFN_vkCmdEndDebugUtilsLabelEXT)( vkGetInstanceProcAddr( instance, "vkCmdEndDebugUtilsLabelEXT" ) );
        if ( fpCmdEndDebugUtilsLabelEXT == nullptr )
            fpCmdEndDebugUtilsLabelEXT = []( VkCommandBuffer ) {};
    }

} // namespace Desert::Graphic::API::Vulkan
