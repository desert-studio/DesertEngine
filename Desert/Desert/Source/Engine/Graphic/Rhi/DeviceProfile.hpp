#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Settings/CapabilityCatalog.hpp>

#include <cstdint>
#include <string>
#include <string_view>

// A LOWER DEVICE THE ENGINE PLAYS ON THIS ONE, NEUTRAL. It names what a card lacks (rows, memory, vendor
// upscalers), never how a backend withholds it: each backend's probe applies the profile to its own rows before
// the driver is asked (Vulkan: DeviceCapsProbe ApplyProfile), and the applied one travels on with the caps.
namespace Desert::Graphic
{
    // UE DeviceProfiles + Preview Rendering Level: the probe withholds what the named device lacks before the caps
    // and the catalog are built, so every reader sees that device's facts. It does not imitate its speed. Source:
    // the --device-profile CLI flag (Editor and Runtime), carried by RhiSelection; never machine.json.
    // Name empty = no profile.
    struct DeviceProfile
    {
        std::string Name;                   ///< "gtx1060", "gtx1650", "gtx1660", "rtx2060"; "" = none
        bool        HideRayTracing = false; ///< no AS / RQ / pipeline rows (GTX without the RT driver)
        bool        HideRayQuery   = false; ///< AS + pipeline kept, ray query withheld
        Common::Scalability::RayTracingAcceleration RayTracingAccel =
             Common::Scalability::RayTracingAcceleration::Hardware; ///< Emulated on GTX 10/16
        bool HideVendorUpscalers = false; ///< no DLSS / DLSS-RR / frame generation (no tensor cores)
        Common::Scalability::GpuArchitecture Architecture =
             Common::Scalability::GpuArchitecture::Other; ///< the played card's (CapabilityCatalog::Architecture)
        bool     HideCooperativeMatrix = false;
        bool     HideShaderFloat16     = false;
        bool     HideInt64Atomics      = false;
        uint64_t VideoMemoryMiB        = 0;    ///< device-local heap cap; 0 = the device's
        float    PerfIndexCap          = 0.0f; ///< recommender's GPU index ceiling; 0 = measured
    };

    // THE PROFILE TABLE, ONE SOURCE (DeviceProfile.cpp): the profile named @p name, or an error that names every
    // profile the table knows. "" is not a name (no profile is the default DeviceProfile{}).
    [[nodiscard]] Common::ResultStr<DeviceProfile> FindDeviceProfile( std::string_view name );

    // A device-local heap of @p heapBytes as the profiled device has it: min(heap, VideoMemoryMiB); unchanged when
    // the profile caps nothing. The one rule for the allocator's heap cap, the memory budget and the catalog.
    [[nodiscard]] uint64_t ProfiledDeviceLocalBytes( const DeviceProfile& profile, uint64_t heapBytes );
} // namespace Desert::Graphic
