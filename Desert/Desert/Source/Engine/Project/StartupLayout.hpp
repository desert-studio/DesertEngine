#pragma once

#include <Common/Core/ResultStr.hpp>

#include <filesystem>
#include <string>

namespace Desert::Project
{
    // ── WHAT A PROGRAM ALREADY KNOWS, IT MUST NOT ASK FOR ────────────────────────────────────────
    //
    // THE DEFECT, reported from a downloaded CI artifact (DesertEngine-Windows-Release, unzipped,
    // Editor.exe double-clicked). The drop is COMPLETE — the binaries, `Resources/Shaders|Fonts|
    // Icons` and `Desert.deproj` all sit in one directory (scripts/MacOS/Package.sh) — and the
    // editor still printed two demands:
    //
    //     [Engine] DESERT_ROOT is not set ... start the Editor through scripts/Windows/RunEditor.bat
    //     No project given. Pass: --project <path/to/.deproj>
    //
    // Both asked for something derivable from the one fact every process has for free: where its own
    // executable is. And the first demand named a script that is NOT IN THE DROP at all — it lives
    // in the repository — so following it was impossible.
    //
    // These three functions are the derivations, and they are pure functions of paths rather than
    // code inside `main` for the usual reason: a decision taken in an entry point is a decision that
    // can only be checked by launching the program and reading what came out. Desert/Tests/Engine/
    // StartupLayout drives all three over temporary directories shaped like a dev tree and like a
    // drop, on both platforms.
    //
    // WHAT IS DELIBERATELY NOT HERE: any notion of "am I a drop or a dev tree?". There is no such
    // flag and no such branch. The two layouts differ in what is beside the executable and in what
    // is above it, and each question below is answered by LOOKING — a dev tree answers "no project
    // beside me" because `build/Bin/<Config>/` holds no `.deproj`, and a drop answers "no engine
    // tree above me" because it carries no repository. One rule covers both, so neither can drift.

    // ── 1. WHERE THIS ENGINE IS ──────────────────────────────────────────────────────────────────

    struct EngineRootLookup
    {
        // Absolute path of the engine tree this executable belongs to, or "" when it belongs to
        // none (a drop, an installed copy, a binary moved somewhere by hand).
        std::string Root;
        // Non-empty EXACTLY when `Root` is empty: what was looked for, where, and what the
        // consequence is. It is a value rather than a log line because the caller is the process
        // entry point, which has no logger yet and a terminal that somebody is reading.
        std::string Explanation;
    };

    // Derives the engine root from the executable's own position by walking UP from it.
    //
    // WHAT COUNTS AS A ROOT, AND WHY IT IS THESE TWO NAMES. `engines.json` is written for exactly
    // one reader, the launcher, and the launcher does exactly two things with the root it reads:
    //
    //   * `<root>/Templates/<Id>/template.json` — the starter templates it creates projects from.
    //     The shared format states this as the definition of the field: "absolute path to the
    //     engine root (the folder holding Templates/)" (DesertShared/EngineRegistry.hpp).
    //   * `<root>/scripts/MacOS/RunEditor.sh`, or `<root>/build/Bin/<config>/Editor.exe` with the
    //     working directory `<root>/Editor` — how it STARTS that engine (desert-launcher
    //     Source/Launch.cpp BuildEditorLaunch).
    //
    // So a root is a REPOSITORY CHECKOUT, and both markers are checked rather than one: a directory
    // that happens to hold a `Templates` folder is not an engine, and registering one would put an
    // entry in the launcher's sidebar that it cannot start.
    //
    // WHICH IS ALSO WHY A DROP MUST NOT REGISTER ITSELF, and this function's "no root" answer is a
    // RESULT rather than a shortfall. The drop has no `Templates/` and no `scripts/`; the launcher
    // prefers the newest entry in `engines.json`, so a drop that registered itself would DISPLACE
    // the developer's real checkout and then fail to start, which is a worse outcome than the
    // message the owner complained about. The message is what changes; the refusal to register is
    // correct and stays.
    [[nodiscard]] EngineRootLookup DeriveEngineRoot( const std::filesystem::path& executable );

    // ── 2. WHICH PROJECT TO OPEN WHEN NOBODY SAID ────────────────────────────────────────────────

    // The `.deproj` beside the executable, for a run that was given no `--project`.
    //
    // THREE OUTCOMES, AND THE TWO THAT ARE NOT "one file" ARE REFUSALS THAT NAME WHAT THEY SAW.
    // Exactly one descriptor is an answer. None is the ordinary state of a development build, whose
    // executable sits in `build/Bin/<Config>/` — the run scripts pass `--project` and never reach
    // here. Several is a directory nobody has told apart, and picking the alphabetically first one
    // would be the silent wrong answer this codebase spends its days removing: the editor would open
    // A, the person would be looking at B, and nothing would say so.
    //
    // The search is the executable's own directory and nothing else — not the working directory, not
    // an ancestor. A second place to look is a second rule, and two rules disagree eventually.
    [[nodiscard]] Common::ResultStr<std::string>
    ProjectBesideExecutable( const std::filesystem::path& directory );

    // ── 3. WHERE `Resources/` IS ─────────────────────────────────────────────────────────────────

    struct ResourceRootLookup
    {
        // A directory to change to before anything reads content, or "" to leave the working
        // directory exactly as it is.
        std::string WorkingDirectory;
        // Non-empty when NEITHER candidate holds the engine resources: names the tree and both
        // places that were looked at. The caller stops on this.
        std::string Explanation;
    };

    // The engine resolves every engine resource as a path RELATIVE TO THE WORKING DIRECTORY —
    // `Resources/Shaders/`, `Resources/Fonts/`, `Resources/Icons/` are literals in
    // Common/Core/Constants.hpp and are never remapped by opening a project. So "where are the
    // resources" is answered by naming a directory to work FROM.
    //
    // THE WORKING DIRECTORY WINS WHEN IT HAS THEM, and that ordering is what keeps every existing
    // launch byte-identical: `scripts/*/RunEditor.*` change into `Editor/`, which holds
    // `Resources/Shaders`, so this returns "" and nothing moves. The executable's own directory is
    // the FALLBACK, and it is only ever taken in a situation that used to be a failure — a working
    // directory with no engine resources under it could not render a frame. Nothing that worked
    // before changes; something that could not start now can.
    //
    // `Resources/Shaders` rather than `Resources` is the marker on purpose: an empty `Resources`
    // directory would satisfy the weaker test and then fail 43 shaders later, with a message about
    // a shader rather than about a layout.
    [[nodiscard]] ResourceRootLookup ResolveResourceRoot( const std::filesystem::path& workingDirectory,
                                                          const std::filesystem::path& executableDirectory );
} // namespace Desert::Project
