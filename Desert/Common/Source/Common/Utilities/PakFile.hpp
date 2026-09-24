#pragma once

#include <array>
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
    // ships SEVERAL paks (base + per-region chunks + patches) mounted as a stack — see VFS.
    //
    //   [ magic "DPK3" | u32 entryCount | u64 indexOffset ]   header (16 bytes)
    //   [ blob | blob | ... ]                                 entry payloads, as stored
    //   [ index record * entryCount ]                         index (at indexOffset)
    //
    //   index record (v3):
    //     u32 pathLen | path utf8 | u64 offset | u64 storedSize | u64 size | u64 hash | u32 crc | u32 codec
    //
    // THE MAGIC IS THE VERSION SEQUENCE, and it is the whole migration story: DPK1, DPK2, DPK3, in
    // that order, each a superset of the last. The reader accepts all three and says which it read;
    // anything that begins "DPK" and is not one of them is refused BY NAME ("a DPK7 archive — this
    // build reads DPK1..DPK3"), not as "corrupt", because a newer archive in an older build is a
    // version problem with a different remedy than a damaged download. There is no in-place migration
    // and there must not be one: an archive is a build artifact, so the migration of a v1 pak is to
    // cook it again, and the only thing the reader owes it is to keep reading it meanwhile.
    //
    //   v1  offset/size only. No integrity column at all; read unverified, as it always was.
    //   v2  + hash: FNV-1a 64 of the CONTENT. Verified on every read, at 0.77 GB/s (see below).
    //   v3  + storedSize/crc/codec: the payload may be compressed, and the integrity check is a
    //       CRC-32C of the bytes AS STORED.
    //
    // WHAT v3 CHANGED AND WHY, in the numbers that forced it (2026-09-22, Release -O2, the shipping
    // cooked tree: 1069 entries, 229 187 752 bytes, 7 repeats). Reading every entry cost 373.7 ms:
    // mounting 0.15 ms, finding all 1069 entries 0.02 ms, moving the bytes 67.2 ms — and verifying
    // them 297.4 ms. FOUR FIFTHS OF THE READ PATH WAS THE HASH, and 2.2x what fetching the same bytes
    // off cold storage costs (137 ms at the 1.67 GB/s an F_NOCACHE read measures here). The engine's
    // own ordering had compression first and hashing second; it is the other way round.
    //
    //   * INTEGRITY moved from FNV-1a to CRC-32C (Crc32c.hpp), 0.77 GB/s -> 8.17 GB/s, so the phase
    //     costs ~28 ms instead of 297 ms and stops being the bottleneck. What it checks is stated in
    //     that header: strictly stronger for the burst corruption storage actually produces, 2^-32
    //     instead of 2^-64 against uniformly random corruption, and neither is a defence against
    //     deliberate modification — which is unchanged, and which PakContentHash below still names
    //     the trigger for.
    //   * COMPRESSION is per entry, never per archive, and it is a POLICY the DATA decides: measured
    //     over the whole shipping corpus, .stmesh and .spv compress 1.9x while .png, .gif, .fbx and
    //     the baked volumes compress 1.00-1.03x, so a whole-archive codec would pay decompression on
    //     a third of the bytes to save nothing on them. The packer compresses an entry only when it
    //     saves at least 37.5 % (kCompressionNumerator/kCompressionDenominator), and stores it
    //     verbatim otherwise. The ONE kind that is exempt whatever its bytes do is listed, with its
    //     reason, in kStoredVerbatimRules below.
    //   * ON LOAD TIME ALONE, COMPRESSION IS A MEASURED REFUSAL ON THIS MACHINE, and the condition
    //     that would return it is written down rather than left to taste. A codec pays exactly when
    //     the storage device is slower than P*(r-1)/r for decompression speed P and ratio r; ours is
    //     P = 2280 MB/s measured on an 8 MB .stmesh, r = 1.92, so the break-even device is 1091 MB/s
    //     and this machine's storage reads at 1670. On the whole cooked archive that predicts 179 ms
    //     compressed against 165 ms stored — 8 % SLOWER. It is switched on anyway for the reason the
    //     owner asked for chunked paks in the first place ("so it is easier to update and patch"):
    //     it is 41 % fewer bytes to ship, and on the SATA-SSD and spinning media a shipped game
    //     actually meets (550 MB/s) the same archive loads 23 % FASTER, not slower.
    //
    // Paths are mount-root-relative, generic (forward-slash) strings — e.g. "Assets/Scenes/Main.desce".
    // No encryption; that would be a v4.

    // Content hash used by the pak index (FNV-1a 64) — public so tools/tests hash the same way, and
    // also the unit of account in a ContentManifest: what the index carries per entry is exactly what
    // a manifest records per file, so a manifest of a tree and a manifest of the archive built from it
    // are the same manifest.
    //
    // SINCE v3 THIS IS IDENTITY, NOT INTEGRITY, and the split is the point. This value answers "is
    // this the same content as the one that release recorded" — it is computed once at cook, diffed by
    // `PakTool patch`, and recorded in every manifest a release keeps. Integrity — "did these bytes
    // survive the disk and the download" — is asked at EVERY read and is now a CRC-32C of the stored
    // bytes (Crc32c.hpp). Making the read path cheap therefore cost nothing here: this function is
    // untouched, so every manifest ever recorded still compares, and no corpus migration exists.
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
    // How much smaller a compressed payload has to be before the packer keeps it. 5/8 is a ratio of
    // 1.6, and it is derived rather than picked: the codec repays its decompression only on a device
    // slower than P*(r-1)/r, so with the decoder measured at P = 2280 MB/s a ratio of 1.6 puts the
    // break-even at 855 MB/s — below every SSD and above every spinning disk and network. Entries
    // that do not clear it (every .png, .gif, .fbx and baked volume in the tree, at 1.00-1.03x) are
    // stored verbatim and cost no decompression at all.
    inline constexpr uint64_t kCompressionNumerator   = 5;
    inline constexpr uint64_t kCompressionDenominator = 8;

    // THE ONE EXCEPTION TO "THE DATA DECIDES", AND IT IS A LIST OF REASONS, NOT A LIST OF EXTENSIONS.
    //
    // The policy above asks the bytes and keeps the answer, which is right for every kind of content
    // whose only question is how many bytes it costs to ship. It is WRONG for a container whose whole
    // purpose is that a PART of it can be read on its own, because compressing the entry destroys that
    // property silently: the archive still round-trips, every test still passes, and the only thing
    // that changed is that reading one mip now costs decompressing the whole texture.
    //
    // MEASURED, NOT FEARED (2026-09-23, this tree, the archive's own Lz4Block at the threshold above).
    // The cooked `T_Checker.tex` is 5 592 752 bytes and LZ4 takes it to 65 825 — it saves
    // 98.8 % where the packer asks for 37.5 %, so the entry WOULD be compressed, by a factor of 26
    // more than the policy demands. Over the fourteen textures this repository can cook, twelve clear
    // the threshold as whole files. A `.tex` is a mip chain of raw pixels, and raw pixels are not PNG:
    // the container is compressible by construction, and the disease it was written to cure — "to get
    // mip 4 you must decode everything" — would come back through the archive rather than through the
    // image format. This list exists because that is a policy decision, not luck, and luck is what
    // decides it when nobody writes it down: the day per-level compression lands the whole file stops
    // clearing the threshold on its own, and an exception that only holds while a measurement holds is
    // not an exception at all.
    //
    // It is deliberately a REGISTER — one row per kind, each carrying the sentence that justifies it —
    // so that Desert/Tests/Common/Pak can pin the rows by name and derive the count from them. A gate
    // that pinned a NUMBER could be satisfied by editing the number.
    struct StoredVerbatimRule
    {
        std::string_view Extension; // lower-case, with the dot
        std::string_view Why;       // why this kind must not be compressed as one entry
    };

    inline constexpr std::array<StoredVerbatimRule, 1> kStoredVerbatimRules{ {
         { ".tex", "the cooked texture container exists so that ONE mip level can be read without "
                   "reading the file; compressing the whole entry makes the resident tail — a short "
                   "read of the smallest levels — inexpressible again. It carries its own per-level "
                   "codec instead (Engine/Assets/Serialization/TextureBinary.hpp)." },
    } };

    // The rule that forbids compressing @p key as one entry, or nullptr when the data decides as usual.
    // The extension is taken after the last '/' so a directory called "Foo.tex" cannot answer for a
    // file inside it, and it is compared ASCII-lower-cased so the verdict cannot depend on the case a
    // filesystem happened to hand back.
    const StoredVerbatimRule* StoredVerbatimRuleFor( std::string_view key );

    // How an entry's payload lies in the archive. The column exists so the DATA decides per entry;
    // see the format note above on why a per-archive codec would be the wrong shape.
    enum class PakCodec : uint32_t
    {
        Store = 0, // the payload is the content, byte for byte
        LZ4   = 1, // Lz4Block.hpp, one block, no frame
    };

    // Which archive version a file turned out to be. Reported by PakReader so a test — and `PakTool
    // list` — can say WHICH format it read rather than inferring it from which columns look plausible.
    enum class PakVersion : uint32_t
    {
        V1 = 1, // offset/size only, no integrity column
        V2 = 2, // + FNV-1a content hash, verified at read
        V3 = 3, // + storedSize/crc/codec
    };

    class PakWriter
    {
    public:
        // How a writer meets a path that already holds an archive.
        enum class Mode
        {
            // Begins a new archive, truncating whatever was there.
            Create,
            // APPENDS to an existing one. The archive must open cleanly first (a damaged archive is
            // not something to add to), and the file must already be a v3: appending to a v1 or v2
            // would mean rewriting its index in a format its own header does not declare.
            //
            // THE OLD INDEX IS NEVER OVERWRITTEN, and that single rule is what makes an interrupted
            // append cost nothing. The naive append writes new blobs where the old index starts —
            // it is exactly at the end of the blobs — so a crash halfway leaves a file whose header
            // points into rubble and whose index has already been destroyed, with nothing to
            // recover from. Here new blobs go AFTER the old index, the new index after them, and the
            // header is rewritten LAST and alone: a crash at any moment before that leaves the file
            // byte for byte as it was, because the old header still names the old index and nothing
            // reads the bytes past it. The cost is a few kilobytes of dead index per append.
            //
            // APPENDING IS A COOK OPERATION, NOT A DELIVERY ONE. The editor rebuilding one region
            // appends to that region's archive; a shipped game never appends to anything. Updates
            // reach a player as a SEPARATE pak mounted above the stack (VFS), which is how a failed
            // update stays recoverable — there is no half-written archive on the player's disk to
            // roll back.
            Append,
        };

        // Begins a new archive (truncates). Check IsOpen() before adding.
        explicit PakWriter( const std::filesystem::path& pakPath );

        // Create or Append; see Mode. In Append the entries already in the archive keep their bytes,
        // their offsets and their identities untouched, and Finalize()'s count covers ALL of them —
        // old and new — because the count is the number of index records the archive now has.
        PakWriter( const std::filesystem::path& pakPath, Mode mode );

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
            uint64_t    Offset     = 0;
            uint64_t    StoredSize = 0; // bytes in the archive
            uint64_t    Size       = 0; // bytes after decoding
            uint64_t    Hash       = 0; // FNV-1a 64 of the CONTENT — identity
            uint32_t    Crc        = 0; // CRC-32C of the STORED bytes — integrity
            PakCodec    Codec      = PakCodec::Store;
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

        // The file this reader was opened on. Exposed so that a caller walking a MOUNT STACK can say
        // which archive answered — see VFS::SourcePak.
        const std::filesystem::path& ArchivePath() const;

        // Which of the three formats this file turned out to be. Meaningful only when IsOpen().
        PakVersion Version() const;

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
        // Size of the CONTENT — what Read() hands back, and what a manifest records. Not the number
        // of bytes the entry occupies; see EntryStoredSize.
        std::optional<uint64_t> EntrySize( const std::string& key ) const;
        // Bytes the entry actually occupies in the archive. Equal to EntrySize for a stored entry and
        // for every v1/v2 archive; smaller for a compressed one.
        std::optional<uint64_t> EntryStoredSize( const std::string& key ) const;
        // How the payload lies on disk. Always Store before v3.
        std::optional<PakCodec> EntryCodec( const std::string& key ) const;
        // Content hash from the index (0 for v1 archives that predate hashing).
        std::optional<uint64_t> EntryHash( const std::string& key ) const;
        // CRC-32C of the stored bytes (0 before v3, which has no such column).
        std::optional<uint32_t> EntryCrc( const std::string& key ) const;
        // Where the payload begins in the file. Exposed because an APPEND must carry every existing
        // entry's span across unchanged — the one thing the mode promises — and because the reader's
        // own corruption message quotes an offset, so `PakTool list` has to be able to print the same
        // number beside the key when somebody goes looking for which entry it belongs to.
        std::optional<uint64_t> EntryOffset( const std::string& key ) const;

        // Keys this archive MASKS in everything mounted beneath it, parsed and validated at open (a
        // corrupt or self-contradictory list is an open failure with a reason, not a silently empty
        // list). Empty for a base archive and for every archive built before deletions existed.
        const std::vector<std::string>& DeletedKeys() const;
        bool                            IsDeleted( const std::string& key ) const;

        // Reads one entry (opens its own stream — safe to call from any thread), decoding it if it
        // was stored compressed.
        //
        // VERIFIES THE ENTRY BEFORE HANDING THE BYTES BACK, and the check is named in the failure: a
        // v3 entry by the CRC-32C of its stored bytes, a v2 entry by the FNV-1a of its content (which
        // is all a v2 archive carries), a v1 entry not at all because it carries nothing to check
        // against. A mismatch logs the key, the archive and both values and returns nullopt — corrupt
        // content is a failed read, never a successful one.
        //
        // WHY THE v3 CHECK IS OVER THE STORED BYTES AND NOT THE DECODED ONES. Decoding is
        // deterministic, so stored bytes that are provably intact decode to content that is provably
        // intact; checking after decoding would instead cost a pass over the LARGER buffer, which is
        // the cost this version exists to remove. What that does NOT cover is a fault in the decoder
        // itself, and the answer to that is a test that decodes what it packed and compares byte for
        // byte over every kind of content in the tree (Desert/Tests/Common/Pak), not a per-read check
        // that would pay for the decoder's correctness on every player's machine for ever.
        std::optional<std::string> Read( const std::string& key ) const;

        // Keys under the given prefix ("Cooked/Meshes"); prefix "" = everything.
        std::vector<std::string> KeysWithPrefix( const std::string& prefix ) const;

    private:
        struct Span
        {
            uint64_t Offset     = 0;
            uint64_t StoredSize = 0;
            uint64_t Size       = 0;
            uint64_t Hash       = 0; // 0 when the archive is v1 (pre-hash)
            uint32_t Crc        = 0; // meaningful from v3
            PakCodec Codec      = PakCodec::Store;
        };

        // The reserved entry included, so the constructor can reach it; every public accessor filters it.
        std::optional<std::string> ReadEntry( const std::string& key ) const;

        // One index lookup and one refusal of the reserved key, shared by every column accessor.
        template <typename T>
        std::optional<T> Column( const std::string& key, T Span::*column ) const;

        std::filesystem::path                 m_Path;
        std::unordered_map<std::string, Span> m_Index;
        std::vector<std::string>              m_Deleted;
        std::unordered_set<std::string>       m_DeletedLookup;
        std::string                           m_OpenError;
        bool                                  m_Ok      = false;
        PakVersion                            m_Version = PakVersion::V1;
    };
} // namespace Common::Utils
