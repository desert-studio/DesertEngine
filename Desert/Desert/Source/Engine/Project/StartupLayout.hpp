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
    // flag and no such branch. The two layouts put the executable in different places, and each
    // question below is answered by LOOKING at where this one is — a development build answers "no
    // project beside me" because `build/Bin/<Config>/` holds no `.deproj`, and a drop answers "not a
    // binary the launcher starts" because it is not at `<root>/build/Bin/<config>/`. One rule covers
    // both, so neither can drift, and neither can be fooled by a drop unzipped inside a checkout.

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

    // Derives the engine root from the executable's own position.
    //
    // WHAT COUNTS AS A ROOT. `engines.json` is written for exactly one reader, the launcher, and the
    // launcher does exactly two things with the root it reads:
    //
    //   * `<root>/Templates/<Id>/template.json` — the starter templates it creates projects from.
    //     The shared format states this as the definition of the field: "absolute path to the
    //     engine root (the folder holding Templates/)" (DesertShared/EngineRegistry.hpp).
    //   * it STARTS `<root>/build/Bin/<config>/Editor[.exe]` — directly on Windows, and on macOS
    //     through `<root>/scripts/MacOS/RunEditor.sh`, which runs that same file (desert-launcher
    //     Source/Launch.cpp, BuildEditorLaunch).
    //
    // So the derivation asks the launcher's own question: IS THIS EXECUTABLE THE ONE THAT ROOT WOULD
    // START? It is the exact relation `<root>/build/Bin/<config>/<this binary>` with `Templates/`
    // and `scripts/` beside `build/` — not a search upwards for an engine.
    //
    // THAT DISTINCTION WAS MEASURED, NOT ANTICIPATED. The first version walked ancestors for the two
    // markers, and the first end-to-end run of a real drop registered the wrong thing: the drop was
    // unzipped at `<worktree>/dist/DesertEngine-Release`, three directories under a checkout, so the
    // walk found the checkout and filed it. Every marker test passed and the answer was still wrong,
    // because "there is an engine above me" is not "this is an engine the launcher can start".
    //
    // WHICH IS ALSO WHY A DROP MUST NOT REGISTER ITSELF, and this function's "no root" answer is a
    // RESULT rather than a shortfall. The launcher PREFERS THE NEWEST ENTRY in `engines.json`, so a
    // drop that registered would displace the developer's real checkout and then fail to start —
    // worse than the message the owner complained about. The message is what changes; the refusal is
    // correct and stays.
    //
    // A checkout built somewhere other than `build/Bin` gets the same refusal, and that is what
    // DESERT_ROOT is still for.
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
    [[nodiscard]] Common::ResultStr<std::string> ProjectBesideExecutable( const std::filesystem::path& directory );

    // ── 3. THE ENGINE DIRECTORY (UE: FPaths::EngineDir) ────────────────────────────────────────────

    struct EngineDirLookup
    {
        // Absolute, lexically normal path of the directory holding the engine's `Resources/`, or empty
        // exactly when Explanation is not.
        std::filesystem::path Dir;
        // True when Dir is the Editor/ of the checkout this binary was BUILT in
        // (`<checkout>/build/Bin/<config>/`). Its development project then sits in Dir too.
        bool FromCheckout = false;
        // Non-empty when no engine directory was found: names every place looked at. The caller stops.
        std::string Explanation;
    };

    // Where the engine's resources are, answered from the executable's OWN POSITION — never from the
    // working directory. The process no longer changes directory to reach its resources: the answer is
    // handed to Common::Constants::Path::SetEngineDir, which makes every engine resource path absolute,
    // so an editor started from /tmp, from an IDE or from a double-click all read the same files.
    //
    // THE ORDER, and each step is final (a step that applies and fails is a refusal, not a fall-through):
    //   1. `--engine-dir <dir>` (overrideDir non-empty) — it must hold `Resources/Shaders`, else refused
    //      naming it. An explicit instruction that is wrong is not silently replaced by a guess.
    //   2. the executable's own directory holds `Resources/Shaders` — a packaged drop.
    //   3. the executable is at `<checkout>/build/Bin/<config>/` and `<checkout>/Editor/Resources/Shaders`
    //      exists — a development build, whoever started it and from wherever.
    //
    // `Resources/Shaders` rather than `Resources` is the marker on purpose: an empty `Resources`
    // directory would satisfy the weaker test and then fail 43 shaders later, with a message about
    // a shader rather than about a layout.
    [[nodiscard]] EngineDirLookup ResolveEngineDir( const std::filesystem::path& executableDirectory,
                                                    const std::filesystem::path& overrideDir );
} // namespace Desert::Project
