#include <Engine/Graphic/Rhi/DeviceProfile.hpp>

#include <algorithm>
#include <vector>

namespace Desert::Graphic
{
    namespace
    {
        // THE PROFILE TABLE (UE DeviceProfiles.ini), ONE SOURCE. What each card's driver lacks against this
        // engine's rows, and its memory. GTX 10 (Pascal): no ray tracing driver path, no native 16-bit arithmetic
        // (D3D12 Native16BitShaderOps is Turing+), no tensor cores. GTX 16 (Turing TU116/117): fp16 native, no
        // tensor cores; the 1660's driver exposes acceleration structures + ray tracing pipelines over the shader
        // cores but not ray query, the 1650 (4 GB) none of them. RTX 2060: hardware RT, tensor cores (DLSS, RR;
        // frame generation is Ada+, and the catalog lists none). PerfIndexCap 0 everywhere: the profile does not
        // imitate speed, the recommender's measured index stands.
        const std::vector<DeviceProfile>& DeviceProfileTable()
        {
            using Common::Scalability::RayTracingAcceleration;
            static const std::vector<DeviceProfile> table = {
                 DeviceProfile{ .Name                  = "gtx1060",
                                .HideRayTracing        = true,
                                .RayTracingAccel       = RayTracingAcceleration::None,
                                .HideVendorUpscalers   = true,
                                .HideCooperativeMatrix = true,
                                .HideShaderFloat16     = true,
                                .VideoMemoryMiB        = 6144 },
                 DeviceProfile{ .Name                  = "gtx1650",
                                .HideRayTracing        = true,
                                .RayTracingAccel       = RayTracingAcceleration::None,
                                .HideVendorUpscalers   = true,
                                .HideCooperativeMatrix = true,
                                .VideoMemoryMiB        = 4096 },
                 DeviceProfile{ .Name                  = "gtx1660",
                                .HideRayQuery          = true,
                                .RayTracingAccel       = RayTracingAcceleration::Emulated,
                                .HideVendorUpscalers   = true,
                                .HideCooperativeMatrix = true,
                                .VideoMemoryMiB        = 6144 },
                 DeviceProfile{ .Name            = "rtx2060",
                                .RayTracingAccel = RayTracingAcceleration::Hardware,
                                .Architecture    = Common::Scalability::GpuArchitecture::Turing,
                                .VideoMemoryMiB  = 6144 },
            };
            return table;
        }
    } // namespace

    Common::ResultStr<DeviceProfile> FindDeviceProfile( std::string_view name )
    {
        std::string known;
        for ( const DeviceProfile& profile : DeviceProfileTable() )
        {
            if ( profile.Name == name )
                return Common::MakeSuccess( profile );
            if ( !known.empty() )
                known += ", ";
            known += profile.Name;
        }
        return Common::MakeFormattedError<DeviceProfile>(
             "--device-profile '{}' is not a device profile; known: {}", name, known );
    }

    uint64_t ProfiledDeviceLocalBytes( const DeviceProfile& profile, uint64_t heapBytes )
    {
        if ( profile.VideoMemoryMiB == 0 )
            return heapBytes;
        return std::min<uint64_t>( heapBytes, profile.VideoMemoryMiB * 1024ull * 1024ull );
    }
} // namespace Desert::Graphic
