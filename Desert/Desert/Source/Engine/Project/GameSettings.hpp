#pragma once

#include <string>
#include <vector>

namespace Desert::Project
{
    // The game's own settings — UE's GeneralProjectSettings, kept in `<project>/Config/Game.json` beside the
    // .deproj. The .deproj is the launcher's descriptor (desert-shared) and already carries the game's NAME
    // (ProjectFile::Name, one field); what only the game itself needs lives here, owned by the engine.
    struct GameSettings
    {
        std::string Company; // the studio, as the credits and the menu show it

        // UE Project Settings ▸ Movies. The movies the game plays full screen at launch, in this order, over
        // its loading and before its first level is shown (Engine/Media/StartupMoviePlayer.hpp). Paths are
        // relative to the project directory (`Content/Movies/Intro.webm`).
        std::vector<std::string> StartupMovies;
        bool                     MoviesAreSkippable      = true; // a key or a click ends the current movie
        bool                     WaitForMoviesToComplete = true; // false: they end as soon as the game has loaded
    };

    // The open project's Config/Game.json, read once per project directory. A project that has no such file
    // has no game settings and gets an empty struct — NOT a fallback value: nothing is invented for it, and a
    // file that exists but does not parse is logged with the parser's own words.
    const GameSettings& CurrentGameSettings();
} // namespace Desert::Project
