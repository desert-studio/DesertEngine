#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace vkb
{
    struct PhysicalDevice;
} // namespace vkb

// A RUNTIME THAT HAS TO SHAPE THE VULKAN INSTANCE AND DEVICE (UE: the IVulkanDynamicRHI extension points a plugin
// such as DLSS or FSR uses - AddEnabledInstanceExtensionsAndLayers / AddEnabledDeviceExtensionsAndLayers - plus
// the "device created" notification the plugin hands its own SDK the handles from).
//
// A vendor runtime (an upscaler SDK, a profiler) needs instance extensions before vkCreateInstance, device
// extensions and features before vkCreateDevice, and the created handles afterwards. The engine's device code
// calls every registered extension at those four points and never names a vendor; which extensions exist is
// decided per platform in VulkanDeviceExtensions.cpp. An extension that cannot run on this machine says why
// (its own log line, its own refusal text) and asks for nothing - it never fails the engine's device.
namespace Desert::Graphic::API::Vulkan
{
    struct CatalogProbe;

    // What the device was created with, for an extension that hands it to its SDK.
    struct VulkanDeviceHandles
    {
        VkInstance       Instance       = VK_NULL_HANDLE;
        VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
        VkDevice         Device         = VK_NULL_HANDLE;
        uint32_t         GraphicsFamily = 0;
        uint32_t         ComputeFamily  = 0;
        // How many queues the engine created in each family (the first free index an SDK may create its own at).
        uint32_t GraphicsQueuesCreated = 0;
        uint32_t ComputeQueuesCreated  = 0;
    };

    class IVulkanDeviceExtension
    {
    public:
        virtual ~IVulkanDeviceExtension() = default;

        [[nodiscard]] virtual std::string_view Name() const = 0;

        // Before vkCreateInstance, once. The instance extensions this runtime needs, every one of them already
        // checked present (an absent one makes the runtime refuse itself and return nothing).
        [[nodiscard]] virtual std::vector<const char*> InstanceExtensions() = 0;

        // After the physical device is chosen and before vkCreateDevice, once. Enables on @p device what the
        // runtime needs (only when all of it is present; otherwise the runtime refuses itself and enables
        // nothing).
        virtual void OnPhysicalDeviceSelected( VkInstance instance, vkb::PhysicalDevice& device ) = 0;

        // While the capability catalog is probed (after OnPhysicalDeviceSelected): what this runtime offers.
        virtual void AmendCatalogProbe( CatalogProbe& probe ) const = 0;

        // After vkCreateDevice and vkGetDeviceQueue, once.
        virtual void OnDeviceCreated( const VulkanDeviceHandles& handles ) = 0;

        // After vkDeviceWaitIdle and before vkDestroyDevice, once: the GPU is idle, every object the runtime made
        // on the device goes now.
        virtual void OnDeviceDestroying() = 0;
    };

    // This platform's runtimes, in the order they are called. Empty where the platform has none.
    [[nodiscard]] std::span<IVulkanDeviceExtension* const> VulkanDeviceExtensions();
} // namespace Desert::Graphic::API::Vulkan
