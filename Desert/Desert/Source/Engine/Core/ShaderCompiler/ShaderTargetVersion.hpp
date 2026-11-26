#pragma once

#include <cstdint>

namespace Desert::Core
{
    // THE OLDEST DEVICE THE ENGINE RUNS ON, owned by the shader target (ShaderTarget.hpp): SPIR-V is cached on
    // disk and shipped, so it is compiled for this version and a device below it cannot run it. 1.2 (RT-SCENE):
    // ray query needs SPIR-V 1.4, which a 1.1 target cannot emit; every ray-tracing device is 1.2+ anyway (the AS
    // rows route from 1.2), and 1.1-only drivers are pre-2020. QueryMemory's vkGetPhysicalDeviceMemoryProperties2
    // and the vkGetPhysicalDeviceFeatures2 probes are core since 1.1. Encoded as VK_MAKE_API_VERSION(0, 1, 2, 0)
    // without the Vulkan header, so the shader compiler's headers stay free of the backend; the Vulkan backend
    // checks the encoding against VK_API_VERSION_1_2 (DeviceCaps.hpp).
    inline constexpr uint32_t kMinimumDeviceApiVersion = ( 1u << 22u ) | ( 2u << 12u );

    // The major and minor numbers of an API version in that encoding (VK_API_VERSION_MAJOR / _MINOR).
    [[nodiscard]] constexpr uint32_t ApiVersionMajor( uint32_t apiVersion )
    {
        return ( apiVersion >> 22u ) & 0x7Fu;
    }
    [[nodiscard]] constexpr uint32_t ApiVersionMinor( uint32_t apiVersion )
    {
        return ( apiVersion >> 12u ) & 0x3FFu;
    }
} // namespace Desert::Core
