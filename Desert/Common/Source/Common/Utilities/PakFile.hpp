#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Common::Utils
{
    // The .dpak archive format (UE .pak-style): a file bundling shipped content. A build usually
    // ships SEVERAL paks (base + per-type chunks + patches) mounted as a stack — see VFS.
    //
    //   [ magic "DPK2" | u32 entryCount | u64 indexOffset ]              header (16 bytes)
    //   [ blob | blob | ... ]                                            raw file contents
    //   [ u32 pathLen | path utf8 | u64 offset | u64 size | u64 hash ]*  index (at indexOffset)
    //
    // hash = FNV-1a 64 of the entry's content: drives `PakTool patch` (patch-pak generation) and the
    // per-entry verification Read() performs. The reader still accepts v1 "DPK1" archives (no hash
    // column; EntryHash reports 0 for them). Paths are mount-root-relative, generic (forward-slash)
    // strings — e.g. "Assets/Scenes/Main.desce". No compression/encryption yet (a future version
    // bumps the magic again).

    // Content hash used by the pak index (FNV-1a 64) — public so tools/tests hash the same way, and
    // now also the unit of account in a ContentManifest: what the index carries per entry is exactly
    // what a manifest records per file, so a manifest of a tree and a manifest of the archive built
    // from it are the same manifest.
    //
    // WHEN THIS HAS TO BECOME A CRYPTOGRAPHIC HASH, stated once so nobody has to re-derive it: when a
    // content set can be distributed PER ENTRY — when a third party can substitute one file INSIDE an
    // archive rather than replacing the archive — a 64-bit non-cryptographic hash is forgeable and the
    // per-entry column stops being a check. Today the payload is one whole .dpak, verified end to end
    // before it is unpacked, so that surface does not exist and a second hash would be a second thing
    // to keep in step with no consumer. The trigger is DISTRIBUTION SHAPE, not speed: measured on this
    // machine, hardware SHA-256 is 1.65x FASTER than this loop over the shipping tree (496 ms against
    // 819 ms for 318 MB), and it is still not worth a v3 magic today. When the trigger fires it is a
    // format bump, never a silent swap.
    uint64_t PakContentHash( const void* data, size_t size );

    // THE ONE KEY THAT IS NOT CONTENT. A patch archive carries its list of DELETED keys here, one per
    // line, and the reader parses it at open and then hides it: Contains, Read, EntrySize, EntryHash
    // and KeysWithPrefix all behave as if it were not in the archive, so no walker, no extractor and
    // no manifest can mistake the bookkeeping for a file.
    //
    // WHY INSIDE THE PATCH AND NOT A NEW FORMAT. An overlay mount can only add or override, so a
    // patch built the old way could not remove a file at all — and removals are not a corner: over
    // this repository's whole history they are 168 of 1927 content changes (8.7 %), touching 16 of
    // the 256 commits that changed packaged content. Every sixteenth update therefore had to re-ship
    // the entire 318 MB base to drop one file. Putting the list in a RESERVED ENTRY rather than a new
    // header field costs no magic bump (so already-built archives stay readable and no migration
    // exists), needs not one change in PakWriter's or PakReader's on-disk layout, and buys the
    // masking rule exactly one home — VFS::Resolve.
    inline constexpr std::string_view kDeletedEntriesKey = ".dpak-deleted";

    // ONE STREAM, ONE VERDICT, AND IT IS Finalize's (Д35). This writer used to open a fresh
    // std::ofstream per operation — one in the constructor for the header, one per blob, one for the
    // index — and let each of them be destroyed at the end of its function. `~std::ofstream` flushes
    // and SWALLOWS whatever the flush reports, so on a full disk or a disconnected volume the
    // constructor set m_Ok, every AddData returned true and recorded its entry, and Finalize returned
    // a non-zero count: three separate green answers about bytes nobody had confirmed, and an index
    // naming blobs that were never written.
    //
    // The invariant that replaces them is ONE sentence: **the index cannot name what is not on the
    // disk.** It is enforced by construction rather than by three checks — every byte of the archive
    // goes through the single stream below, that stream is closed explicitly in Finalize, and
    // Finalize's return value is read AFTER the close. So a failure at any point — header, any blob,
    // the index itself — is visible in exactly one place, and the archive is either finished and whole
    // or reported as failed.
    //
    // What that costs the caller: IsOpen() and AddData() are now honestly weaker than they look, and
    // their comments say so. Finalize() is the only function here whose answer is about the disk.
    class PakWriter
    {
    public:
        // Begins a new archive (truncates). Check IsOpen() before adding.
        explicit PakWriter( const std::filesystem::path& pakPath );

        // The archive file was opened and nothing has failed YET — not "the header reached the disk",
        // which nothing can know before the flush. A false here is worth acting on (the path is
        // unwritable); a true is permission to carry on, not a verdict. See the class comment.
        bool IsOpen() const;

        // Adds one file under the given mount-relative key ("Assets/x.desce"). Returns false on IO error
        // or when the key is the reserved deletion-list key (use SetDeletedKeys for that).
        //
        // A true means the bytes were handed to the archive's stream and the entry recorded, NOT that
        // they are on the disk — see the class comment. A failure that only the flush can see turns up
        // as Finalize() returning 0, and the archive is then never usable.
        bool AddFile( const std::string& key, const std::filesystem::path& sourceFile );
        bool AddData( const std::string& key, const void* data, size_t size );

        // Records the keys this patch DELETES from whatever is mounted beneath it. Returns false when a
        // key cannot be spelled on one line (a line break is legal in a POSIX filename and would split
        // one record into two that both parse), when it is empty, or when it is the reserved key.
        //
        // A key that this same archive also carries as CONTENT is refused at Finalize, not here: the two
        // calls can arrive in either order, and "this archive both ships and deletes X" is a
        // contradiction that must be caught whichever came second.
        bool SetDeletedKeys( const std::vector<std::string>& keys );

        // Writes the index + header, CLOSES the archive and then reports. Returns the number of INDEX
        // RECORDS written — content entries plus the deletion list if there is one — with 0 meaning
        // failure or nothing to write. Counting the deletion list is what makes a patch that ONLY
        // removes files (a legal and useful patch) come back non-zero instead of reading as an empty
        // archive.
        //
        // THIS IS THE ARCHIVE'S ONLY VERDICT. The close happens before the count is returned, so a
        // buffered failure anywhere in the archive's life reaches the caller here instead of into
        // ~ofstream after the caller has been told a number. Calling it twice returns 0 the second
        // time: the stream is shut and there is nothing left to confirm.
        size_t Finalize();

    private:
        struct Entry
        {
            std::string Key;
            uint64_t    Offset = 0;
            uint64_t    Size   = 0;
            uint64_t    Hash   = 0; // FNV-1a 64 of the content
        };

        // Appends and indexes one blob under any key, reserved included — the single body AddData and
        // the deletion list both go through.
        bool WriteBlob( const std::string& key, const void* data, size_t size );

        std::filesystem::path    m_Path;
        std::ofstream            m_Out; // the archive's ONE stream — see the class comment
        std::vector<Entry>       m_Entries;
        std::vector<std::string> m_Deleted;
        uint64_t                 m_Cursor = 0;
        bool                     m_Ok     = false;
    };

    class PakReader
    {
    public:
        // Opens + parses the index. Check IsOpen(), and read OpenError() when it is false.
        explicit PakReader( const std::filesystem::path& pakPath );

        bool   IsOpen() const;
        size_t EntryCount() const;

        // WHY the archive did not open, naming the STEP and the actual numbers — "the index is
        // declared at offset 4194304 but the file is only 1048576 bytes". Empty exactly when
        // IsOpen().
        //
        // This exists because the only thing the caller could say before was "missing or corrupt",
        // and the person who has to act on it is a player with a shipped game: no sources, no
        // editor, and one line of text between them and a game that will not start. "Missing" and
        // "the download stopped two thirds of the way through" call for completely different
        // actions, and the reader is the only place that knows which one happened.
        const std::string& OpenError() const;

        // Every accessor below reports the reserved deletion-list key as ABSENT — it is bookkeeping, not
        // content, and the only way to reach it is DeletedKeys().
        bool Contains( const std::string& key ) const;
        std::optional<uint64_t> EntrySize( const std::string& key ) const;
        // Content hash from the index (0 for v1 archives that predate hashing).
        std::optional<uint64_t> EntryHash( const std::string& key ) const;

        // Keys this archive MASKS in everything mounted beneath it, parsed and validated at open (a
        // corrupt or self-contradictory list is an open failure with a reason, not a silently empty
        // list). Empty for a base archive and for every archive built before deletions existed.
        const std::vector<std::string>& DeletedKeys() const;
        bool                            IsDeleted( const std::string& key ) const;

        // Reads one entry (opens its own stream — safe to call from any thread).
        //
        // VERIFIES THE ENTRY'S CONTENT HASH before handing the bytes back (v2 archives; v1 has no
        // hash column and is read unverified). A mismatch logs the key, the archive and both hashes
        // and returns nullopt — corrupt content is a failed read, never a successful one.
        std::optional<std::string> Read( const std::string& key ) const;

        // Keys under the given prefix ("Cooked/Meshes"); prefix "" = everything.
        std::vector<std::string> KeysWithPrefix( const std::string& prefix ) const;

    private:
        struct Span
        {
            uint64_t Offset = 0;
            uint64_t Size   = 0;
            uint64_t Hash   = 0; // 0 when the archive is v1 (pre-hash)
        };

        // The reserved entry included, so the constructor can reach it; every public accessor filters it.
        std::optional<std::string> ReadEntry( const std::string& key ) const;

        std::filesystem::path                 m_Path;
        std::unordered_map<std::string, Span> m_Index;
        std::vector<std::string>              m_Deleted;
        std::unordered_set<std::string>       m_DeletedLookup;
        std::string                           m_OpenError;
        bool                                  m_Ok        = false;
        bool                                  m_HasHashes = false; // false for a v1 archive
    };
} // namespace Common::Utils
