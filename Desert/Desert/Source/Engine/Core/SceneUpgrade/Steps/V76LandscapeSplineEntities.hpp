#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  76 - ONE ENTITY PER LANDSCAPE SPLINE (WP-LANDSCAPE-SPLINES; UE 5.5 ALandscapeSplineActor). A Landscape
    //       block's Splines {ControlPoints, Segments} leaves the root: every connected group of its control points
    //       becomes one record {LandscapeSpline: {Landscape: the root's id, Splines: the group, re-indexed}}, Tag
    //       "Landscape Spline", its id FNV-1a over "LandscapeSpline:<root id>:<group>" so every upgrade of the
    //       same file states the same identity. A prefab (or override) whose Landscape states control points is
    //       refused naming it (MigrateLandscapeSplineEntitiesV75ToV76).
    inline constexpr int kSceneVersionLandscapeSplineEntities = 76;

    // What MigrateLandscapeSplineEntitiesV75ToV76 did to one file.
    struct LandscapeSplineEntitiesReport
    {
        std::size_t RootsCleared   = 0;   // Landscape blocks (and overrides) whose Splines key was taken out
        std::size_t SplinesCreated = 0;   // LandscapeSpline records added, one per connected group
        std::vector<std::string> Refused; // a Splines value of the wrong shape, or splines in a prefab
    };

    // PURE (no filesystem, no globals): edits the records in place.
    // @p createEntities: true for a scene, false for a prefab (whose control points are refused).
    LandscapeSplineEntitiesReport
    MigrateLandscapeSplineEntitiesV75ToV76( std::vector<Assets::EntityData>& entities, bool createEntities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunLandscapeSplineEntities( UpgradeContext& context );
} // namespace Desert::Core
