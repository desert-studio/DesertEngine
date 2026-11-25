#pragma once

// A WATER BODY (UE: AWaterBodyOcean / UWaterBodyOceanComponent). The entity's world position is the ocean's
// plane height and its WaveOrigin; its `.dwaves` wave set gives the shared Gerstner waves. Physics gathers
// every body each step into Water::WaterSubsystem (ECS/System/WaterBodyGather.hpp), which answers the
// water queries buoyancy, swimming and scripts ask (UE QueryWaterInfoClosestToWorldLocation).

#include <Engine/Assets/Common.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

#include <Water/Public/WaterRiver.hpp>

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

        PROPERTY( DisplayName( "Ocean Extents" ), Category( "Water" ), Length, EditCondition( "Type == Ocean" ),
                  Tooltip( "The footprint of the ocean around the entity, X by Z (UE OceanExtents). A point "
                           "outside it is not in this body." ) )
        glm::vec2 OceanExtents{ 51200.0f, 51200.0f };

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
                  EditCondition( "Type == River" ),
                  Tooltip( "The river's centre line, x y z per point relative to the entity, cm; the water flows "
                           "from the first point to the last. Fewer than two points draw no river." ) )
        std::vector<float> RiverSpline;

        PROPERTY( DisplayName( "River Width" ), Category( "River" ), Length, Range( 1.0f, 1000000.0f ),
                  EditCondition( "Type == River" ),
                  Tooltip( "The river's width across its spline, cm (UE the spline's RiverWidth)." ) )
        float RiverWidth = 800.0f;

        PROPERTY( DisplayName( "River Flow Speed" ), Category( "River" ), Range( 0.0f, 10000.0f ),
                  EditCondition( "Type == River" ),
                  Tooltip( "The surface speed on the centre line, cm/s (UE WaterVelocity); it eases to still "
                           "water at the banks." ) )
        float RiverFlowSpeed = 150.0f;

        PROPERTY( DisplayName( "River Flow Period" ), Category( "River" ), Range( 0.05f, 60.0f ),
                  EditCondition( "Type == River" ),
                  Tooltip( "The two-phase flow's period, s: how long one ripple texture is advected before it "
                           "cross-fades to the other phase. Shorter stretches less, longer pulses less." ) )
        float RiverFlowPeriod = 1.0f;

        // The effects at the foot of each of the river's falls, played by the code as transient entities
        // (Water/WaterFallEffects): as wide as the fall (User.Extent), as dense as the water lands
        // fast. Empty = none.
        PROPERTY( DisplayName( "Fall Foam" ), Category( "River" ), Asset<VFXSystemAsset>,
                  EditCondition( "Type == River" ), Tooltip( "The .dfx on the water where a fall lands" ) )
        Assets::AssetHandle FallFoam;
        PROPERTY( DisplayName( "Fall Spray" ), Category( "River" ), Asset<VFXSystemAsset>,
                  EditCondition( "Type == River" ), Tooltip( "The .dfx of the drops a fall throws up" ) )
        Assets::AssetHandle FallSpray;
        PROPERTY( DisplayName( "Fall Mist" ), Category( "River" ), Asset<VFXSystemAsset>,
                  EditCondition( "Type == River" ), Tooltip( "The .dfx of the mist off a fall" ) )
        Assets::AssetHandle FallMist;

        // WATER-BUOYANCY: the fluid this body is, read by every BuoyancyComponent's pontoons in it (the one
        // source of the water's density - a body floats by the water it is in, not by a value of its own).
        PROPERTY( DisplayName( "Density" ), Category( "Fluid" ), Range( 1.0f, 20000.0f ), Units( "kg/m3" ),
                  Tooltip( "The fluid's density, kg/m^3: a floating body is pushed up with the weight of the fluid "
                           "it displaces (fresh water 1000, sea water 1025, oil about 900). A body denser than "
                           "its fluid sinks." ) )
        float Density = 1000.0f;

        PROPERTY( DisplayName( "Drag" ), Category( "Fluid" ), Range( 0.0f, 100.0f ), Units( "1/s" ),
                  Tooltip( "How fast the fluid pulls a body in it to its own velocity, per second (a river's "
                           "current carries a floating body; still water damps its bobbing and spin). The force at "
                           "a pontoon is -Drag * displaced fluid mass * velocity relative to the fluid." ) )
        float Drag = 1.5f;
    };

    /// @p data's RiverSpline as points (a view of the flat floats; a trailing partial triple is ignored).
    [[nodiscard]] inline std::span<const glm::vec3> RiverPointsOf( const WaterBodyData& data ) noexcept
    {
        static_assert( sizeof( glm::vec3 ) == 3 * sizeof( float ) );
        return { reinterpret_cast<const glm::vec3*>( data.RiverSpline.data() ), data.RiverSpline.size() / 3 };
    }

    struct WaterBodyComponent
    {
        COMPONENT( Key( "WaterBody" ), Block( Data ), Run( SkyAndAtmosphere ), Version( 2 ) )
        WaterBodyData Data;
    };
} // namespace Desert::ECS
