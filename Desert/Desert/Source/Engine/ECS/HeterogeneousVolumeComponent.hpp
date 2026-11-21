#pragma once

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>

#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Assets/Common.hpp>

#include <cstdint>

namespace Desert::ECS
{
    // UE 5.3+ Heterogeneous Volumes (UHeterogeneousVolumeComponent): a box of smoke, steam or fire whose density
    // varies inside it, rendered by its own lit ray march (Graphic::System::HeterogeneousVolumeRenderer) rather
    // than voxelised into the volumetric fog's froxels - a froxel is metres deep at range and would smear a smoke
    // plume into a fog bank. The medium is lit by the same sun (cascades + cloud shadow), sky and local lights the
    // froxels read (Common/VolumeLighting.glslh, one text for both), self-shadowed through a per-volume sun
    // transmittance volume, and emits from its temperature (blackbody) for fire.
    //
    // THE PLACEMENT IS NOT A FIELD. The box's centre, rotation and scale are the entity's world transform; Extent
    // is the half size (cm) in the entity's local space.
    //
    // THE DENSITY SOURCE. A procedural field (fBm of value noise, advected by Wind, shaped by a soft
    // radial/vertical falloff so the box never shows), or a BAKED SPARSE VOLUME (`.dsvol`, UE's
    // SparseVolumeTexture: a simulated grid as bricks + page table, Assets::SparseVolumeAsset) stretched over the
    // box; both answer through one sampler (HvMedium, Common/HeterogeneousVolume.glslh).
    //
    // ZERO COST WHEN NOT SEEN: a volume whose box is outside the view frustum is not gathered, and with no
    // gathered volume the renderer declares no pass and allocates nothing.
    //
    // UNITS. Extent, NoiseScale in centimetres; Extinction is per metre (UE's authored unit) and converted to per
    // centimetre once (Graphic::PackHeterogeneousVolume); Temperature in Kelvin.
    enum class HeterogeneousVolumeDensitySource : uint8_t
    {
        ProceduralNoise,
        // The `.dsvol` named by HeterogeneousVolumeData::SparseVolume; its density times Extinction, its temperature
        // (when the grid has one) drives the blackbody instead of Temperature.
        BakedSparse
    };

    struct HeterogeneousVolumeData
    {
        REFLECT()

        PROPERTY( DisplayName( "Enabled" ), Category( "Heterogeneous Volume" ), Summary,
                  Tooltip( "Off: the volume is not gathered; with no gathered volume the frame declares no "
                           "heterogeneous-volume pass at all." ) )
        bool Enabled = true;

        PROPERTY( DisplayName( "Density Source" ), Category( "Heterogeneous Volume" ),
                  Tooltip( "Where the density comes from: Procedural Noise (a turbulent fBm field) or Baked "
                           "Sparse (the Sparse Volume asset, a simulated smoke / fire grid)." ) )
        HeterogeneousVolumeDensitySource DensitySource = HeterogeneousVolumeDensitySource::ProceduralNoise;

        PROPERTY( DisplayName( "Sparse Volume" ), Category( "Heterogeneous Volume" ), Asset<SparseVolumeAsset>,
                  Tooltip( "The baked grid (.dsvol, imported from a .vol) read under Density Source Baked Sparse, "
                           "stretched over the box. None under Baked Sparse = the volume is not drawn." ) )
        Assets::AssetHandle SparseVolume;

        PROPERTY(
             DisplayName( "Extent" ), Category( "Heterogeneous Volume" ), Length, Range( 1.0f, 100000.0f ),
             Tooltip( "Half size of the box along the entity's local X, Y, Z (cm), before the entity's scale." ) )
        glm::vec3 Extent = { 200.0f, 300.0f, 200.0f };

        PROPERTY( DisplayName( "Extinction" ), Category( "Heterogeneous Volume" ), Range( 0.0f, 100.0f ),
                  Tooltip( "Extinction of the densest smoke (per metre). 5 is a thick plume, 0.5 a haze." ) )
        float Extinction = 5.0f;

        PROPERTY( DisplayName( "Albedo" ), Category( "Heterogeneous Volume" ), Color,
                  Tooltip( "Fraction of the extinguished light scattered rather than absorbed, per channel: ~0.9 "
                           "white smoke or steam, ~0.2 soot." ) )
        glm::vec3 Albedo = { 0.85f, 0.85f, 0.85f };

        PROPERTY( DisplayName( "Phase G" ), Category( "Heterogeneous Volume" ), Range( -0.9f, 0.9f ),
                  Tooltip( "Henyey-Greenstein anisotropy: 0 isotropic, > 0 forward scattering (bright toward "
                           "the sun)." ) )
        float PhaseG = 0.3f;

        PROPERTY( DisplayName( "Noise Scale" ), Category( "Heterogeneous Volume" ), Length,
                  Range( 1.0f, 100000.0f ), Tooltip( "Size of the largest smoke billow (cm)." ) )
        float NoiseScale = 150.0f;

        PROPERTY( DisplayName( "Coverage" ), Category( "Heterogeneous Volume" ), Range( 0.0f, 1.0f ),
                  Tooltip( "How much of the box the smoke fills: 0 wisps, 1 solid." ) )
        float Coverage = 0.55f;

        PROPERTY( DisplayName( "Wind" ), Category( "Heterogeneous Volume" ),
                  Tooltip( "Velocity the noise is advected with, in the entity's local space (cm/s): a rising "
                           "plume is +Y." ) )
        glm::vec3 Wind = { 0.0f, 60.0f, 0.0f };

        PROPERTY( DisplayName( "Edge Fade" ), Category( "Heterogeneous Volume" ), Range( 0.001f, 1.0f ),
                  Tooltip( "Width of the density fade toward the box's faces, as a fraction of its half size, so "
                           "the volume never shows its box." ) )
        float EdgeFade = 0.35f;

        PROPERTY( DisplayName( "Emissive" ), Category( "Heterogeneous Volume" ), Color,
                  Tooltip( "Light the medium emits on its own, per unit of its extinction." ) )
        glm::vec3 Emissive = { 0.0f, 0.0f, 0.0f };

        PROPERTY(
             DisplayName( "Temperature" ), Category( "Heterogeneous Volume" ), Range( 0.0f, 4000.0f ),
             Tooltip( "Peak temperature of the medium (K), at the densest point near the bottom: above ~800 K "
                      "it glows as a blackbody (fire). 0 = cold smoke, no blackbody term." ) )
        float Temperature = 0.0f;

        PROPERTY( DisplayName( "Blackbody Intensity" ), Category( "Heterogeneous Volume" ), Range( 0.0f, 1000.0f ),
                  Tooltip( "Scale on the blackbody emission (UE's Temperature emission scale)." ) )
        float BlackbodyIntensity = 1.0f;
    };

    struct HeterogeneousVolumeComponent
    {
        COMPONENT( Key( "HeterogeneousVolume" ), Block( Data ), Run( SkyAndAtmosphere ) )
        HeterogeneousVolumeData Data;
    };
} // namespace Desert::ECS
