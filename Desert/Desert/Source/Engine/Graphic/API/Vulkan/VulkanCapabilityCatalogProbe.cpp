#include <Engine/Graphic/API/Vulkan/VulkanCapabilityCatalog.hpp>

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

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties( physicalDevice, &properties );
        probe.VendorId                    = properties.vendorID;
        probe.Type                        = properties.deviceType;
        probe.ColorSampleCounts           = properties.limits.framebufferColorSampleCounts;
        probe.DepthSampleCounts           = properties.limits.framebufferDepthSampleCounts;
        probe.MaxSamplerAnisotropy        = properties.limits.maxSamplerAnisotropy;
        probe.MaxImageDimension2D         = properties.limits.maxImageDimension2D;
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

        // No third-party upscaler SDK is integrated, so none of their runtimes can have reported this device:
        // the flags keep their false default and the catalog never offers them.

        if ( surface != VK_NULL_HANDLE )
        {
            uint32_t formatCount = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, surface, &formatCount, nullptr );
            probe.SurfaceFormats.resize( formatCount );
            vkGetPhysicalDeviceSurfaceFormatsKHR( physicalDevice, surface, &formatCount,
                                                  probe.SurfaceFormats.data() );

            uint32_t modeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, surface, &modeCount, nullptr );
            probe.PresentModes.resize( modeCount );
            vkGetPhysicalDeviceSurfacePresentModesKHR( physicalDevice, surface, &modeCount,
                                                       probe.PresentModes.data() );
        }
        return probe;
    }
} // namespace Desert::Graphic::API::Vulkan
