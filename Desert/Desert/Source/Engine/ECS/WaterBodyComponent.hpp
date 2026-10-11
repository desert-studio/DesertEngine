#pragma once

// A WATER BODY (UE: AWaterBodyOcean / UWaterBodyOceanComponent). The entity's world position is the ocean's
// plane height and its WaveOrigin; its `.dwaves` wave set gives the shared Gerstner waves. Physics gathers
// every body each step into Water::WaterSubsystem (ECS/System/WaterBodyGather.hpp), which answers the
// water queries buoyancy, swimming and scripts ask (UE QueryWaterInfoClosestToWorldLocation).

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>

#include <glm/vec2.hpp>

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
    };

    struct WaterBodyComponent
    {
        COMPONENT( Key( "WaterBody" ), Block( Data ), Run( SkyAndAtmosphere ) )
        WaterBodyData Data;
    };
} // namespace Desert::ECS
