#pragma once

#include <Common/Content/ContentKinds.hpp>

#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;

    // THE PROCESS'S ONE COOKED ASSET REGISTRY — what replaced the boot's directory walk.
    //
    // ── WHAT CHANGED, AND WHAT IT COST BEFORE ─────────────────────────────────────────────────────
    //
    // Both hosts used to start by walking eight content roots with `ListFilesRecursive`, once per
    // content kind, and every handle the engine could resolve was minted by that walk. `AssetPathIndex`
    // made the result observable — `368 handle(s) can name their own path` — and said in its own header
    // that inverting the hash is the PRECONDITION for removing the walk, not the removal. This is the
    // removal: the walk happens once, in the editor, and its answer is a file.
    //
    // Boot now reads that file and publishes every row's identity into `AssetPathIndex` BEFORE anything
    // is created, so a handle read out of a `.desce` on a cold start names its file with nothing having
    // been walked, parsed or loaded. `[ContentScan] boot finished` is the detector that says the walk
    // really is gone rather than moved.
    //
    // ── THE ROWS ARE TOTAL BY CONSTRUCTION, AND THAT IS THE WHOLE DESIGN ──────────────────────────
    //
    // `NoteAsset` is called by `AssetManager::CreateAsset`, i.e. at the exact moment a file becomes an
    // asset — the same place and for the same reason `AssetHandle::FromCookedPath` records its own
    // inverse. Nothing becomes content in this engine without going through that function, so no kind
    // can be missing from the registry because somebody forgot to add it to a list. The repository has
    // paid twice for the other arrangement: the packager's hand-typed tree list that forgot fonts and
    // icons, and the twelve-branch `ToPath` that returns "" for a type nobody added a branch for.
    //
    // ── WHAT THE EDITOR DOES THAT THE RUNTIME DOES NOT ────────────────────────────────────────────
    //
    // The editor is the cook. `Refresh` walks the content roots, enters anything new, drops rows whose
    // file is gone, and fills in each row's dependency edges from the live manager. It runs AFTER the
    // boot is over — after `[ContentScan] boot finished` has been printed — deliberately, because
    // putting it before would put the walk back into the number this whole tier is judged by. The
    // runtime never calls it and never writes the file.
    //
    // It is the SAFETY NET and not the mechanism: content authored in the editor gets its row from
    // `NoteAsset` the moment `CreateAsset` sees it, and content the cook writes gets one from
    // `NoteFile` the moment `WriteCookedJson` writes it. What `Refresh` catches is a file that arrived
    // on disk with nobody looking — a `git pull`, a drop into the folder while the editor was closed —
    // which is available the next time the project opens rather than in the session that found it.
    //
    // ── WHAT HAPPENS WHEN THE FILE IS MISSING OR STALE ────────────────────────────────────────────
    //
    // `Load` REFUSES, loudly, naming the file and the command that rebuilds it. It does not fall back
    // to a scan: a fallback would restore the cost this exists to remove and would do it silently, on
    // exactly the machines where nobody is watching. A file that is on disk and not in the registry is
    // a file the engine does not have — which is the danger GAP_ANALYSIS T2.7 names, and it is answered
    // by a gate that FAILS the build (`Desert/Tests/Editor/CookedRegistryGate`), never by a scan.
    namespace ContentRegistry
    {
        // Reads `Common::Utils::AssetRegistry::DefaultPath()` and publishes every row's identity into
        // `Common::AssetPathIndex`. Returns how many bindings were made, so a host can log the number
        // beside the one it is meant to reproduce.
        //
        // AN ABSENT FILE IS A SUCCESS WITH ZERO ROWS, and only for a project that has never been
        // cooked — `Refresh` writes one at the end of the first editor session. A file that exists and
        // cannot be parsed is a REFUSAL: a truncated or hand-broken registry must not read as an empty
        // project, because an empty project starts and looks almost right.
        [[nodiscard]] Common::ResultStr<std::size_t> Load();

        [[nodiscard]] const Common::Utils::AssetRegistry& Get();

        // The files of one kind, as paths on THIS machine — each row's key expanded through
        // `AssetHandle::PathForStableKey`. This is the call that replaced
        // `ListFilesRecursive( root )` filtered by extension at sixteen call sites.
        [[nodiscard]] std::vector<std::filesystem::path> FilesOfKind( Common::Content::ContentKind kind );

        // Records that `file` is content and that the engine knows it by `effectiveHandle`. Called by
        // `AssetManager::CreateAsset`; idempotent, and cheap enough to be on that path (a hash lookup
        // and, on a genuinely new file, one insert).
        //
        // A path whose extension is not one of the census's kinds is IGNORED rather than refused: the
        // manager also creates assets for `.dgraph` documents and for procedural and memory-backed
        // keys, none of which any scan enumerates. `KindForFile` is the one place that decides.
        void NoteAsset( const std::filesystem::path& file, uint64_t effectiveHandle );

        // Records that `file` is content, WITHOUT claiming to know the handle the engine will know it
        // by. Called by the cook at the moment it writes a cooked file (`WriteCookedJson`), which is
        // the one place a `.stmesh`, `.skeleton`, `.anim` or `.tex` comes into existence.
        //
        // WHY TWO FUNCTIONS AND NOT A DEFAULT ARGUMENT. A `.tex` declares a handle of its own INSIDE
        // the file; the cook writes the bytes and the row, and the declared identity only becomes
        // known when something parses it — which is `NoteAsset`, later, from `CreateAsset`. Passing 0
        // through `NoteAsset` would mean "the identity is the path-derived one", which for a `.tex` is
        // a claim that is false and would be written into the row. Not knowing and knowing-it-is-none
        // are different answers, so they are different calls.
        void NoteFile( const std::filesystem::path& file );

        // Which kind, if any, a file belongs to — by extension, over the census. std::nullopt means
        // "not scanned content", which is an answer and not a failure.
        [[nodiscard]] std::optional<Common::Content::ContentKind> KindForFile( const std::filesystem::path& file );

        // THE COOK. Walks every content root, enters files that have no row, drops rows whose file is
        // gone, and re-reads dependency edges from `manager` for every asset it holds. Writes the file
        // when anything changed. Editor only.
        struct RefreshOutcome
        {
            std::size_t Rows      = 0;
            std::size_t Added     = 0;
            std::size_t Removed   = 0;
            std::size_t Edges     = 0;
            bool        Written   = false;
            std::string Describe() const;
        };
        [[nodiscard]] Common::ResultStr<RefreshOutcome> Refresh( AssetManager& manager );

        // Writes the current rows to `Common::Utils::AssetRegistry::DefaultPath()`. Separate from
        // `Refresh` so the editor can flush rows that `NoteAsset` added during a session without
        // re-walking the disk.
        [[nodiscard]] Common::BoolResultStr Save();

        // Has a row been added or changed since the last `Load`/`Save`? The editor flushes on this
        // rather than writing the file on every import.
        [[nodiscard]] bool Dirty();

        // EXISTS FOR TESTS ONLY, for `AssetPathIndex::Clear`'s reason: a suite that moves the project
        // root underneath the registry must not judge its second run against the first run's rows.
        void ResetForTest();
    } // namespace ContentRegistry
} // namespace Desert::Assets
