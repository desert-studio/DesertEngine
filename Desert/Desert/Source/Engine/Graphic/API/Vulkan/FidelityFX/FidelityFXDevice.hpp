#pragma once

#include <Engine/Graphic/API/Vulkan/VulkanDeviceExtensions.hpp>

// THE DEVICE THE FIDELITYFX VULKAN BACKEND ASSUMES (FSR 3.1, Graphic/Upscalers/FSR). UE's FSR plugin and AMD's
// own samples (Cauldron device_vk.cpp) create the device with what the backend uses; this is that half for the
// engine's one device chain.
//
// The backend (ThirdParty/FidelityFX-SDK/sdk/src/backends/vk/ffx_vk.cpp) never sees which extensions and
// features the device was CREATED with: GetDeviceCapabilitiesVK (:1977-2080) reads the physical device's
// AVAILABLE extension list and treats every one it finds as usable, and CreateBackendContextVK (:1464-1511) loads
// extension entry points by their extension names through vkGetDeviceProcAddr. So for each extension that probe
// acts on, the host must have enabled it (or the core feature it carries), or the backend calls a pointer the
// device never handed out. Measured: an RTX 3070 Ti lists VK_KHR_dedicated_allocation, the device enabled
// neither it nor VK_KHR_get_memory_requirements2 (both core since 1.1), vkGetDeviceProcAddr answered NULL for
// "vkGetBufferMemoryRequirements2KHR", and ffx_vk.cpp:1663 called address 0 on the first FSR frame.
//
// FSR is linked, not loaded: there is nothing to refuse at device time (the catalog's FsrDeviceSupport decides
// whether FSR is offered); each requirement below is enabled exactly when the backend's probe would act on it.
namespace Desert::Graphic::API::Vulkan::FidelityFX
{
    class DeviceRequirements final : public IVulkanDeviceExtension
    {
    public:
        static DeviceRequirements& Get();

        [[nodiscard]] std::string_view         Name() const override;
        [[nodiscard]] InstanceRequirements InstanceExtensions() override;
        void OnPhysicalDeviceSelected( VkInstance instance, vkb::PhysicalDevice& device ) override;
        void AmendCatalogProbe( CatalogProbe& probe ) const override;
        void OnDeviceCreated( const VulkanDeviceHandles& handles ) override;
        void OnDeviceDestroying() override;

    private:
        DeviceRequirements() = default;
    };
} // namespace Desert::Graphic::API::Vulkan::FidelityFX
