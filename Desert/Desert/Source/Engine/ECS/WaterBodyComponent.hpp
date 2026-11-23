#pragma once

// A WATER BODY (UE: AWaterBodyOcean / UWaterBodyOceanComponent). The entity's world position is the ocean's
// plane height and its WaveOrigin; its `.dwaves` wave set gives the shared Gerstner waves. Physics gathers
// every body each step into Water::WaterSubsystem (ECS/System/WaterBodyGather.hpp), which answers the
// water queries buoyancy, swimming and scripts ask (UE QueryWaterInfoClosestToWorldLocation).

#include <Engine/Assets/Common.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <Engine/Water/WaterRiver.hpp>

#include <glm/vec2.hpp>

#include <span>
#include <vector>

namespace Desert::ECS
{
    struct WaterBodyData
    {
        REFLECT()

        PROPERTY( DisplayName( "Water Waves" ), Category( "Wave" ), Summary, Asset<WaterWavesAsset>,
                  Tooltip( "The wave set of this body (UE WaterWaves) — drag a .dwaves from the Content "
                           "Browser. An empty slot is a flat ocean; a set that cannot be read is refused at "
                           "Play by name." ) )
        Assets::AssetHandle WaterWaves;

        PROPERTY( DisplayName( "Ocean Extents" ), Category( "Water" ), Length,
                  Tooltip( "The footprint of the ocean around the entity, X by Z (UE OceanExtents). A point "
                           "outside it is not in this body." ) )
        glm::vec2 OceanExtents{ 51200.0f, 51200.0f };

        PROPERTY( DisplayName( "Target Wave Mask Depth" ), Category( "Wave" ), Length, Range( 1.0f, 100000.0f ),
                  Tooltip( "Water depth at which the waves reach their full height; shallower water damps "
                           "them (UE TargetWaveMaskDepth)." ) )
        float TargetWaveMaskDepth = 2048.0f;

        PROPERTY( DisplayName( "Water Material" ), Category( "Rendering" ), Summary, Asset<MaterialAsset>,
                  Role( WaterMaterial ),
                  Tooltip( "The look of this body (UE UWaterBodyComponent::WaterMaterial): a .demat of a "
                           "SingleLayerWater-domain shader - its scattering, absorption, phase and ripple "
                           "maps. Empty draws no water; a material of another domain is refused by name." ) )
        Assets::AssetHandle WaterMaterial;

        // WATER-RIVERS-FFT R4: added with defaults (a body without them is the ocean it always was), no migration.
        PROPERTY( DisplayName( "Body Type" ), Category( "Water" ), Summary,
                  Tooltip( "Ocean: the footprint Ocean Extents around the entity. River: a ribbon River Width wide "
                           "along River Spline, flowing from its first point to its last (UE AWaterBodyRiver); "
                           "where a river overlaps an ocean the river's surface is drawn." ) )
        Water::WaterBodyType Type = Water::WaterBodyType::Ocean;

        // x, y, z per point, flat: the reflection serializes a vector of scalars (a std::vector<glm::vec3> field
        // is not a container to it - VolumeComponent::HullPoints). Relative to the entity's location.
        PROPERTY( DisplayName( "River Spline" ), Category( "River" ), Advanced, Length,
                  Tooltip( "The river's centre line, x y z per point relative to the entity, cm; the water flows "
                           "from the first point to the last. Fewer than two points draw no river." ) )
        std::vector<float> RiverSpline;

        PROPERTY( DisplayName( "River Width" ), Category( "River" ), Length, Range( 1.0f, 1000000.0f ),
                  Tooltip( "The river's width across its spline, cm (UE the spline's RiverWidth)." ) )
        float RiverWidth = 800.0f;

        PROPERTY( DisplayName( "River Flow Speed" ), Category( "River" ), Range( 0.0f, 10000.0f ),
                  Tooltip( "The surface speed on the centre line, cm/s (UE WaterVelocity); it eases to still "
                           "water at the banks." ) )
        float RiverFlowSpeed = 150.0f;

        PROPERTY( DisplayName( "River Flow Period" ), Category( "River" ), Range( 0.05f, 60.0f ),
                  Tooltip( "The two-phase flow's period, s: how long one ripple texture is advected before it "
                           "cross-fades to the other phase. Shorter stretches less, longer pulses less." ) )
        float RiverFlowPeriod = 1.0f;
    };

    /// @p data's RiverSpline as points (a view of the flat floats; a trailing partial triple is ignored).
    [[nodiscard]] inline std::span<const glm::vec3> RiverPointsOf( const WaterBodyData& data ) noexcept
    {
        static_assert( sizeof( glm::vec3 ) == 3 * sizeof( float ) );
        return { reinterpret_cast<const glm::vec3*>( data.RiverSpline.data() ), data.RiverSpline.size() / 3 };
    }

    struct WaterBodyComponent
    {
        COMPONENT( Key( "WaterBody" ), Block( Data ), Run( SkyAndAtmosphere ) )
        WaterBodyData Data;
    };
} // namespace Desert::ECS
