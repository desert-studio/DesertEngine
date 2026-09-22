#pragma once

#include <vulkan/vulkan.hpp>

#include <Engine/Graphic/DeviceLost.hpp>

inline PFN_vkSetDebugUtilsObjectNameEXT
     fpSetDebugUtilsObjectNameEXT; // Making it static randomly sets it to nullptr for some reason.
inline PFN_vkCmdBeginDebugUtilsLabelEXT fpCmdBeginDebugUtilsLabelEXT; // command-buffer regions (RenderDoc tree)
inline PFN_vkCmdEndDebugUtilsLabelEXT   fpCmdEndDebugUtilsLabelEXT;

namespace Desert::Graphic::API::Vulkan
{
    void VulkanLoadDebugUtilsExtensions( VkInstance instance );

    const std::string VkResultToString( VkResult result );

    /// TRUE when @p result means the device is gone, having latched `Graphic::DeviceLost` on the way. Every
    /// macro below asks this before deciding what a failed result means, so that ONE result — the one the
    /// engine cannot do anything about and did not cause — is recognised wherever it appears rather than
    /// only where somebody remembered to look for it.
    ///
    /// It is deliberately not `noexcept`-narrow and deliberately not inline-only: the site string is built
    /// here so that every route into device loss names its call the same way.
    [[nodiscard]] bool NoteIfDeviceLost( VkResult result, const char* call, const char* file, int line );

    // A FAILED RESULT IS AN INVARIANT VIOLATION — EXCEPT FOR THE ONE THAT IS NOT.
    //
    // `VK_ERROR_DEVICE_LOST` is a state this engine expects to meet (see Engine/Graphic/DeviceLost.hpp:
    // on macOS any other process's GPU fault takes us down with it). Aborting on it treats an expected
    // event as an impossible one and loses everything the user had not saved, with a log whose last line
    // points at a synchronisation defect that does not exist. So device loss latches and returns; every
    // OTHER failure still aborts exactly as it always did, because those really are invariant violations.
#define VK_CHECK_RESULT( f )                                                                                      \
    {                                                                                                             \
        const VkResult res = ( f );                                                                               \
        if ( res != VK_SUCCESS && !NoteIfDeviceLost( res, #f, __FILE__, __LINE__ ) )                              \
        {                                                                                                         \
            LOG_ERROR( "VkResult is '{}' in {}:{}", VkResultToString( res ), __FILE__, __LINE__ );                \
            DESERT_VERIFY( false );                                                                               \
        }                                                                                                         \
    }

    // The three error-RETURNING macros already stop their function, so device loss needs no special
    // control flow here — only the latch, so that whichever of them meets it first is the one that names
    // the cause. The result of NoteIfDeviceLost is discarded on purpose: these macros return either way.
#define VK_CHECK_RESULT_BOOL( f )                                                                                 \
    {                                                                                                             \
        const VkResult res = ( f );                                                                               \
        if ( res != VK_SUCCESS )                                                                                  \
        {                                                                                                         \
            (void)NoteIfDeviceLost( res, #f, __FILE__, __LINE__ );                                                \
            LOG_ERROR( "VkResult is '{}' in {}:{}", VkResultToString( res ), __FILE__, __LINE__ );                \
            return Common::MakeFormattedError<bool>( "VkResult is '{}' in {}:{}", VkResultToString( res ),        \
                                                     __FILE__, __LINE__ );                                        \
        }                                                                                                         \
    }

#define VK_RETURN_RESULT_IF_FALSE( f )                                                                            \
    {                                                                                                             \
        const VkResult res = ( f );                                                                               \
        if ( res != VK_SUCCESS )                                                                                  \
        {                                                                                                         \
            (void)NoteIfDeviceLost( res, #f, __FILE__, __LINE__ );                                                \
            return Common::MakeFormattedError<VkResult>( "VkResult is '{}' in {}:{}", VkResultToString( res ),    \
                                                         __FILE__, __LINE__ );                                    \
        }                                                                                                         \
    }

#define VK_RETURN_RESULT_IF_FALSE_TYPE( type, f )                                                                 \
    {                                                                                                             \
        const VkResult res = ( f );                                                                               \
        if ( res != VK_SUCCESS )                                                                                  \
        {                                                                                                         \
            (void)NoteIfDeviceLost( res, #f, __FILE__, __LINE__ );                                                \
            return Common::MakeFormattedError<type>( "VkResult is '{}' in {}:{}", VkResultToString( res ),        \
                                                     __FILE__, __LINE__ );                                        \
        }                                                                                                         \
    }

#define VK_RETURN_RESULT( f )                                                                                     \
    {                                                                                                             \
        const VkResult res = ( f );                                                                               \
        if ( res != VK_SUCCESS )                                                                                  \
        {                                                                                                         \
            (void)NoteIfDeviceLost( res, #f, __FILE__, __LINE__ );                                                \
            /* The stringified call is an ARGUMENT, never the format string: `#f` is arbitrary source             \
               text, so a brace anywhere in it would be parsed as a placeholder and throw - from inside           \
               the code reporting a Vulkan failure. */                                                            \
            return Common::MakeFormattedError<VkResult>( "{} -> result: {}", #f, VkResultToString( res ) );       \
        }                                                                                                         \
        else                                                                                                      \
        {                                                                                                         \
            return Common::MakeSuccess( res );                                                                    \
        }                                                                                                         \
    }

    namespace Utils
    {
        void InsertImageMemoryBarrier( VkCommandBuffer cmdBuf, VkImage Image, VkFormat Format,
                                       VkImageLayout OldLayout, VkImageLayout NewLayout, uint32_t layers = 1,
                                       uint32_t mipLevels = 1 );

        void InsertImageMemoryBarrier( VkCommandBuffer cmdbuffer, VkImage image, VkAccessFlags srcAccessMask,
                                       VkAccessFlags dstAccessMask, VkImageLayout oldImageLayout,
                                       VkImageLayout newImageLayout, VkPipelineStageFlags srcStageMask,
                                       VkPipelineStageFlags    dstStageMask,
                                       VkImageSubresourceRange subresourceRange );

        Common::ResultStr<VkImageView> CreateImageView( VkDevice device, VkImage image, VkFormat format,
                                                     VkImageAspectFlags aspectFlags, VkImageViewType viewType,
                                                     uint32_t layerCount, uint32_t mipLeveles );
    } // namespace Utils

    namespace VKUtils
    {
        inline static void SetDebugUtilsObjectName( const VkDevice device, const VkObjectType objectType,
                                                    const std::string& name, const void* handle )
        {
            VkDebugUtilsObjectNameInfoEXT nameInfo;
            nameInfo.sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
            nameInfo.objectType   = objectType;
            nameInfo.pObjectName  = name.c_str();
            nameInfo.objectHandle = (uint64_t)handle;
            nameInfo.pNext        = VK_NULL_HANDLE;

            VK_CHECK_RESULT( fpSetDebugUtilsObjectNameEXT( device, &nameInfo ) );
        }

        // Opens a named region in the command buffer — RenderDoc/Xcode show these as a tree of
        // passes. Always paired with EndDebugLabel; no-ops when debug utils are unavailable.
        inline static void BeginDebugLabel( VkCommandBuffer cmdBuffer, const char* name )
        {
            VkDebugUtilsLabelEXT label{};
            label.sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
            label.pLabelName = name;
            fpCmdBeginDebugUtilsLabelEXT( cmdBuffer, &label );
        }

        inline static void EndDebugLabel( VkCommandBuffer cmdBuffer )
        {
            fpCmdEndDebugUtilsLabelEXT( cmdBuffer );
        }
    } // namespace VKUtils

} // namespace Desert::Graphic::API::Vulkan