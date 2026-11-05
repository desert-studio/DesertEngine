#pragma once

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include <Common/Settings/CapabilityCatalog.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

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

    // struct DeviceProfile lives in DeviceCaps.hpp (unchanged), so DeviceCaps can carry the applied one to every
    // reader past the probe (the catalog builder, VMA, the memory readout) without a second copy of its fields.

    // THE PROFILE TABLE, ONE SOURCE (DeviceCapsProbe.cpp): the profile named @p name, or an error that names
    // every profile the table knows. "" is not a name (no profile is the default DeviceProfile{}).
    [[nodiscard]] Common::ResultStr<DeviceProfile> FindDeviceProfile( std::string_view name );

    // The host's choice (Editor and Runtime --device-profile), made before the device exists; read once by
    // VulkanLogicalDevice::Create into DeviceRestrictions::Profile. Unknown name: the FindDeviceProfile error and
    // nothing changes. Never called = no profile (DeviceProfile{}), today's behaviour.
    [[nodiscard]] Common::BoolResultStr RequestDeviceProfile( std::string_view name );
    [[nodiscard]] const DeviceProfile&  RequestedDeviceProfile();

    // A device-local heap of @p heapBytes as the profiled device has it: min(heap, VideoMemoryMiB); unchanged when
    // the profile caps nothing. The one rule for VMA's pHeapSizeLimit, QueryMemory, VideoMemory and the catalog.
    [[nodiscard]] uint64_t ProfiledDeviceLocalBytes( const DeviceProfile& profile, uint64_t heapBytes );

    // What this machine withholds from the engine on top of what the driver offers (machine.json; the host reads
    // Common::Settings::MachineSettings and passes it here, so this file stays free of the settings store).
    struct DeviceRestrictions
    {
        // HideRayTracing: the device is probed as one without ray tracing (DeviceCaps HideRayTracing).
        bool HideRayTracing = false;
        // The --device-profile the host passes (DeviceProfile, DeviceCaps.hpp); applied after HideRayTracing.
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
