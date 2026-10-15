#include <Engine/Graphic/API/Vulkan/VulkanDeviceExtensions.hpp>

#include <array>

#if defined( DESERT_WITH_STREAMLINE )
#include <Engine/Graphic/API/Vulkan/Streamline/StreamlineRuntime.hpp>
#endif

namespace Desert::Graphic::API::Vulkan
{
    std::span<IVulkanDeviceExtension* const> VulkanDeviceExtensions()
    {
#if defined( DESERT_WITH_STREAMLINE )
        // NVIDIA Streamline (DLSS Super Resolution): Windows builds only (premake defines DESERT_WITH_STREAMLINE and
        // compiles the Streamline/ directories there alone).
        static const std::array<IVulkanDeviceExtension*, 1> kExtensions{ &Streamline::Runtime::Get() };
        return kExtensions;
#else
        return {};
#endif
    }
} // namespace Desert::Graphic::API::Vulkan
