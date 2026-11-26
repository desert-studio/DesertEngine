#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  75 - THE GRASS RASTER PATH IS THE RENDERER'S (FPS-GRASS-VISBUF). The volume's bool
    //       PostProcessSettings.GrassVisibilityBuffer becomes GrassRaster, a Core::GrassRasterPath (Scalability 0,
    //       GBuffer 1, VisibilityBuffer 2): false -> Scalability, true -> VisibilityBuffer; a value that is not a
    //       bool is refused, naming the entity (MigrateGrassRasterPathV74ToV75). The record and every override.
    inline constexpr int kSceneVersionGrassRasterPath = 75;

    // What MigrateGrassRasterPathV74ToV75 did to one file.
    struct GrassRasterPathReport
    {
        std::size_t              Restated = 0; // GrassVisibilityBuffer bools re-stated as GrassRaster
        std::vector<std::string> Refused;      // a Settings block that is not an object, or a non-bool value
    };

    // PURE (no filesystem, no globals): edits the records in place.
    GrassRasterPathReport MigrateGrassRasterPathV74ToV75( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunGrassRasterPath( UpgradeContext& context );
} // namespace Desert::Core
