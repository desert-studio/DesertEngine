#pragma once

#include <glm/glm.hpp>

#include <Engine/Core/PostProcessSettings.hpp>
#include <CoreReflection/ReflectionMacros.hpp>

namespace Desert::ECS
{
    // UE's APostProcessVolume: a grade that applies where the camera is. The pattern taken from UE
    // (Engine/Source/Runtime/Engine/Private/SceneView.cpp, OverridePostProcessSettings / the volume loop in
    // World.cpp's AddPostProcessingSettings), not its letter: volumes are applied lowest Priority first onto
    // the defaults, each with the weight BlendWeight x (1 inside, fading to 0 across BlendRadius outside);
    // interpolable values are lerped by that weight and the rest are taken whole from any volume that
    // contributes at all. Two differences, both stated: the bounds are an axis-aligned box of half-extent
    // Extent around the entity's Translation (UE uses the brush), and a volume overrides EVERY setting — UE's
    // per-field bOverride_ flags are not ported, so a bounded volume restates the whole grade it wants.
    struct PostProcessVolumeData
    {
        REFLECT()

        PROPERTY( DisplayName( "Infinite Extent (Unbound)" ), Category( "Post Process Volume" ),
                  Tooltip( "Applies everywhere, whatever the camera position — the level's base grade." ) )
        bool Unbound = true;

        PROPERTY( DisplayName( "Extent" ), Category( "Post Process Volume" ), Length,
                  Tooltip( "Half-size of the box, around the entity's position, inside which the volume "
                           "applies at full BlendWeight." ) )
        glm::vec3 Extent = glm::vec3( 500.0f );

        PROPERTY( DisplayName( "Blend Radius" ), Category( "Post Process Volume" ), Length,
                  Range( 0.0f, 100000.0f ),
                  Tooltip( "Distance outside the box across which the volume fades out." ) )
        float BlendRadius = 100.0f;

        PROPERTY( DisplayName( "Priority" ), Category( "Post Process Volume" ),
                  Tooltip( "Higher priority is applied later and therefore wins where volumes overlap." ) )
        float Priority = 0.0f;

        PROPERTY( DisplayName( "Blend Weight" ), Category( "Post Process Volume" ), Range( 0.0f, 1.0f ),
                  Tooltip( "0 = no effect, 1 = full effect." ) )
        float BlendWeight = 1.0f;

        PROPERTY( DisplayName( "Settings" ), Category( "Post Process Volume" ) )
        Core::PostProcessSettings Settings;
    };

    // Version 2 (RENDER-BLOOM-UE): Settings lost EnableBloom and AutoExposureSpeed. The upgrade step from 1 is
    // the post-process settings' v1 step, Core::MigratePostProcessBloomV0ToV1 (Core/Serialize/SceneUpgrade.hpp) -
    // the one copy of the mapping: EnableBloom false -> BloomIntensity 0, AutoExposureSpeed -> AutoExposureSpeedUp
    // and AutoExposureSpeedDown. Every file at post process v0 (Assets::kPostProcessSchemaTag) stores this block
    // at Version 1.
    struct PostProcessVolumeComponent
    {
        COMPONENT( Key( "PostProcessVolume" ), Block( Data ), Run( SkyAndAtmosphere ), Version( 2 ) )
        PostProcessVolumeData Data;
    };
} // namespace Desert::ECS
