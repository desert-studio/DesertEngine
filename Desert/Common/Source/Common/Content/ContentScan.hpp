#pragma once

#include <Common/Content/ContentKinds.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <cstdint>
#include <map>
#include <string>
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
    };

    // Every content file under every census root, keyed by the STABLE KEY the engine identifies it by,
    // sorted (std::map) so two scans of one tree produce one order.
    //
    // THE WORKING DIRECTORY MATTERS AND THAT IS NOT THIS FUNCTION'S TO FIX. Engine resource roots —
    // `Resources/Shaders/` — are never remapped by a project (Constants.hpp says so beside them), so
    // they resolve against the process's working directory, and both hosts `cd` into the directory
    // that holds them before starting. A caller standing somewhere else finds no shaders. Callers that
    // can be run from anywhere check `SHADERDIR_PATH` themselves and refuse; see AssetRegistryTool.
    [[nodiscard]] std::map<std::string, ContentFile> ScanContentRoots();

    // One disagreement between the registry and the tree, as a sentence a person can act on.
    struct RegistryDisagreement
    {
        enum class Kind
        {
            MissingRow, ///< on disk, no row — it does not reach the engine or a packaged build
            OrphanRow,  ///< a row, no file — the loader will fail to read it once per boot, for ever
            WrongKind,  ///< recorded as one kind, is another — the loader builds the wrong class
            StaleSize,  ///< the file was edited after the cook
        };

        Kind        What = Kind::MissingRow;
        std::string Key;
        std::string Detail; ///< the whole sentence, including what to run to fix it
    };

    // What `registry` and `onDisk` disagree about. EMPTY MEANS CURRENT — that is the whole interface,
    // and it is why this returns a list rather than a bool: a gate that can only say "no" teaches
    // people to re-run it until it says "yes", while a gate that names the file is one somebody fixes.
    [[nodiscard]] std::vector<RegistryDisagreement>
    CompareWithDisk( const Utils::AssetRegistry& registry, const std::map<std::string, ContentFile>& onDisk );
} // namespace Common::Content
