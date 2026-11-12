#pragma once

#include <CoreReflection/ReflectionMacros.hpp>

#include <cstdint>

// THE LEVEL'S BAKE SETTINGS (UE: AWorldSettings::LightmassSettings, FLightmassWorldInfoSettings, plus GPU Lightmass's
// UGPULightmassSettings). Q1 (lead, 10-22): a member `Lightmass` of Core::SceneSettings — the World Settings — drawn
// by the World Settings panel from this reflection; no separate settings component or asset. Their hash is
// LightingBuildDataAsset::SettingsHash and StaticLightingInputs::SettingsHash (Assets::HashLightmassSettings), so a
// changed setting stales every record of the build at once.
//
// In its own header, not in LightmassBake.hpp, because the World Settings (Core) and the cook's gate (Assets) state
// and hash it, and neither may pull in the frame graph and the ray tracing scene the baker needs.
namespace Desert::Graphic::StaticLighting
{
    struct LightmassSettings
    {
        REFLECT()

        PROPERTY( DisplayName( "GI Samples" ), Category( "Lightmass" ), Summary, Range( 16, 65536 ),
                  Tooltip( "Path-traced samples per lightmap texel (GPU Lightmass GISamples)." ) )
        uint32_t GISamplesPerTexel = 512;

        PROPERTY( DisplayName( "Stationary Light Shadow Samples" ), Category( "Lightmass" ), Range( 8, 4096 ),
                  Tooltip( "Shadow rays per texel towards a stationary light's area, for its distance-field "
                           "shadow map (GPU Lightmass StationaryLightShadowSamples)." ) )
        uint32_t StationaryShadowSamples = 128;

        PROPERTY( DisplayName( "Max Bounces" ), Category( "Lightmass" ), Range( 1, 32 ),
                  Tooltip( "Path length of the bake's indirect light (CPU Lightmass NumIndirectLightingBounces)." ) )
        uint32_t MaxBounces = 8;

        PROPERTY( DisplayName( "Static Lighting Level Scale" ), Category( "Lightmass" ), Range( 0.1f, 10.0f ),
                  Units( "x" ),
                  Tooltip( "Scales every lightmap resolution of the level (UE StaticLightingLevelScale)." ) )
        float StaticLightingLevelScale = 1.0f;

        PROPERTY( DisplayName( "Build Volumetric Lightmap" ), Category( "Lightmass" ),
                  Tooltip( "Bake the volumetric lightmap that lights Movable objects, foliage, grass, particles "
                           "and fog with the baked indirect (UE r.VolumetricLightmap)." ) )
        bool BuildVolumetricLightmap = true;

        PROPERTY( DisplayName( "Volumetric Lightmap Detail Cell Size" ), Category( "Lightmass" ), Length,
                  Range( 50.0f, 2000.0f ),
                  Tooltip( "Finest cell of the volumetric lightmap near Static surfaces, cm (UE default 200)." ) )
        float VolumetricLightmapDetailCellSize = 200.0f;

        PROPERTY( DisplayName( "Volumetric Lightmap Max Brick Memory" ), Category( "Lightmass" ), Advanced,
                  Range( 1, 4096 ), Units( "MB" ),
                  Tooltip( "Ceiling of the volume's brick atlas (UE VolumetricLightmapMaximumBrickMemoryMb)." ) )
        uint32_t VolumetricLightmapMaxBricksMB = 128;

        PROPERTY( DisplayName( "Use Sky Occlusion" ), Category( "Lightmass" ),
                  Tooltip( "Bake the bent normal and visible sky fraction per texel (UE bUseSkyOcclusion)." ) )
        bool UseSkyOcclusion = true;

        PROPERTY( DisplayName( "Denoise" ), Category( "Lightmass" ),
                  Tooltip( "Q3: Intel Open Image Denoise over the baked lightmaps and volume, as UE GPU Lightmass." ) )
        bool Denoise = true;

        PROPERTY( DisplayName( "Frame Budget" ), Category( "Lightmass" ), Advanced, Range( 1.0f, 500.0f ),
                  Units( "ms" ),
                  Tooltip( "GPU time the progressive bake may add to one editor frame, keeping the viewport "
                           "interactive. Does not change the result, so it is not in the build's hash." ) )
        float FrameBudgetMs = 30.0f;

        [[nodiscard]] bool operator==( const LightmassSettings& ) const = default;
    };
} // namespace Desert::Graphic::StaticLighting
