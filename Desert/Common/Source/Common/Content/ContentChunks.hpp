#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/AssetRegistry.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Common::Content
{
    // WHICH ARCHIVE A FILE SHIPS IN — the unit of shipment, and how it is decided.
    //
    // ── WHAT THE OWNER ASKED FOR, AND WHICH THREE THINGS CARRY IT ─────────────────────────────────
    //
    // "We should try to divide into modules, so it is easier to update and patch." Reduced to what
    // actually has to exist, that is three statements and nothing else:
    //
    //   1. THE UNIT OF SHIPMENT IS A CHUNK, not the whole content set. An update carries one chunk.
    //   2. MEMBERSHIP IS DATA AND IS DERIVED, not authored. `AssetRegistry`'s `Dependencies` column
    //      already holds asset->asset edges, so "what travels together" is one walk of a file we
    //      already cook, and no second entity has to be kept in step with it.
    //   3. A PATCH IS AN OVERLAY with higher precedence; the base archive is never rewritten. That is
    //      `VFS`'s mount stack, which already exists, plus the rule that appends never touch the old
    //      index (PakFile.hpp).
    //
    // WHAT WE DELIBERATELY DID NOT COPY FROM UNREAL, each with its reason, because "copy the pattern,
    // not the letter" is only honest if the refusals are written down:
    //
    //   * `pakchunkN` / the `_p` suffix. Precedence here is the MOUNT ORDER and the manifest below,
    //     never a filename convention — a name a human types is a name a human mistypes, and the
    //     archive that wins would then depend on lexicographic luck.
    //   * Primary Asset Labels as a separate authored entity. UE derives a chunk from labels PLUS the
    //     dependency graph — two sources, one of them hand-maintained. Our edges are already IN the
    //     asset's own registry row, so one walk answers it. Fewer entities, same answer. (Our
    //     equivalent of "the label" is a chunk's ROOTS, below, and it is the only authored part.)
    //   * IoStore / .utoc containers. A second container format is a second reader to keep correct,
    //     and every property it would buy us here is a property `.dpak` v3 already has.
    //
    // ── THE SCHEME IS A LIST OF OVERRIDES, NEVER A LIST OF CONTENTS ───────────────────────────────
    //
    // This is the one rule the whole file is shaped around, and it has a scar behind it:
    // `PackagedContentTrees.hpp` exists because a hand-written sequence of packaging calls FORGOT
    // fonts and icons, a shipped game contained no `.ttf` at all, and the first frame with text died.
    // A schema that ENUMERATES what ships is a list you can silently fall out of.
    //
    // So the assignment is TOTAL BY CONSTRUCTION: every key that is not placed anywhere else is in
    // the base chunk. Forget a line and content ships in the base — wasteful, visible, harmless.
    // The opposite failure — content that reaches no archive and therefore no player — is not
    // expressible here, and `ChunkFor` returning `BASE_CHUNK` for an unknown key is what makes that
    // true rather than a promise in a comment.
    inline constexpr std::string_view BASE_CHUNK_NAME = "Base";

    // The base chunk's index. Zero, and every unplaced key resolves to it — see above.
    inline constexpr std::size_t BASE_CHUNK = 0;

    // ONE CHUNK'S AUTHORED PART: its name, and the assets whose dependency closure it is.
    //
    // ROOTS AND NOT A MEMBER LIST, and the difference is the whole design. A member list is the
    // whitelist this file refuses to be; roots name the few things a region is ABOUT (today: a
    // handful of assets; once `WorldPartition` lands, a cell's entities — `PROGRAMME.md` §8) and the
    // registry's edges do the rest. Adding a texture to a material then moves the texture into the
    // right chunk with nobody editing anything.
    struct ChunkRule
    {
        std::string              Name;
        std::vector<std::string> Roots; // stable keys, e.g. "assets:Materials/M_Rock.demat"
    };

    // THE ONLY THINGS A HUMAN WRITES. Everything else in a plan is derived.
    struct ChunkScheme
    {
        std::vector<ChunkRule> Chunks;

        // Keys pinned to the base archive whatever the derivation concluded. This is the
        // NON-DERIVABLE half named in `PROGRAMME.md`: what has to be readable BEFORE a world loads
        // (the loading screen, its font, the UI that draws it) cannot be deduced from a dependency
        // graph, because nothing in the graph says "this is needed first".
        //
        // It can only ever move content TOWARDS the base — i.e. towards being present — so a wrong
        // line here costs bytes, never a missing asset.
        std::vector<std::string> AlwaysBase;
    };

    // Parses the scheme from its JSON form (reflect-cpp). Both fields are REQUIRED and an empty text is
    // refused: "one archive" is a choice the file states (no chunks declared), never what a missing,
    // blank or half-written scheme silently turns into (owner, 2026-09-25).
    ResultStr<ChunkScheme> ParseChunkScheme( std::string_view json );
    std::string            WriteChunkScheme( const ChunkScheme& scheme );

    // The file a project keeps its scheme in, beside the project descriptor.
    std::filesystem::path ChunkSchemePath();

    // Reads and parses the scheme at @p path. An ABSENT file is a refusal that names the path and the
    // way out (WriteDefaultChunkScheme) — packaging never invents a division the project did not state.
    ResultStr<ChunkScheme> LoadChunkScheme( const std::filesystem::path& path );

    // THE ONE WAY a default scheme comes into existence: the explicit single-archive scheme (no chunks,
    // so everything ships in the base), written atomically to @p path. Refuses to overwrite an existing
    // file — a default must never replace a division somebody authored. The packaging panel's button,
    // the palette command and the tests all go through this function.
    BoolResultStr WriteDefaultChunkScheme( const std::filesystem::path& path );

    // The registry-free half of BuildChunkPlan's refusals: every chunk nameable as a file, none called
    // the base, no two alike, none without roots. BuildChunkPlan runs it first; an editor runs it on
    // an edit before any registry is at hand, so both refuse with one text.
    BoolResultStr ValidateChunkScheme( const ChunkScheme& scheme );

    // THE ANSWER: a total function from a stable key to the archive it ships in.
    class ChunkPlan
    {
    public:
        // Chunk names, base FIRST and always present. Index into this is what ChunkFor returns.
        [[nodiscard]] const std::vector<std::string>& Names() const;
        [[nodiscard]] std::size_t                     Count() const;

        // WHICH CHUNK THIS KEY SHIPS IN. Never fails, and an unknown key is BASE_CHUNK — see the
        // header note on why that direction is the safe one.
        [[nodiscard]] std::size_t ChunkFor( std::string_view stableKey ) const;

        // Only the keys the derivation MOVED off the base. Kept so a caller can report the division
        // without walking the registry again, and so the census can ask "which rows does the plan
        // claim to have placed" separately from "which rows ended up in an archive".
        [[nodiscard]] const std::unordered_map<std::string, std::size_t>& Placed() const;

        // Dependency handles that named no registry row while the closure was walked. NOT a refusal:
        // a dangling reference is a content defect that `AssetReferences` owns, and refusing to
        // package because of one would make a broken material unshippable. It is reported instead of
        // dropped, because a closure that silently ignored edges would produce a chunk that is too
        // small and look exactly like a correct one.
        [[nodiscard]] const std::vector<uint64_t>& UnresolvedEdges() const;

    private:
        friend ResultStr<ChunkPlan> BuildChunkPlan( const Utils::AssetRegistry&, const ChunkScheme& );

        std::vector<std::string>                     m_Names{ std::string( BASE_CHUNK_NAME ) };
        std::unordered_map<std::string, std::size_t> m_Placed;
        std::vector<uint64_t>                        m_Unresolved;
    };

    // Derives the plan. The registry is the DATA; the scheme only names roots and pins.
    //
    // REFUSES BY NAME rather than shrugging, in every case where a silent answer would be an empty
    // one: a chunk with no name, two chunks with one name, a chunk calling itself the base, a chunk
    // with no roots (it would ship an empty archive and look like a success), and a root or a pin
    // naming a key the registry does not have (a typo would otherwise produce a chunk containing
    // nothing, and "nothing" is what a correct empty region looks like too).
    //
    // AN ASSET REACHED FROM TWO CHUNKS GOES TO THE BASE and is not duplicated into both. That is a
    // derived rule, not a choice: duplicating it ships the bytes twice and, worse, makes "which copy
    // did the player get" depend on mount order for a file nobody meant to override.
    ResultStr<ChunkPlan> BuildChunkPlan( const Utils::AssetRegistry& registry, const ChunkScheme& scheme );

    // Writes an EDITED scheme over @p path, atomically and in the canonical layout. Refuses — writing
    // nothing — whatever BuildChunkPlan would refuse against @p registry, so a scheme the editor saved
    // is one the packager accepts; nothing is corrected on the way (a bad name stays the author's to fix).
    BoolResultStr SaveChunkScheme( const std::filesystem::path& path, const ChunkScheme& scheme,
                                   const Utils::AssetRegistry& registry );

    // One content folder and how many of its registry rows land in each chunk (index-matched to the
    // plan's Names(); [BASE_CHUNK] is what no chunk claimed, plus shared and pinned rows).
    struct ChunkFolderRow
    {
        std::string              Folder; // stable-key prefix, e.g. "assets:Materials"
        std::vector<std::size_t> FilesPerChunk;
    };

    // Every registry row grouped by folder, each resolved through ChunkPlan::ChunkFor — the function
    // WriteChunkedPaks files a source under — so the panel shows the division the archives will have.
    std::vector<ChunkFolderRow> SummarizeChunkFolders( const Utils::AssetRegistry& registry,
                                                       const ChunkPlan&            plan );

    // ── WRITING THE ARCHIVES ──────────────────────────────────────────────────────────────────────

    // The archive key the base carries its chunk list under.
    //
    // AN ORDINARY ENTRY AND NOT A RESERVED ONE like `.dpak-deleted`, deliberately: a reserved entry
    // is invisible to `KeysWithPrefix`, so it is invisible to `ContentManifest`, so a patch could
    // never replace it — and an update that ADDS a chunk must be able to ship a new list. It follows
    // the packaged `.deproj` descriptor, which is an ordinary root entry for the same reason.
    inline constexpr std::string_view CHUNK_MANIFEST_KEY = "Content.dchunks";

    // How a chunk archive is named beside its base: `<dir>/Chunk_<name>.dpak`.
    //
    // THE NAME IS NOT THE MECHANISM — the base's manifest is, and the runtime mounts what the
    // manifest names. The prefix exists for exactly one reason: `FindBasePak` picks the game's
    // archive by scanning the folder, and without a prefix it can distinguish a chunk from a game
    // only by exclusion, i.e. by reading a manifest out of an archive it has not identified yet.
    std::filesystem::path ChunkArchivePath( const std::filesystem::path& baseArchive, std::string_view chunkName );

    struct ChunkedWriteStats
    {
        std::vector<std::filesystem::path> Archives; // index-matched to the plan's Names()
        std::vector<std::size_t>           Entries;
        std::vector<uint64_t>              Bytes;
    };

    // Writes one archive per chunk out of `files` — (archive key, source path) pairs, exactly what a
    // packager already holds. Each file's chunk is `ChunkFor( StableKeyForPath( source ) )`, so a
    // file the registry has never heard of (a font, an icon, a scene) lands in the base.
    //
    // A DECLARED CHUNK THAT RECEIVES NO FILE IS A REFUSAL, not an empty archive: it is what a chunk
    // whose roots are all shared with another chunk produces, and an empty archive reads exactly like
    // a correct small region. This is the half of "no asset in zero chunks" that fires on the
    // PRODUCER side, before anything ships.
    // `baseBlobs` are (key, bytes) that are NOT files on disk and always belong to the base: the
    // packaged project descriptor is the one that exists today. They go to the base rather than
    // through the plan because they have no path to derive a stable key from — and because a game
    // that cannot be identified without mounting a chunk is a game that cannot be identified.
    ResultStr<ChunkedWriteStats>
    WriteChunkedPaks( const std::filesystem::path& baseArchive, const ChunkPlan& plan,
                      const std::vector<std::pair<std::string, std::filesystem::path>>& files,
                      const std::vector<std::pair<std::string, std::string>>&           baseBlobs = {} );

    // The chunk names the manifest text lists, in mount order. Base is NOT in it — the base is the
    // archive the manifest was read out of.
    ResultStr<std::vector<std::string>> ParseChunkManifest( std::string_view text );
    std::string                         WriteChunkManifest( const ChunkPlan& plan );
} // namespace Common::Content
