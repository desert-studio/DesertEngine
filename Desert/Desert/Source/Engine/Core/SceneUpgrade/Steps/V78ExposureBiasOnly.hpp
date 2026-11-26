#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  78 - ONE EXPOSURE OFFSET (BISTRO-NIGHT-LOOK; UE has only Exposure Compensation). PostProcessVolume
    //       Settings.AutoExposureKey (the middle-grey target, default 0.18) is dropped and AutoExposureBias gains
    //       log2(key / 0.18): the exposure scale key/L x 2^bias is 0.18/L x 2^(bias + log2(key/0.18)), the same
    //       picture (Graphic::kExposureMiddleGrey). A key that is not a positive number is refused naming the
    //       entity; so is a prefab override that states a key other than 0.18 without its bias (a delta whose
    //       inherited bias this file cannot read) (MigrateExposureKeyV77ToV78). The record and every override.
    inline constexpr int kSceneVersionExposureBiasOnly = 78;

    // What MigrateExposureKeyV77ToV78 did to one file.
    struct ExposureKeyReport
    {
        std::size_t              Folded = 0; // AutoExposureKey values folded into AutoExposureBias
        std::vector<std::string> Refused;    // a non-object Settings, a key that is not a positive number, or an
                                             // override stating a key other than 0.18 without its bias
    };

    // PURE (no filesystem, no globals): edits the records in place.
    ExposureKeyReport MigrateExposureKeyV77ToV78( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunExposureBiasOnly( UpgradeContext& context );
} // namespace Desert::Core
