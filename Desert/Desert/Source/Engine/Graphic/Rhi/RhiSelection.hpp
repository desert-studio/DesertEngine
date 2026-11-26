#pragma once

#include <Engine/Graphic/Rhi/DeviceProfile.hpp>
#include <Engine/Graphic/Rhi/RhiKind.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Desert::Graphic
{
    // Which preference won; logged once at start ("[Rhi] vulkan from default, adapter auto from default").
    enum class RhiSource : uint8_t
    {
        CommandLine,
        MachineSettings,
        Default
    };

    // Which physical adapter the backend opens. Text form (--adapter, machine.json "Adapter"):
    //   "auto"                      the backend's own rank (discrete > integrated > virtual > cpu)
    //   "<n>"                       the n-th adapter the backend lists, from 0
    //   "nvidia" | "amd" | "intel"  the best-ranked adapter of that vendor (0x10DE / 0x1002 / 0x8086)
    //   "vendor:0x<hex>"            the best-ranked adapter of that PCI vendor id
    //   "name:<text>"               the best-ranked adapter whose name contains <text>
    // A preference no adapter meets is the backend's refusal, never a quiet switch to another adapter.
    struct AdapterPreference
    {
        enum class Kind : uint8_t
        {
            Auto,
            Index,
            Vendor,
            Name
        } By                 = Kind::Auto;
        uint32_t    Index    = 0;
        uint32_t    VendorId = 0;
        std::string NameSubstring;
    };

    struct RhiSelection
    {
        RhiKind           Kind     = RhiKind::Vulkan;
        RhiSource         KindFrom = RhiSource::Default;
        AdapterPreference Adapter;
        RhiSource         AdapterFrom = RhiSource::Default;
        // --device-profile: the lower device the backend plays (DeviceProfile.hpp). Name empty = none.
        DeviceProfile Profile;
    };

    struct RhiSelectionInputs
    {
        std::optional<std::string> CommandLineRhi;           // --rhi
        std::optional<std::string> CommandLineAdapter;       // --adapter
        std::optional<std::string> CommandLineDeviceProfile; // --device-profile
        std::optional<std::string> MachineRhi;               // machine.json "Rhi"
        std::optional<std::string> MachineAdapter;           // machine.json "Adapter"
    };

    // Precedence: command line > machine.json > Vulkan (a project default arrives with
    // the D3D12 renderer: ProjectFile lives in the shared submodule). Refuses an unknown backend name, d3d12 on a
    // non-Windows build, malformed adapter text and an unknown device profile. Never falls back silently.
    [[nodiscard]] Common::ResultStr<RhiSelection>      ResolveRhiSelection( const RhiSelectionInputs& in );
    [[nodiscard]] Common::ResultStr<AdapterPreference> ParseAdapterPreference( std::string_view text );
    // The one start-up line: "vulkan from --rhi, adapter 'name:RTX' from machine.json, profile gtx1660".
    [[nodiscard]] std::string Describe( const RhiSelection& selection );
    // "auto", "#1", "vendor 0x10DE", "name 'RTX'": the adapter preference in a log line or a refusal.
    [[nodiscard]] std::string Describe( const AdapterPreference& adapter );
} // namespace Desert::Graphic
