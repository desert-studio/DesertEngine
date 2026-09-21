#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Common::Utils
{
    // THE COOKED ASSET REGISTRY: what the project's content is, without the content.
    //
    // ── WHAT IT IS FOR, IN ONE SENTENCE ───────────────────────────────────────────────────────────
    //
    // `AssetHandle::FromCookedPath` is a one-way hash, so a number read out of a `.desce` names a file
    // only if SOMETHING already hashed that file's path. Until now the only thing that did, wholesale,
    // was the preloader's directory walk over eight content roots at every boot — which is why the
    // eager preloader could not be removed by making loads lazy (`AssetPathIndex.hpp` says exactly
    // this, and `[AssetPathIndex] boot finished — N handle(s)` is the number it left behind). This file
    // is the other way of hashing them: a list produced ONCE at cook time and read back as one file.
    //
    // ── WHAT A ROW IS, AND WHAT IS DELIBERATELY NOT IN ONE ────────────────────────────────────────
    //
    // A row is one content FILE. It carries:
    //
    //   * the STABLE KEY (`cooked:Textures/T.tex`) — the same string `AssetHandle::StableKeyForPath`
    //     produces, so a registry cooked on one machine means the same thing on another. Not a path:
    //     a path carries the cooking machine's home directory, which this repository has paid for
    //     three separate times.
    //   * the SIZE in bytes at cook time. It is what makes a stale row detectable without reading the
    //     payload, and it is the column T2.5's size map will rank by.
    //   * the KIND — which scan the file belongs to (`StaticMesh`, `Texture`, `Material`, ...). An
    //     opaque string here on purpose: `Common` must not learn the engine's asset class list, and
    //     the kind census that owns these spellings lives beside the loader that uses them.
    //   * DEPENDENCY EDGES, as the handles this asset names. Handles and not keys, because a
    //     reference in this engine IS a handle: a material's texture slot may hold an identity the
    //     file declared rather than one derived from a path, and a key column could not express it.
    //   * the DECLARED IDENTITY, when the file carries a handle of its own (a `.tex` stores `Handle`,
    //     a `.demat` stores `MaterialId`). Absent — written `-` — for the great majority.
    //
    // WHAT IS NOT IN A ROW IS THE PATH-DERIVED HANDLE, and the absence is the design. It is
    // `AssetHandle::FromKey( Key )` and nothing else, so storing it would be a second spelling of a
    // value the row already determines — i.e. exactly the shape that lets two columns disagree while
    // each looks right. `Desert/Tests/Common/CookedAssetRegistry` asserts the derivation over the
    // shipped registry rather than trusting that a writer kept two columns in step.
    //
    // ── THE FORM, AND WHY IT IS NOT JSON ──────────────────────────────────────────────────────────
    //
    //   DesertAssetRegistry 1
    //   <size> <kind> <identity:16 hex | -> <deps: 16 hex, comma separated | -> <key>
    //
    // Five columns, the key LAST so a key containing a space round-trips — every rule here is taken
    // from `ContentManifest`, which is the same kind of artifact for the same kind of reason (a
    // per-line text file that a human reads in a diff, that sorts and compares by line, and that
    // needs nothing from reflect-cpp, which `Common` does not link).
    //
    // ROWS ARE HELD SORTED BY KEY. Two cooks of one tree must serialize to the same bytes whatever
    // order the filesystem walked them in, or the file cannot be diffed, cannot be committed
    // meaningfully, and the cook gate's "is this current?" becomes a question about walk order.
    //
    // ── WHAT IT IS NOT ────────────────────────────────────────────────────────────────────────────
    //
    // It is not a cache that repairs itself. A file on disk that has no row is a file the engine does
    // not have — that is the whole point of removing the walk, and it is also the danger T2.7 names:
    // "on disk" stops meaning "shipped" the moment the walk goes. The answer is a gate that FAILS
    // (`Desert/Tests/Editor/CookedRegistryGate`), never a fallback scan that would quietly restore
    // the cost this file exists to remove.
    struct AssetRegistryEntry
    {
        std::string           Key; // stable key, e.g. "assets:Materials/M_Rock.demat"
        uint64_t              Size = 0;
        std::string           Kind;         // the scan this file belongs to; see Common/Content/ContentKinds.hpp
        uint64_t              Identity = 0; // the handle the FILE declares, or 0 when it declares none
        std::vector<uint64_t> Dependencies; // handles this asset names, read once at cook

        // The handle this file's PATH derives — `AssetHandle::FromKey( Key )`. A method rather than a
        // column, for the reason the header note gives.
        [[nodiscard]] uint64_t PathHandle() const;

        // The handle the engine will actually know this asset by: the declared identity when it has
        // one, the path-derived handle otherwise. This is the number a scene reference holds.
        [[nodiscard]] uint64_t EffectiveHandle() const;
    };

    class AssetRegistry
    {
    public:
        // Adds a row. REFUSES a second row for one key rather than replacing: two rows for one file
        // means the cook enumerated it twice, and whichever won would make the registry depend on walk
        // order — the same reason `AssetPathIndex::Record` refuses a colliding handle instead of
        // overwriting it.
        [[nodiscard]] BoolResultStr Insert( AssetRegistryEntry entry );

        // Drops the row for `key`. True when there was one. The cook calls it for a row whose file is
        // gone: a registry that keeps naming a deleted file is a registry that hands the loader a path
        // it will fail to read, once per boot, for ever.
        bool Remove( std::string_view key );

        // The two fields a row acquires AFTER it is first written, and the only two that may change
        // without the file changing. A row is created from the filesystem — key, size, kind — and the
        // identity and the edges can only be read from a LOADED asset, which happens later and in a
        // different program (the editor's cook) from where the row is first inserted.
        //
        // Both return false when there is no such row, rather than inserting one: a caller that does
        // not know whether the key is content is a caller that would insert rows for `procedural://`
        // keys and for documents no scan enumerates.
        bool SetIdentity( std::string_view key, uint64_t identity );
        bool SetDependencies( std::string_view key, std::vector<uint64_t> dependencies );

        [[nodiscard]] const std::vector<AssetRegistryEntry>& Entries() const;
        [[nodiscard]] std::size_t                            Count() const;
        [[nodiscard]] bool                                   Empty() const;

        // nullptr when nothing matches — the absence is an answer (a handle that names no content is
        // exactly what a dangling reference looks like), so it is a pointer rather than a throw.
        //
        // BY HANDLE ANSWERS FOR BOTH NUMBERS A ROW CAN BE KNOWN BY: the path-derived one and the
        // declared identity. A `.tex` is registered under its cooked path but referenced by the id
        // inside it, and a lookup that only knew one of the two would answer for half the corpus.
        [[nodiscard]] const AssetRegistryEntry* FindByHandle( uint64_t handle ) const;
        [[nodiscard]] const AssetRegistryEntry* FindByKey( std::string_view key ) const;

        // Every row of one kind, in key order. This is what replaces a directory walk at the call
        // site: `ListFilesRecursive(root)` filtered by extension becomes `OfKind("Texture")`.
        [[nodiscard]] std::vector<const AssetRegistryEntry*> OfKind( std::string_view kind ) const;

        // Binds every row's PATH-DERIVED handle to its key in `Common::AssetPathIndex`, and returns
        // how many bindings were made. THIS IS THE FUNCTION THE WHOLE FILE EXISTS FOR: after it, a
        // number read out of a `.desce` names its file with nothing having been walked, parsed or
        // loaded — which used to require the boot's directory walk and nothing else.
        //
        // THE DECLARED IDENTITY IS NOT PUBLISHED, and the reason is a distinction the implementation
        // spells out at length: the index says which STRING a number was hashed from, this registry
        // says which FILE a number names, and for a cooked texture those are two different files.
        [[nodiscard]] std::size_t PublishIdentities() const;

        [[nodiscard]] std::string                     Serialize() const;
        [[nodiscard]] static ResultStr<AssetRegistry> Parse( std::string_view text );

        // Where the cooked registry lives: under the project's `Cooked/` tree, which is already a
        // packaged tree (`Editor/Source/Editor/Packaging/PackagedContentTrees.hpp`), so a shipped game
        // gets it through the same `.dpak` mount as everything else with no extra plumbing.
        [[nodiscard]] static std::filesystem::path DefaultPath();

        // Reads and parses the registry at `path`, through the VFS — so a packaged game reads it out
        // of `Content.dpak` exactly as a loose checkout reads it off disk.
        [[nodiscard]] static ResultStr<AssetRegistry> LoadFrom( const std::filesystem::path& path );

    private:
        std::vector<AssetRegistryEntry> m_Entries; // sorted by Key
        // Built by Insert, never by a separate pass: an index rebuilt on demand is an index that can
        // be forgotten.
        //
        // IT MAPS TO THE KEY AND NOT TO A ROW INDEX, which is the difference between an index that is
        // correct and one that is correct until the next Insert. Rows are held sorted, so inserting a
        // row shifts every index after it; a map of numbers to positions would have been silently
        // right for a registry built in sorted order and silently wrong for one built in any other.
        std::unordered_map<uint64_t, std::string> m_KeyByHandle;
    };
} // namespace Common::Utils
