#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  73 - GI COMES FROM THE METHOD ALONE (GTX-TIER-LADDER). A volume that stated GIRequest ScreenSpace (2) asks
    //       for Off (0) - the GI it rendered in effect - and GIIntensity, read by nothing now, is dropped
    //       (MigrateComposedGIV72ToV73). The record and every prefab override.
    inline constexpr int kSceneVersionComposedGI = 73;

    // What MigrateComposedGIV72ToV73 did to one file.
    struct ComposedGIReport
    {
        std::size_t              Restated = 0; // GlobalIlluminationMethod ScreenSpace re-stated as Off
        std::size_t              Dropped  = 0; // GIIntensity keys removed
        std::vector<std::string> Refused;      // a Settings block that is not an object
    };

    // PURE (no filesystem, no globals): edits the records in place.
    ComposedGIReport MigrateComposedGIV72ToV73( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunComposedGI( UpgradeContext& context );
} // namespace Desert::Core
