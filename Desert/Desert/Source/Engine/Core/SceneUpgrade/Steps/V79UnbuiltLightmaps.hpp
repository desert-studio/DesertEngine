#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  79 - A VOLUME ASKS FOR THE GI IT RENDERED (MIG-BISTRO). GIRequest Lightmaps (1) has been declared and never
    //       implemented since the method ladder was born (v52, RMODE-IFACE: "Lightmaps - not implemented"): the
    //       resolution's walk (Graphic::ResolveRenderMethods, WalkDown, MethodLimit::NotImplemented) stepped it
    //       down to Off (0) on every build, so every file below v79 that states 1 rendered with GI Off. The
    //       loader now refuses a request this build does not admit, so the number is re-stated as Off - the
    //       picture those files drew. A Settings block that is not an object is refused naming the entity
    //       (MigrateUnbuiltLightmapsV78ToV79). The record and every prefab override.
    inline constexpr int kSceneVersionUnbuiltLightmaps = 79;

    // What MigrateUnbuiltLightmapsV78ToV79 did to one file.
    struct UnbuiltLightmapsReport
    {
        std::size_t              Restated = 0; // GlobalIlluminationMethod Lightmaps re-stated as Off
        std::vector<std::string> Refused;      // a Settings block that is not an object
    };

    // PURE (no filesystem, no globals): edits the records in place.
    UnbuiltLightmapsReport MigrateUnbuiltLightmapsV78ToV79( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunUnbuiltLightmaps( UpgradeContext& context );
} // namespace Desert::Core
