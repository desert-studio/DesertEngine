#pragma once

#include <Common/Core/ResultStr.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Common::Utils
{
    class PakReader;

    // A CONTENT MANIFEST: what a source handed out, one line per file, without the files.
    //
    // WHY THIS TYPE EXISTS AT ALL. Two jobs in this engine ask the same question and neither could
    // answer it:
    //
    //   - a game PATCH has to say which files changed since the last release. `PakTool diff` did it
    //     by comparing two whole .dpak archives, which means the publisher must keep every shipped
    //     archive for ever (318 MB per version) — and could not express a DELETION at all, because
    //     an overlay mount can only add or override.
    //   - a COLLECTION installed into a project has to tell "the source published a new version" from
    //     "a person edited this file". Comparing the disk against the incoming source cannot: both
    //     look like "different".
    //
    // One answer serves both, and it is not a hash — it is a THIRD REFERENCE POINT. A manifest is a
    // record of what the source last handed over, kept beside the content instead of inside it. Given
    // two of them, CompareManifests says what moved; given three (recorded / on-disk / incoming),
    // PlanContentUpdate in ContentUpdate.hpp says who moved it. See Docs/Architecture/P3_CONTENT_MANIFEST.md.
    //
    // COST, measured on the shipping tree (1268 files, 317.7 MiB): the manifest is 247 KiB, about
    // 0.076 % of the content it describes — so a release process can keep the manifest of every
    // version it has ever published and keep NO old archives.
    //
    // WHAT IT DOES NOT ANSWER. A manifest identifies a FILE, never a piece of content: two materials
    // authored with identical parameters carry different random MaterialId values and hash
    // differently for ever. It is not a deduplicator and must not be used as one.
    //
    // WHERE IT LIVES, AND WHERE IT IS GOING. Beside PakFile, because it is the pak index's own hash
    // column with the offsets left out — the two are one format and must move together. The analysis
    // proposed putting it straight into the desert-shared submodule (the launcher unpacks, the engine
    // mounts); that is the right destination and this is not it yet, for one concrete reason: PakFile
    // is still here, and a manifest type in the submodule reading a hash defined in the engine would
    // be the two-homes-for-one-format shape the submodule exists to prevent. When PakFile moves, this
    // moves with it, unchanged.

    struct ContentManifestEntry
    {
        // Mount-root-relative, generic (forward slash) — the same string a .dpak uses as its key, so
        // a manifest built from a directory and one built from the pak made of it compare equal.
        std::string Key;
        uint64_t    Size = 0;
        uint64_t    Hash = 0; // PakContentHash of the bytes (the pak index's own column)
    };

    class ContentManifest
    {
    public:
        // Entries are held SORTED BY KEY, and every producer below goes through Insert to keep that
        // true. It is not a convenience: two manifests of the same tree must serialize to the same
        // bytes whatever order the filesystem or the pak's hash map walked them in, or the artifact
        // a release keeps for ever is not comparable with the next one by eye, by diff, or by hash.
        void Insert( ContentManifestEntry entry );
        // True when the key was there to remove.
        bool Remove( const std::string& key );

        const std::vector<ContentManifestEntry>& Entries() const;
        size_t                                   Count() const;
        bool                                     Empty() const;

        // nullptr when the key is absent — the absence IS an answer here (four of the six states in
        // ContentUpdate.hpp are about a file missing from one of the three points), so it is a
        // pointer rather than a throw or a default-constructed entry.
        const ContentManifestEntry* Find( const std::string& key ) const;

        // Every regular file under `root`, keyed relative to it. `keyPrefix`, when given, is prepended
        // to each key ("Resources") so a manifest of a tree matches the pak that Package.sh builds
        // from that tree with --prefix.
        //
        // Reads every file: hashing is the point. A key that cannot be spelled on one line (a newline
        // in a path — legal on POSIX) is a hard error rather than a silently mangled record.
        static Common::ResultStr<ContentManifest> FromDirectory( const std::filesystem::path& root,
                                                                 const std::string&           keyPrefix = {} );

        // From an OPEN archive's index — no file bytes are read, the hashes are already there.
        static ContentManifest FromPak( const PakReader& pak );

        // Text form, one entry per line, key LAST so a key containing spaces round-trips:
        //
        //   DesertContentManifest 1
        //   <hash:16 hex> <size> <key>
        //
        // Deliberately not JSON. This file is an artifact a release keeps for ever and a human reads
        // in a diff; a line per file with the key last is smaller than any JSON spelling of the same
        // data, sorts and diffs by line, and — the reason that decided it — needs nothing from
        // reflect-cpp, which Common does not link.
        std::string                               Serialize() const;
        static Common::ResultStr<ContentManifest> Parse( std::string_view text );

    private:
        std::vector<ContentManifestEntry> m_Entries; // sorted by Key
    };

    // What moved between two manifests. Nothing here knows WHO moved it — that is the whole point of
    // the seam: this is the two-point comparison both consumers share, and the three-point matrix in
    // ContentUpdate.hpp is this comparison applied twice. If this function ever needs to ask whether
    // it is serving a game patch or a collection, the seam is in the wrong place.
    struct ContentDiff
    {
        std::vector<std::string> Added;   // in `to`, not in `from`
        std::vector<std::string> Changed; // in both, different hash
        std::vector<std::string> Removed; // in `from`, not in `to`

        bool Empty() const;
    };

    ContentDiff CompareManifests( const ContentManifest& from, const ContentManifest& to );

    // What building a patch produced. `Written` is a SEPARATE fact from success, and the separation is
    // the point: "the two versions are identical, so there is nothing to ship" and "a patch archive is
    // waiting at `outPatch`" are two different outcomes that both mean the operation worked, and a
    // caller that cannot tell them apart uploads a file that is not there. It used to be exactly that —
    // one `return 0` for both — which is DC §1.4's shape at the step where a release is published.
    struct PatchBuild
    {
        ContentDiff Diff;
        bool        Written = false; // false ONLY when Diff.Empty(); no archive was created
    };

    // The overlay archive that turns the release `baseManifestFile` describes into the release
    // `newerPak` is: every key `newerPak` added or changed, carried whole, plus the keys the base had
    // and `newerPak` does not, recorded as deletions (PakFile.hpp, kDeletedEntriesKey). Mount it AFTER
    // the base and the base becomes the newer release.
    //
    // WHY THIS IS A LIBRARY FUNCTION RATHER THAN PakTool's OWN LOOP. It is the consumer of the manifest
    // a release records, so it is the half a test has to be able to run in order to prove the recording
    // is worth anything — and PakTool's copy was inside `main()`'s process, reachable only by spawning a
    // binary a test cannot depend on having been built. PakTool now calls this and prints what it says.
    //
    // A MISSING OR UNREADABLE BASE MANIFEST IS A REFUSAL, never an empty base. Treating an absent
    // manifest as "the previous release contained nothing" produces a patch that is a copy of the whole
    // new release and reports success, which is the worst available answer: it is enormous, it is wrong,
    // and nothing about it says so.
    Common::ResultStr<PatchBuild> BuildPatchPak( const std::filesystem::path& baseManifestFile,
                                                 const std::filesystem::path& newerPak,
                                                 const std::filesystem::path& outPatch );
} // namespace Common::Utils
