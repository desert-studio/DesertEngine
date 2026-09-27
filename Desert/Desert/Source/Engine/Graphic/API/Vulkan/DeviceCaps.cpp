#include <Engine/Graphic/API/Vulkan/DeviceCaps.hpp>

#include <algorithm>
#include <format>

namespace Desert::Graphic::API::Vulkan
{
    namespace
    {
        using C = Capability;
        using N = CapabilityNeed;

        constexpr uint32_t V10 = VK_API_VERSION_1_0;
        constexpr uint32_t V12 = VK_API_VERSION_1_2;
        constexpr uint32_t V13 = VK_API_VERSION_1_3;

        // THE TABLE. Order matters twice: it is the enum's order (checked below), and it is the probe
        // order, so every row listed in DependsOn must come before the row that depends on it.
        constexpr std::array<CapabilitySpec, kCapabilityCount> kTable = { {
             { C::TessellationShader, "tessellationShader", N::Required, V10, {} },
             { C::Swapchain, "swapchain", N::Required, 0, VK_KHR_SWAPCHAIN_EXTENSION_NAME },
             // MoltenVK has no wide lines; the debug-line path clamps the width instead.
             { C::WideLines, "wideLines", N::Optional, V10, {} },
             { C::FillModeNonSolid, "fillModeNonSolid", N::Optional, V10, {} },
             { C::SamplerAnisotropy, "samplerAnisotropy", N::Optional, V10, {} },
             { C::TextureCompressionBC, "textureCompressionBC", N::Optional, V10, {} },
             // Not a capability the engine uses but a spec rule: an implementation that advertises it
             // (MoltenVK) must have it enabled. "Optional" gives exactly that: enabled iff present.
             { C::PortabilitySubset, "portability_subset", N::Optional, 0, "VK_KHR_portability_subset" },
             { C::MemoryBudget, "memory_budget", N::Optional, 0, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
             { C::TimelineSemaphore, "timelineSemaphore", N::Available, V12,
               VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME },
             { C::DescriptorIndexing, "descriptorIndexing", N::Available, V12,
               VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME },
             { C::BufferDeviceAddress, "bufferDeviceAddress", N::Available, V12,
               VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME },
             // Below 1.2 the extension needs depth_stencil_resolve and create_renderpass2 as extensions.
             { C::DynamicRendering, "dynamicRendering", N::Available, V13, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
               V12 },
             { C::Synchronization2, "synchronization2", N::Available, V13,
               VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME },
             { C::DeferredHostOperations, "deferred_host_operations", N::Optional, 0,
               VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, V12 },
             // Ray tracing: below 1.2 its dependencies (spirv_1_4, buffer_device_address, ...) are
             // extensions of their own, so the extension route starts at 1.2.
             { C::AccelerationStructure,
               "acceleration_structure",
               N::Optional,
               0,
               VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
               V12,
               { C::BufferDeviceAddress, C::DescriptorIndexing, C::DeferredHostOperations } },
             { C::RayQuery,
               "ray_query",
               N::Optional,
               0,
               VK_KHR_RAY_QUERY_EXTENSION_NAME,
               V12,
               { C::AccelerationStructure, C::Count, C::Count } },
             { C::RayTracingPipeline,
               "ray_tracing_pipeline",
               N::Optional,
               0,
               VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
               V12,
               { C::AccelerationStructure, C::Count, C::Count } },
        } };

        // Row i describes enum value i, so SpecOf() and DeviceCaps::Rows index the same thing; and every
        // dependency precedes its dependant, so the probe (table order) has settled it first.
        constexpr bool TableIsWellOrdered()
        {
            for ( std::size_t i = 0; i < kCapabilityCount; ++i )
            {
                if ( static_cast<std::size_t>( kTable[i].Id ) != i )
                    return false;
                for ( const Capability dependency : kTable[i].DependsOn )
                    if ( dependency != Capability::Count && static_cast<std::size_t>( dependency ) >= i )
                        return false;
            }
            return true;
        }
        static_assert( TableIsWellOrdered(), "CapabilityTable must follow the enum, dependencies first" );

        uint32_t MajorMinor( uint32_t version )
        {
            return VK_MAKE_API_VERSION( 0, VK_API_VERSION_MAJOR( version ), VK_API_VERSION_MINOR( version ), 0 );
        }
    } // namespace

    const std::array<CapabilitySpec, kCapabilityCount>& CapabilityTable()
    {
        return kTable;
    }

    const CapabilitySpec& SpecOf( Capability capability )
    {
        return kTable[static_cast<std::size_t>( capability )];
    }

    uint32_t UsedApiVersion( uint32_t deviceApiVersion )
    {
        return std::min( MajorMinor( deviceApiVersion ), kMaximumApiVersion );
    }

    std::string FormatApiVersion( uint32_t version )
    {
        return std::format( "{}.{}.{}", VK_API_VERSION_MAJOR( version ), VK_API_VERSION_MINOR( version ),
                            VK_API_VERSION_PATCH( version ) );
    }

    DeviceCaps PlanRoutes( std::string deviceName, std::string driver, uint32_t deviceApiVersion,
                           const std::function<bool( std::string_view )>& hasExtension )
    {
        DeviceCaps caps;
        caps.DeviceName       = std::move( deviceName );
        caps.Driver           = std::move( driver );
        caps.DeviceApiVersion = deviceApiVersion;
        caps.UsedApiVersion   = UsedApiVersion( deviceApiVersion );

        for ( std::size_t i = 0; i < kCapabilityCount; ++i )
        {
            const CapabilitySpec& spec  = kTable[i];
            CapabilityRoute       route = CapabilityRoute::Unreachable;
            // Plans against the USED version: a 1.4 device is driven as 1.3, so a 1.4-core row would still
            // go through its extension — the engine never relies on a version it does not target.
            if ( spec.CoreSince != 0 && caps.UsedApiVersion >= spec.CoreSince )
                route = CapabilityRoute::Core;
            else if ( !spec.Extension.empty() && caps.UsedApiVersion >= spec.ExtensionMinimumApi &&
                      hasExtension( spec.Extension ) )
                route = CapabilityRoute::Extension;
            caps.Rows[i].Route = route;
        }
        return caps;
    }

    bool DependenciesPresent( const DeviceCaps& caps, Capability capability )
    {
        for ( const Capability dependency : SpecOf( capability ).DependsOn )
        {
            if ( dependency != Capability::Count && !caps.Has( dependency ) )
                return false;
        }
        return true;
    }

    Common::BoolResultStr CheckRequired( const DeviceCaps& caps )
    {
        std::string missing;
        if ( MajorMinor( caps.DeviceApiVersion ) < kMinimumDeviceApiVersion )
        {
            missing += std::format( "Vulkan {} (device reports {})", FormatApiVersion( kMinimumDeviceApiVersion ),
                                    FormatApiVersion( caps.DeviceApiVersion ) );
        }
        for ( std::size_t i = 0; i < kCapabilityCount; ++i )
        {
            const CapabilitySpec& spec = kTable[i];
            if ( spec.Need != CapabilityNeed::Required || caps.Rows[i].Present )
                continue;
            if ( !missing.empty() )
                missing += ", ";
            missing += spec.Name;
            if ( !spec.Extension.empty() )
                missing += std::format( " ({})", spec.Extension );
        }
        if ( missing.empty() )
            return Common::MakeSuccess( true );
        return Common::MakeError<bool>(
             std::format( "the Vulkan device '{}' ({}) lacks what this engine requires: {}", caps.DeviceName,
                          caps.Driver, missing ) );
    }

    std::string FormatCapsTable( const DeviceCaps& caps )
    {
        std::string line =
             std::format( "device {} | driver {} | Vulkan {} (used {}) |", caps.DeviceName, caps.Driver,
                          FormatApiVersion( caps.DeviceApiVersion ), FormatApiVersion( caps.UsedApiVersion ) );
        for ( std::size_t i = 0; i < kCapabilityCount; ++i )
        {
            const CapabilitySpec& spec = kTable[i];
            const CapabilityRow&  row  = caps.Rows[i];
            const char*           how  = !row.Present                              ? "no"
                                         : row.Route == CapabilityRoute::Extension ? "ext"
                                         : spec.CoreSince == VK_API_VERSION_1_0    ? "1.0"
                                         : spec.CoreSince == VK_API_VERSION_1_2    ? "core1.2"
                                                                                   : "core1.3";
            const char*           need = spec.Need == CapabilityNeed::Required ? "!" : "";
            line += std::format( " {}{}={}", need, spec.Name, how );
        }
        return line;
    }
} // namespace Desert::Graphic::API::Vulkan
