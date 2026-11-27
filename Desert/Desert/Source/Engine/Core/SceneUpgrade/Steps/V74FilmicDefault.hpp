#pragma once

#include <Engine/Core/SceneUpgrade/SceneUpgradeSteps.hpp>

#include <cstddef>
#include <string>
#include <vector>

// One step of the scene upgrade (SceneUpgradeSteps.cpp holds the table that orders it).
namespace Desert::Core
{
    //  74 - FILMIC BY DEFAULT (POST-LOOK-LAND, UE parity). A volume's Tonemapper ACES (0) - the former default -
    //       is Filmic (2); Reinhard stays (MigrateFilmicDefaultV73ToV74). The record and every override.
    inline constexpr int kSceneVersionFilmicDefault = 74;

    // What MigrateFilmicDefaultV73ToV74 did to one file.
    struct FilmicDefaultReport
    {
        std::size_t              Restated = 0; // Tonemapper ACES re-stated as Filmic
        std::vector<std::string> Refused;      // a Settings block that is not an object
    };

    // PURE (no filesystem, no globals): edits the records in place.
    FilmicDefaultReport MigrateFilmicDefaultV73ToV74( std::vector<Assets::EntityData>& entities );

    // The table row's adapter: runs the step over the context and states what it did.
    [[nodiscard]] StepOutcome RunFilmicDefault( UpgradeContext& context );
} // namespace Desert::Core
