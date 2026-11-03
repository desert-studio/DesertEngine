#include <Engine/Graphic/API/Vulkan/VulkanDeviceExtensions.hpp>

#if defined( DESERT_UPSCALER_FSR )
#include <Engine/Graphic/API/Vulkan/FidelityFX/FidelityFXDevice.hpp>
#endif
#if defined( DESERT_UPSCALER_DLSS )
#include <Engine/Graphic/API/Vulkan/Streamline/StreamlineRuntime.hpp>
#endif

namespace Desert::Graphic::API::Vulkan
{
    std::span<IVulkanDeviceExtension* const> VulkanDeviceExtensions()
    {
        // Windows builds only (the build defines DESERT_UPSCALER_FSR / DESERT_UPSCALER_DLSS and compiles the
        // FidelityFX/ and Streamline/ directories there alone). FidelityFX: the device the FSR 3.1 Vulkan backend
        // assumes (FidelityFXDevice.hpp). NVIDIA Streamline: DLSS Super Resolution.
#if defined( DESERT_UPSCALER_FSR ) || defined( DESERT_UPSCALER_DLSS )
        static IVulkanDeviceExtension* const kExtensions[]{
#if defined( DESERT_UPSCALER_FSR )
             &FidelityFX::DeviceRequirements::Get(),
#endif
#if defined( DESERT_UPSCALER_DLSS )
             &Streamline::Runtime::Get(),
#endif
        };
        return kExtensions;
#else
        return {};
#endif
    }
} // namespace Desert::Graphic::API::Vulkan
