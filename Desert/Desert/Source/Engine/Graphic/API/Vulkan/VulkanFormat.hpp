#pragma once

#include <Engine/Core/Formats/ImageFormat.hpp>

#include <vulkan/vulkan.h>

// ImageFormat -> VkFormat and aspect, with no engine object behind them: the one table both the engine's
// images and the render graph's Vulkan backend read (moved out of VulkanImage.hpp by RDG2 so the
// backend and its device suite need not pull the device singletons in).
namespace Desert::Graphic::API::Vulkan
{
    namespace Utils
    {
        // The engine format -> VkFormat table. Total over ImageFormat, no `default:` label and nothing
        // returned after the switch, for the same reason as Core::Formats::GetBytesPerPixel: the previous
        // version fell through to VK_FORMAT_UNDEFINED for anything it did not list, and an image created
        // with an undefined format is a problem you meet in a capture, not at the line that caused it.
        // Constexpr so the guard below can constant-evaluate it — see the long note in
        // Core/Formats/ImageFormat.hpp for why a warning cannot do this job in this workspace.
        //
        // @p deviceDepthFormat is the packed depth+stencil format the physical device selected; it is the
        // only entry that is not a compile-time constant.
        constexpr VkFormat GetVulkanFormat( Core::Formats::ImageFormat format, VkFormat deviceDepthFormat )
        {
            switch ( format )
            {
                case Core::Formats::ImageFormat::RGBA8F:
                    return VK_FORMAT_R8G8B8A8_UNORM;
                case Core::Formats::ImageFormat::RGBA16F:
                    return VK_FORMAT_R16G16B16A16_SFLOAT;
                case Core::Formats::ImageFormat::RGBA32F:
                    return VK_FORMAT_R32G32B32A32_SFLOAT;
                case Core::Formats::ImageFormat::BGRA8F:
                    return VK_FORMAT_B8G8R8A8_UNORM;
                case Core::Formats::ImageFormat::DEPTH32F:
                    return VK_FORMAT_D32_SFLOAT;
                case Core::Formats::ImageFormat::R16_UNORM:
                    return VK_FORMAT_R16_UNORM;
                case Core::Formats::ImageFormat::R32F:
                    return VK_FORMAT_R32_SFLOAT;
                case Core::Formats::ImageFormat::DEPTH24STENCIL8:
                    return deviceDepthFormat;
                // The four block formats. `textureCompressionBC` is read off the physical device and
                // requested by `VulkanLogicalDevice::CreateDevice` (pinned by the TextureCompressionBC
                // suite); without it these two VkFormats are not usable, and on THIS machine that is
                // undetectable at run time — measured, four mode-6 blocks came back bit-exact with the
                // feature off and no validation message. The census is the gate, not the device.
                case Core::Formats::ImageFormat::BC7_UNORM:
                    return VK_FORMAT_BC7_UNORM_BLOCK;
                case Core::Formats::ImageFormat::BC6H_UFLOAT:
                    return VK_FORMAT_BC6H_UFLOAT_BLOCK;
                // UNORM and not SNORM for both. The signed variants are different VkFormats with a
                // different endpoint transform, and the engine's normal maps arrive as UNORM bytes that
                // the shader re-centres with `2*n - 1`; picking the signed format here would move that
                // re-centring into the hardware for BC5 and leave it in the shader for RGBA8, so one of
                // the two paths would be wrong and only on the textures an author had marked up.
                case Core::Formats::ImageFormat::BC4_UNORM:
                    return VK_FORMAT_BC4_UNORM_BLOCK;
                case Core::Formats::ImageFormat::BC5_UNORM:
                    return VK_FORMAT_BC5_UNORM_BLOCK;
                case Core::Formats::ImageFormat::Count:
                    break; // the sentinel is not a format — fall through to the error path below
            }

            LOG_ERROR( "GetVulkanFormat: ImageFormat value {} is outside the enumeration",
                       static_cast<uint32_t>( format ) );
            DESERT_VERIFY( false, "ImageFormat outside the enumeration" );
        }

        namespace Detail
        {
            constexpr bool VulkanFormatTableIsTotal()
            {
                for ( uint32_t i = 0; i < Core::Formats::kImageFormatCount; ++i )
                {
                    if ( GetVulkanFormat( static_cast<Core::Formats::ImageFormat>( i ),
                                          VK_FORMAT_D32_SFLOAT_S8_UINT ) == VK_FORMAT_UNDEFINED )
                        return false;
                }
                return true;
            }
        } // namespace Detail

        static_assert( Detail::VulkanFormatTableIsTotal(),
                       "Every ImageFormat enumerator needs a VkFormat in Utils::GetVulkanFormat." );
    } // namespace Utils

    // Aspect mask a barrier or a view must name for this format. Translates the backend-independent
    // Core::Formats::GetImageAspect answer into Vulkan bits explicitly, rather than assuming the two
    // enumerations happen to share numeric values.
    inline VkImageAspectFlags GetImageVulkanAspect( Core::Formats::ImageFormat format )
    {
        const Core::Formats::ImageAspect aspect = Core::Formats::GetImageAspect( format );

        VkImageAspectFlags flags = 0;
        if ( aspect & Core::Formats::ImageAspect_Colour )
            flags |= VK_IMAGE_ASPECT_COLOR_BIT;
        if ( aspect & Core::Formats::ImageAspect_Depth )
            flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ( aspect & Core::Formats::ImageAspect_Stencil )
            flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
        return flags;
    }
} // namespace Desert::Graphic::API::Vulkan
