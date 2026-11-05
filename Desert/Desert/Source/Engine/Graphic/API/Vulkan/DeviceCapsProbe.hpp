#pragma once

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include <Common/Settings/CapabilityCatalog.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace vkb
{
    struct Instance;
    struct PhysicalDevice;
} // namespace vkb

// THE ONLY CODE THAT ASKS THE DRIVER WHAT A DEVICE HAS. Every feature and extension question goes
// through vk-bootstrap here, on the route DeviceCaps planned, and every "yes" is enabled on the spot, so
// the vkb::PhysicalDevice handed back carries exactly the set DeviceCaps records — the logical device is
// built from that object and nothing else (VulkanLogicalDevice::CreateDevice). DeviceCapsCensus pins
// that no other engine file enumerates devices, features or extensions, or creates an instance or device.
namespace Desert::Graphic::API::Vulkan
{
    struct ProbedDevice
    {
        std::shared_ptr<vkb::PhysicalDevice> Physical; ///< with every present row already enabled
        DeviceCaps                           Caps;
    };

    // A lower device the engine plays on this one (UE DeviceProfiles + Preview Rendering Level): the probe
    // withholds what the named device lacks before DeviceCaps and the catalog are built, so every reader sees
    // that device's facts. It does not imitate its speed. Source: the --device-profile CLI flag (Editor and
    // Runtime), never machine.json. Name empty = no profile.
    struct DeviceProfile
    {
        std::string Name;                              ///< "gtx1060", "gtx1650", "gtx1660", "rtx2060"; "" = none
        bool        HideRayTracing      = false;       ///< no AS / RQ / pipeline rows (GTX without the RT driver)
        bool        HideRayQuery        = false;       ///< AS + pipeline kept, VK_KHR_ray_query withheld
        Common::Scalability::RayTracingAcceleration RayTracingAccel =
             Common::Scalability::RayTracingAcceleration::Hardware; ///< Emulated on GTX 10/16
        bool     HideVendorUpscalers = false; ///< no DLSS / DLSS-RR / frame generation (no tensor cores)
        bool     HideCooperativeMatrix = false;
        bool     HideShaderFloat16     = false;
        bool     HideInt64Atomics      = false;
        uint64_t VideoMemoryMiB        = 0;   ///< device-local heap cap (VMA pHeapSizeLimit); 0 = the device's
        float    PerfIndexCap          = 0.0f; ///< recommender's GPU index ceiling; 0 = measured
    };

    // What this machine withholds from the engine on top of what the driver offers (machine.json; the host reads
    // Common::Settings::MachineSettings and passes it here, so this file stays free of the settings store).
    struct DeviceRestrictions
    {
        // HideRayTracing: the device is probed as one without ray tracing (DeviceCaps HideRayTracing).
        bool HideRayTracing = false;
        // The --device-profile the host passes (DeviceProfile above); applied after HideRayTracing.
        DeviceProfile Profile;
    };

    // The instance must be created as VulkanContext creates it: apiVersion kMaximumApiVersion (a device's
    // usable version is min(instance apiVersion, device), and routes are planned for min(device, 1.3), so a
    // lower instance turns rows planned as core into extensions enabled without their dependencies) and
    // with the window-system extensions (the required swapchain row depends on VK_KHR_surface).
    // RenderGraphVulkan creates its device through here and fails on any vkCreateDevice validation message.
    //
    // Probes every device the instance lists, keeps those that pass CheckRequired, and returns the best
    // by type (discrete > integrated > virtual > cpu). When none passes, the error names every device
    // and what each one lacks.
    [[nodiscard]] Common::ResultStr<ProbedDevice> SelectDevice( const vkb::Instance&      instance,
                                                                const DeviceRestrictions& restrictions );
} // namespace Desert::Graphic::API::Vulkan
