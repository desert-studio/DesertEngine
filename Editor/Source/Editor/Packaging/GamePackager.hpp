#pragma once

#include <cstddef>
#include <string>

namespace Desert::Editor
{
    // THE PLAYER BINARY'S NAME INSIDE A .app. macOS starts whatever CFBundleExecutable names, and that
    // has to be the launcher SCRIPT (only a process started by the script inherits the Vulkan
    // environment dyld reads at image load), so the real binary cannot also be called "Runtime". This
    // is a constant rather than a literal spelled three times — the packager copies it, the launcher
    // execs it and a test has to be able to find it — and the three drifting apart produces a bundle
    // macOS reports as "damaged", which is the least diagnosable failure in the whole packager.
    inline constexpr const char* kBundlePlayerBinary = "Runtime-bin";

    // The launcher script's name inside a .app, which is also what its Info.plist declares as
    // CFBundleExecutable. Two independent literals until now, in two different std::ostringstreams
    // twenty lines apart, and their disagreement is the same "damaged application" with no further
    // explanation — the relation above, in the other direction. Not `host.LauncherName`: that is
    // `run.sh`, the name a person double-clicks in a plain folder, and a bundle's executable must be
    // extensionless.
    inline constexpr const char* kBundleLauncherName = "Runtime";

    // THE SUFFIX OF A RELEASE'S OWN RECORD, appended to the package's name: `MyGame` -> `MyGame.manifest`
    // beside `MyGame/` (or `MyGame.app/`) in the output directory. One symbol rather than a literal for
    // the reason П5 gave the descriptor one: the packager writes this file and something else entirely
    // has to find it later — a release script, a test, whoever hands it to `PakTool patch` — and a
    // release that cannot find its own baseline cannot be patched at all.
    inline constexpr const char* kContentManifestExtension = ".manifest";

    // EVERY FIELD HERE IS READ BY PackageGame, AND THAT IS CHECKED — Desert/Tests/Editor/
    // BuildSettingsConsumers asserts the relation in both directions: no option the Build Settings panel
    // offers that the packager ignores, and no option the packager honours that nothing can set. It was
    // written because the reverse held: the panel drew a "Target platform" chooser, `PackageOptions` had
    // no platform in it, and a Windows selection on a macOS host produced a macOS package in silence.
    //
    // There is deliberately NO target-platform field. The target is this editor's own host and cannot be
    // anything else (Editor/Packaging/PackageTarget.hpp explains why and derives the four things that
    // follow from it); a field whose only reachable value is the host would be the dead setting П6
    // removed, in a new place.
    // THE CONFIGURATIONS A PACKAGE CAN BE CUT FROM, in the order the panel offers them, and the reason
    // this is a table rather than two literals is the one PackageTarget.hpp gives for its own: the panel
    // drew `{ "Debug", "Release" }` and the packager's default said "Release", two spellings of one fact
    // in two files. `Shipping` arriving as a third build configuration is exactly the event that would
    // have made them disagree — the panel would have kept offering two.
    //
    // SHIPPING IS FIRST BECAUSE IT IS THE DEFAULT, and the default matters more here than anywhere else
    // in the editor: this is the one button whose product a stranger runs. Debug and Release are still
    // offered, because packaging a development build to reproduce something on another machine is a real
    // need — but they are now a CHOICE to make rather than the only thing on the menu.
    inline constexpr const char* kPackageConfigs[] = { "Shipping", "Release", "Debug" };

    struct PackageOptions
    {
        std::string OutputDir = "Build/Output"; // relative to the editor cwd, or absolute
        // Which Runtime binary to bundle; one of kPackageConfigs above. Shipping by default: the
        // development instruments — the frame capture, the profiler, the counters — are compiled out of
        // that one and out of no other (BuildScripts/Configurations.lua).
        std::string Config = "Shipping";
        // macOS: produce <Name>.app (launcher + Info.plist + MoltenVK/loader inside Contents/Frameworks
        // so the player machine needs no Homebrew). false -> plain folder + launcher. On a host with no
        // .app concept this is refused with a LOG_WARN and the plain layout is produced instead.
        bool MacAppBundle = true;
    };

    // ── WHAT PACKAGING ANSWERS, AND WHY IT IS THREE ANSWERS AND NOT TWO (I12) ────────────────────────
    //
    // THE DEFECT. This carried ONE bit for THREE outcomes. A cook failure — a `.ttf` that will not bake,
    // a shader that will not compile — was counted by CookStats, logged, and then thrown away: the
    // result came back `Success == true`, and the Build Settings panel painted it the same green as a
    // clean package. Measured on a fixture whose font and icon are both unbakeable: the log said
    // "2 failure(s)" and the caller could not tell that from a package with nothing wrong with it. The
    // one non-clean case that DID reach the caller, an unwritten artifact, reached it as ENGLISH inside
    // `Message`, so the only way to ask the question was to substring-match prose. That is Ф4's shape at
    // the last step before the game reaches a player: everything that did not make it is discovered by
    // whoever RUNS the game, not by whoever built it.
    //
    // WHY `Success` IS NOT REDEFINED TO MEAN "everything cooked". Because a project may legitimately ship
    // content that is already broken — an unbakeable font, a mesh that will not import, a shader a
    // material still names — and the runtime reports that content for itself. The packager's job is to
    // say what it shipped, not to decide the project is invalid; a packager that refused over a compile
    // failure would be unable to package such a project at all, and the person who has to fix the asset
    // would lose the build they were about to test it in.
    //
    // (This paragraph used to rest its case on ONE example, `Resources/Shaders/Programs/Graph/
    // MatBroken.shader`, which this repository shipped on purpose. Г20 moved that fixture into
    // Desert/Tests/Engine/ShaderCacheKey/Fixtures — it was compiled at every editor start and printed
    // two errors into every clean log, which is a cost the argument never needed. The argument is about
    // what a PROJECT is allowed to contain, not about what this one happens to contain, and the tests
    // below still exercise it with a corrupt `.ttf`.)
    //
    // SO THE ANSWER IS STRUCTURED INSTEAD. `Success` keeps its one meaning — a package exists — and what
    // the cook could not put into it comes back as NUMBERS a caller can branch on, with `Complete()` as
    // the named third state. "Packaged" and "packaged, N artifacts will be rebuilt on the player's
    // machine at every start" are now two different values rather than one value and a sentence.
    //
    // THE TWO COUNTS STAY SEPARATE because they are separate facts, and CookStats is careful about the
    // difference for a reason: content that could not be READ is already-broken content a project may
    // choose to ship, while an artifact that was produced and could not be WRITTEN is never normal — it
    // is a hole in the shipped cache that only a player's slow startup would ever reveal. Folding them
    // into one number would throw that away.
    struct PackageResult
    {
        bool        Success = false;
        std::string Message;    // human-readable summary / error
        std::string PackageDir; // the produced game folder (valid on success)

        // Appended AFTER the three fields above rather than beside `Success`, so the fifteen
        // `return { false, "...", "" }` refusals in GamePackager.cpp keep meaning what they say.
        size_t CookFailures  = 0; // content the cook could not read/parse/compile (see CookStats)
        size_t CookUnwritten = 0; // artifacts produced that did not reach the disk

        // WHERE THE PATCH BASELINE WAS WRITTEN (П7), or empty when this result did not produce one —
        // which is every BuildContentPak, because a dev archive is not a release and a baseline for a
        // version nobody shipped is a file nobody can ever patch against.
        //
        // It is a FIELD rather than a sentence inside `Message` because of what the file is: the one
        // artifact of a release that has to outlive the release, and the only one whose absence cannot
        // be repaired later. Whoever built the game has to be able to find it, and a path buried in
        // prose is a path a build script cannot pick up.
        // The `= {}` is load-bearing, not decoration: without a default member initializer every one of
        // the fifteen `return { false, "...", "" }` refusals in GamePackager.cpp becomes a
        // -Wmissing-field-initializers warning, and a field added at the end of this struct must cost
        // the refusals nothing (which is the reason the counts above were appended here too).
        std::string ManifestPath = {};

        // A package exists AND everything the cook was asked to produce is in it. This is the question
        // "did the build go green", and it is the one a caller should ask — `Success` alone answers a
        // narrower question than anybody looking at a build result means.
        bool Complete() const
        {
            return Success && CookFailures == 0 && CookUnwritten == 0;
        }
    };

    // Bakes the CURRENTLY OPEN project into a self-contained game FOR THIS EDITOR'S OWN HOST (.app
    // bundle by default on macOS, plain folder otherwise). THE PRODUCT IS A BINARY AND ONE ARCHIVE,
    // in one directory, and it starts with no arguments (П5):
    //
    //   the player binary   — the Runtime for options.Config
    //   Content.dpak        — ALL content in one archive, tree by tree out of the shared census in
    //                         PackagedContentTrees.hpp: project assets (raw mesh sources stripped —
    //                         the runtime reads cooked meshes only), the cooked cache, and the engine
    //                         shaders, fonts and icons — PLUS the regenerated descriptor at the
    //                         archive root under Project::kPackagedDescriptorName. The Runtime mounts
    //                         the archive found beside its own executable, opens that descriptor out
    //                         of it, and every content read resolves through the same mount.
    //   launcher            — run.sh / run.bat. It exists for the VULKAN ENVIRONMENT ONLY (Finder
    //                         hands a double-clicked app no VK_ICD_FILENAMES and no DYLD_*, and both
    //                         must be set before the image loads). It is not a second way to start
    //                         the game and it names no project: the binary run directly comes up too.
    //   Contents/Frameworks — (bundle only) MoltenVK + the Vulkan loader, so the player machine
    //                         needs no Homebrew; falls back to the target's Homebrew when the local
    //                         artifacts are absent.
    //
    // In a .app the binary and the archive live together in Contents/MacOS; Contents/Resources is not
    // produced. That split was what forced the launcher to pass `--project`, and it is gone with it.
    //
    // AND ONE FILE THAT IS NOT PART OF THE PACKAGE (П7): `<Name>.manifest`, written BESIDE the package
    // directory in options.OutputDir, never inside it. It is the record of what this release hands out
    // — the "before" side `PakTool patch` needs to build the next update — and it can only be taken
    // while this version exists: a release packaged without one can never be patched, and no later run
    // can reconstruct it. It stays outside because the product really is a binary and one archive: the
    // player needs nothing from it, and a publisher's record inside the folder a player copies around
    // is one more thing an installer can lose and one more thing that reads as content. The reader has
    // been in the tree since П3 — Runtime/Source/PackagedContent.cpp mounts every Patch*.dpak over the
    // base — and until now there was no writer anywhere on the path a game actually takes.
    //
    // Pure CPU + filesystem — safe to run on a JobSystem worker.
    PackageResult PackageGame( const PackageOptions& options );

    // Rebuilds ONLY the content archive (no Runtime copy, no bundle) — written next to the project's
    // own .deproj so the standalone Runtime can mount it for the CURRENT dev project. Same census AND
    // same embedded descriptor as PackageGame, so the two entry points produce the same KIND of
    // archive. Loose files still override pak entries (disk-first VFS), so a stale archive can never
    // shadow fresh edits in dev.
    PackageResult BuildContentPak();
} // namespace Desert::Editor
