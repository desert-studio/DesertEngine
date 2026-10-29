#include <Engine/Graphic/API/Vulkan/VulkanCapabilityCatalog.hpp>
#include <Engine/Graphic/API/Vulkan/VulkanDeviceExtensions.hpp>

#include <Engine/Graphic/API/UpscalerBuild.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>

// The driver half of the catalog fill: every vkGet* the catalog needs, asked once. Kept apart from
// VulkanCapabilityCatalog.cpp so the pure builder compiles in the contract suite without a Vulkan loader.
namespace Desert::Graphic::API::Vulkan
{
    CatalogProbe ProbeCatalog( VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, const DeviceCaps& caps )
    {
        CatalogProbe probe;
        probe.Caps        = caps;
        probe.Portability = caps.Has( Capability::PortabilitySubset );

        // FSR is linked (not a runtime that loads or not): the device and the build decide, by name.
        if ( const Common::BoolResultStr fsr = FsrDeviceSupport( caps, kFsrBuilt ); fsr )
            probe.FsrAvailable = true;
        else
        {
            LOG_INFO( "[CapabilityCatalog] {}", fsr.GetError() );
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties( physicalDevice, &properties );
        probe.VendorId                    = properties.vendorID;
        probe.Type                        = properties.deviceType;
        probe.ColorSampleCounts           = properties.limits.framebufferColorSampleCounts;
        probe.DepthSampleCounts           = properties.limits.framebufferDepthSampleCounts;
        probe.MaxSamplerAnisotropy        = properties.limits.maxSamplerAnisotropy;
        probe.MaxImageDimension2D         = properties.limits.maxImageDimension2D;
        probe.MaxRenderArea               = std::min(
             { properties.limits.maxFramebufferWidth, properties.limits.maxFramebufferHeight,
                             properties.limits.maxViewportDimensions[0], properties.limits.maxViewportDimensions[1] } );
        probe.TimestampComputeAndGraphics = properties.limits.timestampComputeAndGraphics == VK_TRUE;

        // The largest device-local heap, as DeviceCapabilities::VideoMemory counts it.
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties( physicalDevice, &memory );
        for ( uint32_t i = 0; i < memory.memoryHeapCount; ++i )
            if ( ( memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ) != 0 )
                probe.DeviceLocalBytes = std::max<uint64_t>( probe.DeviceLocalBytes, memory.memoryHeaps[i].size );

        // ASTC counts only when it can be sampled AND linearly filtered: a texture that cannot be filtered is not
        // a cooker target.
        VkFormatProperties astc{};
        vkGetPhysicalDeviceFormatProperties( physicalDevice, VK_FORMAT_ASTC_4x4_UNORM_BLOCK, &astc );
        constexpr VkFormatFeatureFlags kSampled =
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        probe.AstcLdrSampled = ( astc.optimalTilingFeatures & kSampled ) == kSampled;

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties( physicalDevice, &familyCount, nullptr );
        std::vector<VkQueueFamilyProperties> families( familyCount );
        vkGetPhysicalDeviceQueueFamilyProperties( physicalDevice, &familyCount, families.data() );
        bool graphicsSeen = false;
        for ( const VkQueueFamilyProperties& family : families )
        {
            const bool graphics = ( family.queueFlags & VK_QUEUE_GRAPHICS_BIT ) != 0;
            const bool compute  = ( family.queueFlags & VK_QUEUE_COMPUTE_BIT ) != 0;
            if ( graphics && !graphicsSeen )
            {
                // The first graphics family is the one the device creates its graphics queue on.
                probe.TimestampValidBitsGraphics = family.timestampValidBits;
                graphicsSeen                     = true;
            }
            if ( compute && !graphics )
                probe.SeparateComputeFamily = true;
        }

        // Whether the driver EXPORTS the two handle types OIDN imports (CapabilityCatalog::Sharing): a storage
        // buffer the path tracer writes and copies, and a timeline semaphore. Core 1.1 queries.
        const auto bufferExportable = [physicalDevice]( VkExternalMemoryHandleTypeFlagBits handleType )
        {
            VkPhysicalDeviceExternalBufferInfo info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            info.handleType = handleType;
            VkExternalBufferProperties properties{ VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
            vkGetPhysicalDeviceExternalBufferProperties( physicalDevice, &info, &properties );
            return ( properties.externalMemoryProperties.externalMemoryFeatures &
                     VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ) != 0;
        };
        const auto timelineExportable = [physicalDevice]( VkExternalSemaphoreHandleTypeFlagBits handleType )
        {
            VkSemaphoreTypeCreateInfo timeline{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
            timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkPhysicalDeviceExternalSemaphoreInfo info{
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO };
            info.pNext      = &timeline;
            info.handleType = handleType;
            VkExternalSemaphoreProperties properties{ VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
            vkGetPhysicalDeviceExternalSemaphoreProperties( physicalDevice, &info, &properties );
            return ( properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT ) != 0;
        };
        probe.ExternalBufferExportWin32 = bufferExportable( VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT );
        probe.ExternalBufferExportFd    = bufferExportable( VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT );
        probe.ExternalTimelineExportWin32 =
             timelineExportable( VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT );
        probe.ExternalTimelineExportFd = timelineExportable( VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT );

        // No third-party upscaler SDK is integrated, so none of their runtimes can have reported this device:
        // the flags keep their false default and the catalog never offers them.

        if ( surface != VK_NULL_HANDLE )
        {
            // Both enumerations READ their VkResult: a surface the driver cannot answer for offers nothing,
            // rather than a list sized by a count the failed call never wrote. VK_INCOMPLETE (the list grew
            // between the two calls) keeps what was written.
            const auto answered = []( VkResult result )
            { return result == VK_SUCCESS || result == VK_INCOMPLETE; };

            uint32_t formatCount = 0;
            if ( answered(
                      vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, surface, &formatCount, nullptr ) ) )
            {
                probe.SurfaceFormats.resize( formatCount );
                if ( answered( vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, surface, &formatCount,
                                                                     probe.SurfaceFormats.data() ) ) )
                    probe.SurfaceFormats.resize( formatCount );
                else
                    probe.SurfaceFormats.clear();
            }

            uint32_t modeCount = 0;
            if ( answered(
                      vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, surface, &modeCount, nullptr ) ) )
            {
                probe.PresentModes.resize( modeCount );
                if ( answered( vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, surface, &modeCount,
                                                                          probe.PresentModes.data() ) ) )
                    probe.PresentModes.resize( modeCount );
                else
                    probe.PresentModes.clear();
            }
        }
        // What a vendor runtime offers (DlssAvailable...): its own answer, given once the device's requirements
        // were enabled or refused.
        for ( const IVulkanDeviceExtension* extension : VulkanDeviceExtensions() )
            extension->AmendCatalogProbe( probe );
        return probe;
    }
} // namespace Desert::Graphic::API::Vulkan
