#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  72 - A VIEW ASKS FOR "TRACED", NOT FOR A TRACER (TRACED-METHOD-AXIS). PostProcessSettings
    //       GlobalIlluminationMethod is a GIRequest (Off 0, Lightmaps 1, ScreenSpace 2, ProbeVolume 3, Traced 4,
    //       PathTraced 5) and ReflectionsMethod a ReflectionRequest (Off 0, Captures 1, ScreenSpace 2, Traced 3,
    //       PathTraced 4). A v64..v71 file stated the rendered ladders: GI SoftwareTraced 4 / HardwareTraced 5 /
    //       PathTraced 6, reflections SoftwareTraced 3 / HardwareTraced 4 / PathTraced 5. Both traced numbers
    //       become Traced and PathTraced moves down one; a value outside the old ladder is refused, naming the
    //       entity (MigrateTracedRequestV71ToV72). The record and every prefab override.
    inline constexpr int kSceneVersionTracedRequest = 72;

    // What MigrateTracedRequestV71ToV72 did to one file.
    struct TracedRequestReport
    {
        std::size_t              Restated = 0; // GlobalIlluminationMethod / ReflectionsMethod numbers changed
        std::vector<std::string> Refused;      // a Settings block or a value that is not on the old ladder
    };

    // PURE (no filesystem, no globals): edits the records in place.
    TracedRequestReport MigrateTracedRequestV71ToV72( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunTracedRequest( UpgradeContext& context );
} // namespace Desert::Core
