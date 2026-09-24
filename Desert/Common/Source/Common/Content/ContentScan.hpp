#pragma once

#include <Common/Content/AssetEnvelope.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Common::Content
{
    // WALKING THE CONTENT ROOTS, ONCE — and comparing what is there with what the cooked registry says
    // is there.
    //
    // ── WHY IT IS ONE FUNCTION AND NOT THREE ──────────────────────────────────────────────────────
    //
    // Since GAP_ANALYSIS T2.4 neither host walks the content roots at boot; the registry is the list.
    // That makes "does the registry still describe the tree" a question four different programs have to
    // ask, and the first draft of this slice answered it three separate times — in `AssetRegistryTool`,
    // in `ContentRegistry::Refresh`, and in the `CookedRegistryGate` suite. Three copies of one loop
    // over one census is exactly the shape the whole task is about ending: the packager's hand-typed
    // tree list forgot fonts and icons, and a built game shipped with no `.ttf` in it.
    //
    // So the walk lives here, below the engine, beside the census it reads. The four callers are the
    // cook (which enters what it finds), the tool (which writes and checks), the CI gate (which fails
    // the build), and the packager (which refuses to ship against a stale registry).
    //
    // ── WHAT IT DELIBERATELY DOES NOT KNOW ────────────────────────────────────────────────────────
    //
    // A file's DECLARED IDENTITY (a `.tex`'s own `Handle`, a `.demat`'s `MaterialId`) and its
    // DEPENDENCY EDGES. Both require parsing the asset with the class that owns its format, which is
    // the engine; `Common` does not link it, and three of the four callers do not either. A caller
    // that cannot compute a value must not judge it, so the comparison below asserts exactly the two
    // properties a walk CAN settle: the set of files, and each file's size.

    struct ContentFile
    {
        ContentKind Kind = ContentKind::StaticMesh;
        uint64_t    Size = 0;
        // What the file's own header states, read WITHOUT the body (ReadAssetHeaderIfStated, RecordOnly):
        // std::nullopt for content that states none. HeaderError is set instead when a header format
        // claims the file and the header is malformed — never folded into "states none".
        std::optional<AssetHeader> Header;
        std::string                HeaderError;
    };

    // The file at `file`, of `kind`: its size and its header, read the way the registry cook reads it.
    [[nodiscard]] ContentFile DescribeContentFile( const std::filesystem::path& file, ContentKind kind );

    // Every content file under every census root, keyed by the STABLE KEY the engine identifies it by,
    // sorted (std::map) so two scans of one tree produce one order.
    //
    // THE WORKING DIRECTORY MATTERS AND THAT IS NOT THIS FUNCTION'S TO FIX. Engine resource roots —
    // `Resources/Shaders/` — are never remapped by a project (Constants.hpp says so beside them), so
    // they resolve against the process's working directory, and both hosts `cd` into the directory
    // that holds them before starting. A caller standing somewhere else finds no shaders. Callers that
    // can be run from anywhere check `SHADERDIR_PATH` themselves and refuse; see AssetRegistryTool.
    // Which content kind a path is, or std::nullopt for a file no scan enumerates. Tests BOTH the
    // extension and the root, because the cloud roots nest — the two lists below would otherwise
    // disagree about a `.dcnv` sitting in `Clouds/Types`.
    [[nodiscard]] std::optional<ContentKind> KindOfContentFile( const std::filesystem::path& file );

    [[nodiscard]] std::map<std::string, ContentFile> ScanContentRoots();

    // ── AND THE OTHER LIST, WHICH IS NOT THE SAME LIST ────────────────────────────────────────────
    //
    // THE DEFECT THIS EXISTS FOR, MEASURED ON `dev`. The cooked asset registry is a COMMITTED file, and
    // the first version of the gate held it against the WORKING DIRECTORY. Those are different sets and
    // the difference is not small: on the integrator's machine ten rows disagreed, and exactly ONE of
    // them was a file git tracks. The other nine were his own cook's output under `Editor/Cooked/`,
    // which `.gitignore` excludes by design — so the gate was red for content no other clone has, and
    // "just re-cook it" would have written HIS machine's state into the shared file and turned the gate
    // red for everybody else. Measured again from a fresh checkout of `dev`: the committed registry
    // carried ten rows for files a clean clone does not contain, all ten mine.
    //
    // The rule that fell out of it is worth stating plainly, because this was the SECOND instance of
    // the shape in one day (a suite that depended on two fixtures nobody had committed was the first):
    //
    //     AN INSTRUMENT MUST READ THE DISK ONLY WHEN THE QUESTION IS ABOUT THIS MACHINE.
    //
    // A committed artifact is a claim about the REPOSITORY, so it is checked against the repository.
    // `ScanContentRoots` above answers the machine question and keeps its callers: the cook enters what
    // it finds, and the packager refuses against what it is about to pack — both of which are correctly
    // about a disk. This answers the repository question, and the gate is its only caller.
    //
    // std::nullopt means THE QUESTION COULD NOT BE ASKED — no `git`, or not a checkout — which is a
    // different thing from "nothing is tracked" and must never be reported as an empty answer. That
    // distinction is the same one `scripts/CI/CheckTidy.sh` spends its exit code 2 on.
    [[nodiscard]] std::optional<std::map<std::string, ContentFile>>
    TrackedContent( const std::filesystem::path& repoRoot );

    // One disagreement between the registry and the tree, as a sentence a person can act on.
    struct RegistryDisagreement
    {
        enum class Kind
        {
            MissingRow, ///< on disk, no row — it does not reach the engine or a packaged build
            OrphanRow,  ///< a row, no file — the loader will fail to read it once per boot, for ever
            WrongKind,  ///< recorded as one kind, is another — the loader builds the wrong class
            StaleSize,  ///< the file was edited after the cook
            BadHeader,  ///< the file's header is malformed, or states another kind than its place does
            StaleHeader ///< the row's GUID/versions are not the ones the file's header states
        };

        Kind        What = Kind::MissingRow;
        std::string Key;
        std::string Detail; ///< the whole sentence, including what to run to fix it
    };

    // What `registry` and `present` disagree about. EMPTY MEANS CURRENT — that is the whole interface,
    // and it is why this returns a list rather than a bool: a gate that can only say "no" teaches
    // people to re-run it until it says "yes", while a gate that names the file is one somebody fixes.
    //
    // `sourceName` NAMES THE LIST IN EVERY SENTENCE, and it is a parameter rather than the word "disk"
    // because the two callers ask about two different worlds and a message that says "on disk" to
    // somebody comparing against a repository is the instrument lying about its own question. It reads
    // in place of a noun: "... and no such file is <sourceName>".
    [[nodiscard]] std::vector<RegistryDisagreement> Compare( const Utils::AssetRegistry&               registry,
                                                             const std::map<std::string, ContentFile>& present,
                                                             std::string_view sourceName );
} // namespace Common::Content
