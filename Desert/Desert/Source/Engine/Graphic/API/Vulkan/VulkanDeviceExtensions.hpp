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

    // What a runtime asks of the instance.
    struct InstanceRequirements
    {
        // Instance extensions, every one of them already checked present.
        std::vector<const char*> Extensions;
        // The runtime's SDK hands the engine's Vulkan handles to the driver along a path the layers do not sit
        // on (NVIDIA NGX: nvngx_dlss calls into the ICD directly). The validation layer gives the application
        // wrapped handles (unique ids, not the driver's pointers) unless told not to, and a wrapped handle that
        // reaches the driver unseen is dereferenced as a pointer - an access violation inside the driver, which
        // Streamline reports as Result::eErrorExceptionHandler. With this set, the instance is created with
        // VK_VALIDATION_FEATURE_DISABLE_UNIQUE_HANDLES_EXT whenever the validation layer is on.
        bool PassesHandlesBesideTheLayers = false;
    };

    // THE DEVICE'S PRESENTATION ENTRY POINTS (UE: the DLSS/Streamline plugin's manual hooking, where the RHI's
    // present, acquire and swapchain calls run through Streamline's proxies so its common plugin sees every frame
    // - presentCommon - and every swapchain). The engine's swapchain and device call these, never the loader's
    // symbols directly; by default each one IS the loader's. A runtime that has to see presentation (Streamline
    // with eUseManualHooking: its interposer exports these functions and runs its plugins' before/after hooks
    // around the driver's) replaces them in AmendPresentationDispatch. Members carry the Vulkan names so a call
    // reads as the Vulkan call it is.
    struct VulkanPresentationDispatch
    {
        PFN_vkQueuePresentKHR       vkQueuePresentKHR       = ::vkQueuePresentKHR;
        PFN_vkAcquireNextImageKHR   vkAcquireNextImageKHR   = ::vkAcquireNextImageKHR;
        PFN_vkCreateSwapchainKHR    vkCreateSwapchainKHR    = ::vkCreateSwapchainKHR;
        PFN_vkDestroySwapchainKHR   vkDestroySwapchainKHR   = ::vkDestroySwapchainKHR;
        PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = ::vkGetSwapchainImagesKHR;
        PFN_vkDeviceWaitIdle        vkDeviceWaitIdle        = ::vkDeviceWaitIdle;
    };

    class IVulkanDeviceExtension
    {
    public:
        virtual ~IVulkanDeviceExtension() = default;

        [[nodiscard]] virtual std::string_view Name() const = 0;

        // Before vkCreateInstance, once. What this runtime needs of the instance (a runtime that cannot run
        // refuses itself and asks for nothing).
        [[nodiscard]] virtual InstanceRequirements InstanceExtensions() = 0;

        // After the physical device is chosen and before vkCreateDevice, once. Enables on @p device what the
        // runtime needs (only when all of it is present; otherwise the runtime refuses itself and enables
        // nothing).
        virtual void OnPhysicalDeviceSelected( VkInstance instance, vkb::PhysicalDevice& device ) = 0;

        // While the capability catalog is probed (after OnPhysicalDeviceSelected): what this runtime offers.
        virtual void AmendCatalogProbe( CatalogProbe& probe ) const = 0;

        // After vkCreateDevice and vkGetDeviceQueue, once.
        virtual void OnDeviceCreated( const VulkanDeviceHandles& handles ) = 0;

        // Right after OnDeviceCreated, once, before any swapchain exists: replaces the presentation entry points
        // this runtime has to see (a runtime that does not, or that refused itself, leaves @p dispatch as it is).
        // What it puts there stays valid until OnDeviceDestroying; the device restores the loader's entry points
        // before calling that.
        virtual void AmendPresentationDispatch( VulkanPresentationDispatch& dispatch ) const = 0;

        // After vkDeviceWaitIdle and before vkDestroyDevice, once: the GPU is idle, every object the runtime made
        // on the device goes now.
        virtual void OnDeviceDestroying() = 0;
    };

    // This platform's runtimes, in the order they are called. Empty where the platform has none.
    [[nodiscard]] std::span<IVulkanDeviceExtension* const> VulkanDeviceExtensions();
} // namespace Desert::Graphic::API::Vulkan
