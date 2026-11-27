#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    // THE POST-PROCESS SETTINGS' OWN CHAIN (Assets::kPostProcessSchemaTag), run after the light chain.
    //   1 - BLOOM AND AUTO EXPOSURE AS UE (RENDER-BLOOM-UE). A PostProcessVolume's Settings lose EnableBloom
    //       (false -> BloomIntensity 0, true keeps the intensity, or states the old 0.8 where none is stated) and
    //       AutoExposureSpeed (-> AutoExposureSpeedUp = AutoExposureSpeedDown); BloomThreshold <= 0 -> -1. A
    //       record (one without Settings too) also states what the v0 defaults gave it (bloom off, threshold 2,
    //       speed 1.5, auto exposure off) and the 0 / 95 histogram window it was graded under; an override is a
    //       delta and re-states only its own keys. A Settings that is not an object, or a key of the wrong type,
    //       is refused (MigratePostProcessBloomV0ToV1). This step is the PostProcessVolume block's upgrade from
    //       COMPONENT Version 1 to Version( 2 ) (ECS/PostProcessVolumeComponent.hpp). Scenes and prefabs alike.
    inline constexpr int kPostProcessVersionBloom = 1;

    // What MigratePostProcessBloomV0ToV1 did to one file.
    struct PostProcessBloomReport
    {
        std::size_t              Restated = 0; // Settings blocks re-stated
        std::size_t              BloomOff = 0; // EnableBloom false re-stated as BloomIntensity 0
        std::vector<std::string> Refused;      // a Settings block that is not an object, or a mistyped key
    };

    // PURE (no filesystem, no globals): edits the records in place.
    PostProcessBloomReport MigratePostProcessBloomV0ToV1( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunPostProcessBloom( UpgradeContext& context );
} // namespace Desert::Core
