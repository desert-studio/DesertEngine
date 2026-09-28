#pragma once

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include <memory>

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

    // Probes every device the instance lists, keeps those that pass CheckRequired, and returns the best
    // by type (discrete > integrated > virtual > cpu). When none passes, the error names every device
    // and what each one lacks.
    [[nodiscard]] Common::ResultStr<ProbedDevice> SelectDevice( const vkb::Instance& instance );
} // namespace Desert::Graphic::API::Vulkan
