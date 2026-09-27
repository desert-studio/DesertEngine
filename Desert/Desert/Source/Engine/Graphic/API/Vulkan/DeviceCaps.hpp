#pragma once

#include <Common/Core/ResultStr.hpp>

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// DEVICE CAPS — THE ONE PLACE THAT SAYS WHAT THIS DEVICE HAS, AND THROUGH WHAT.
//
// The engine is written against a MANDATORY FEATURE SET WITH ONE CODE PATH (owner, 2026-09-27). Every
// Vulkan capability the engine touches is a row in CapabilityTable(): its need (required / optional /
// available), the core version that carries it, and the extension that carries it on an older device.
// The ROUTE is chosen here, once — core when the device's version has it, otherwise the extension —
// and code downstream asks `Has()` instead of re-deriving it from versions and extension lists. A device
// that misses a REQUIRED row is refused with the list of what it misses (CheckRequired), never half-used.
//
// This header is pure: no vk-bootstrap, no Vulkan calls, so the planning and the refusal are testable on
// made-up devices (Desert/Tests/Engine/DeviceCaps). The part that asks the driver is DeviceCapsProbe.cpp.
namespace Desert::Graphic::API::Vulkan
{
    enum class Capability : uint8_t
    {
        // Vulkan 1.0 features and the swapchain — what the renderer uses today.
        TessellationShader,
        Swapchain,
        WideLines,
        FillModeNonSolid,
        SamplerAnisotropy,
        TextureCompressionBC,
        PortabilitySubset,
        MemoryBudget,
        // Vulkan 1.2 / 1.3 — asked for and recorded; VKF2 makes them required when it starts using them.
        TimelineSemaphore,
        DescriptorIndexing,
        BufferDeviceAddress,
        DynamicRendering,
        Synchronization2,
        // Ray tracing — optional for good; real code branches on these.
        DeferredHostOperations,
        AccelerationStructure,
        RayQuery,
        RayTracingPipeline,

        Count
    };
    inline constexpr std::size_t kCapabilityCount = static_cast<std::size_t>( Capability::Count );

    enum class CapabilityNeed : uint8_t
    {
        Required,  ///< the device is refused without it
        Optional,  ///< enabled when present; the engine has a real branch for its absence
        Available, ///< enabled when present and recorded; nothing branches on it yet (VKF2 will require it)
    };

    enum class CapabilityRoute : uint8_t
    {
        Unreachable, ///< neither the device's version nor its extension list carries it
        Core,        ///< in the core of the device's version (or plain Vulkan 1.0 features)
        Extension,   ///< through the extension named in the table row
    };

    struct CapabilitySpec
    {
        Capability       Id;
        std::string_view Name;
        CapabilityNeed   Need;
        // The core version that carries it; 0 = never core (a pure extension, e.g. the swapchain).
        uint32_t CoreSince = 0;
        // The extension that carries it below CoreSince; empty = core only.
        std::string_view Extension;
        // The extension route is only taken on a device at least this new: below it the extension's own
        // dependencies (spirv_1_4, depth_stencil_resolve, ...) are extensions too, and the engine does not
        // chase dependency chains on devices older than the ones it targets.
        uint32_t ExtensionMinimumApi = VK_API_VERSION_1_1;
        // Capabilities this one is not enabled without (ray tracing on acceleration structures, ...).
        std::array<Capability, 3> DependsOn{ Capability::Count, Capability::Count, Capability::Count };
    };

    [[nodiscard]] const std::array<CapabilitySpec, kCapabilityCount>& CapabilityTable();
    [[nodiscard]] const CapabilitySpec&                               SpecOf( Capability capability );

    // The oldest device the engine runs on: vkGetPhysicalDeviceMemoryProperties2 (QueryMemory) and
    // vkGetPhysicalDeviceFeatures2 (every probe below) are core 1.1. The shader compiler's target is tied
    // to this constant (ShaderCompiler.cpp), so SPIR-V never outruns the oldest accepted device.
    inline constexpr uint32_t kMinimumDeviceApiVersion = VK_API_VERSION_1_1;
    // The newest version the engine is written for; the instance, VMA and routing never go past it.
    inline constexpr uint32_t kMaximumApiVersion = VK_API_VERSION_1_3;

    // min(device, 1.3): what VMA is told and what routing plans against. The patch number is dropped —
    // VMA and the core-version checks compare major.minor only.
    [[nodiscard]] uint32_t UsedApiVersion( uint32_t deviceApiVersion );

    struct CapabilityRow
    {
        CapabilityRoute Route   = CapabilityRoute::Unreachable;
        bool            Present = false; ///< probed on the route AND enabled on the logical device
    };

    struct DeviceCaps
    {
        std::string DeviceName;
        std::string Driver; ///< driverName + driverInfo where the device reports them, else the number
        uint32_t    DeviceApiVersion = 0;
        uint32_t    UsedApiVersion   = 0;

        std::array<CapabilityRow, kCapabilityCount> Rows{};

        [[nodiscard]] bool Has( Capability capability ) const
        {
            return Rows[static_cast<std::size_t>( capability )].Present;
        }
        [[nodiscard]] CapabilityRoute RouteOf( Capability capability ) const
        {
            return Rows[static_cast<std::size_t>( capability )].Route;
        }
    };

    // Step 1, pure: choose each row's route from the device's version and extension list. Presence is
    // left false — only the probe, which asks the driver on exactly that route, may set it.
    [[nodiscard]] DeviceCaps PlanRoutes( std::string deviceName, std::string driver, uint32_t deviceApiVersion,
                                         const std::function<bool( std::string_view )>& hasExtension );

    // Step 2 happens in DeviceCapsProbe.cpp; between probes it asks this: are the rows this one needs
    // already present? (Rows are probed in table order and every dependency precedes its dependant.)
    [[nodiscard]] bool DependenciesPresent( const DeviceCaps& caps, Capability capability );

    // Step 3, pure: the refusal. Success, or an error naming the device, its version against the
    // minimum, and every required row it misses — all of them, not the first.
    [[nodiscard]] Common::BoolResultStr CheckRequired( const DeviceCaps& caps );

    // The start-up log line: device | driver | version | every row as name=core/ext/no.
    [[nodiscard]] std::string FormatCapsTable( const DeviceCaps& caps );

    [[nodiscard]] std::string FormatApiVersion( uint32_t version );
} // namespace Desert::Graphic::API::Vulkan
