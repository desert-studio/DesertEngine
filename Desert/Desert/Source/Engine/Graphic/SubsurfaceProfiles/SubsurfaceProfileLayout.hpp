#pragma once

#include <cstdint>

namespace Desert::Graphic::SubsurfaceProfileLayout
{
    // THE PROFILE ATLAS ROW (SSS-PROFILE): one RGBA16F row per profile, these columns - a port of UE
    // SubsurfaceProfileCommon.ush:13-29 (SSSS_*_OFFSET). The GLSL twin is Shaders/Common/SubsurfaceProfileLayout.glslh;
    // a census suite holds the two equal (tests are off, 10-15: listed in the card's REMAINDER).
    inline constexpr uint32_t kTint                  = 0;  // Tint.rgb, a = Implementation (0 AFIS, 1 Separable)
    inline constexpr uint32_t kSurfaceAlbedo         = 1;  // SurfaceAlbedo.rgb, a = 1
    inline constexpr uint32_t kDiffuseMeanFreePath   = 2;  // MeanFreePathColor * MeanFreePathDistance (cm).rgb
    inline constexpr uint32_t kTransmission          = 3;  // ExtinctionScale, NormalScale, ScatteringDistribution, 1/IOR
    inline constexpr uint32_t kBoundaryColorBleed    = 4;  // BoundaryColorBleed.rgb
    inline constexpr uint32_t kDualSpecular          = 5;  // Roughness0, Roughness1, LobeMix
    inline constexpr uint32_t kKernel0               = 6;  // Separable kernel, 13 taps
    inline constexpr uint32_t kKernel0Size           = 13;
    inline constexpr uint32_t kKernel1               = 19; // 9 taps
    inline constexpr uint32_t kKernel1Size           = 9;
    inline constexpr uint32_t kKernel2               = 28; // 6 taps
    inline constexpr uint32_t kKernel2Size           = 6;
    inline constexpr uint32_t kTransmissionProfile   = 34; // 32 texels of the Burley transmission over thickness
    inline constexpr uint32_t kTransmissionProfileSize = 32;
    inline constexpr uint32_t kAtlasWidth            = 66;
    // UE SSSS_MAX_TRANSMISSION_PROFILE_DISTANCE, in cm (the project unit): the thickness the last texel stands for.
    inline constexpr float kMaxTransmissionDistance = 5.0f;

    static_assert( kTransmissionProfile + kTransmissionProfileSize == kAtlasWidth );
    static_assert( kKernel2 + kKernel2Size == kTransmissionProfile );
} // namespace Desert::Graphic::SubsurfaceProfileLayout
