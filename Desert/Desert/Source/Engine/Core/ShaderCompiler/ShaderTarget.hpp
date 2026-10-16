#pragma once

#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include <shaderc/shaderc.h>

#include <cstdint>

namespace Desert::Core
{
    // THE SHADER TARGET IS THE OLDEST ACCEPTED DEVICE, NOT THE CURRENT ONE. SPIR-V is cached on disk and
    // shipped, so its target cannot follow the GPU of the machine that compiled it; it follows
    // kMinimumDeviceApiVersion, which every device that passes CheckRequired meets. This header is the
    // only place the compiler's target environment and the cache key's options fingerprint read it from.
    //
    // Vulkan 1.2 targets SPIR-V 1.5, which carries what ray query needs (SPIR-V 1.4: SPV_KHR_ray_query
    // and its OpRayQuery* instructions are rejected below it).
    [[nodiscard]] constexpr shaderc_env_version ShaderTargetEnvVersion( uint32_t apiVersion )
    {
        switch ( VK_API_VERSION_MINOR( apiVersion ) )
        {
            case 0:
                return shaderc_env_version_vulkan_1_0;
            case 1:
                return shaderc_env_version_vulkan_1_1;
            case 2:
                return shaderc_env_version_vulkan_1_2;
            default:
                return shaderc_env_version_vulkan_1_3;
        }
    }

    inline constexpr uint32_t            kShaderTargetApiVersion = Graphic::API::Vulkan::kMinimumDeviceApiVersion;
    inline constexpr shaderc_env_version kShaderTargetEnvVersion =
         ShaderTargetEnvVersion( kShaderTargetApiVersion );
} // namespace Desert::Core
