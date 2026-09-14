#include "PakFile.hpp"

#include <Common/Core/Logger.hpp>

#include <cstring>
#include <fstream>
#include <system_error>

namespace Common::Utils
{
    namespace
    {
        constexpr char     kMagicV1[4] = { 'D', 'P', 'K', '1' }; // pre-hash (still readable)
        constexpr char     kMagicV2[4] = { 'D', 'P', 'K', '2' }; // + u64 content hash per entry
        constexpr uint64_t kHeaderSize = 4 + sizeof( uint32_t ) + sizeof( uint64_t );

        // Longest key the reader will accept. A sanity bound, not a format limit: a damaged index
        // otherwise asks for a multi-gigabyte string before anything else can notice it is wrong.
        constexpr uint32_t kMaxKeyLength = 4096;

        template <typename T>
        void WritePod( std::ofstream& out, const T& value )
        {
            out.write( reinterpret_cast<const char*>( &value ), sizeof( T ) );
        }

        template <typename T>
        bool ReadPod( std::ifstream& in, T& value )
        {
            in.read( reinterpret_cast<char*>( &value ), sizeof( T ) );
            return static_cast<bool>( in );
        }

        // A key the deletion list can hold and hand back unchanged. The list is one key per line, so a
        // line break inside a key would split one record into two that both parse — a corrupt list that
        // reads as a valid one.
        bool DeletableKey( std::string_view key )
        {
            return !key.empty() && key != kDeletedEntriesKey && key.find( '\n' ) == std::string_view::npos &&
                   key.find( '\r' ) == std::string_view::npos;
        }
    } // namespace

    uint64_t PakContentHash( const void* data, size_t size )
    {
        // FNV-1a 64: tiny, dependency-free, plenty for change detection (a diff/integrity aid,
        // not a cryptographic guarantee).
        uint64_t    h = 14695981039346656037ull;
        const auto* p = static_cast<const unsigned char*>( data );
        for ( size_t i = 0; i < size; ++i )
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // ---------------------------------------------------------------- PakWriter

    PakWriter::PakWriter( const std::filesystem::path& pakPath ) : m_Path( pakPath )
    {
        std::error_code ec;
        std::filesystem::create_directories( pakPath.parent_path(), ec );

        // in|out|trunc rather than plain trunc: this one stream both appends blobs and later seeks back
        // to patch the header, which is what makes a second handle (and a second unchecked destructor)
        // unnecessary. See PakFile.hpp on why there is exactly one stream.
        m_Out.open( m_Path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
        if ( !m_Out )
            return;

        // Placeholder header; Finalize() rewrites it with the real index offset.
        m_Out.write( kMagicV2, 4 );
        WritePod<uint32_t>( m_Out, 0 );
        WritePod<uint64_t>( m_Out, 0 );
        m_Cursor = kHeaderSize;
        m_Ok     = static_cast<bool>( m_Out );
    }

    bool PakWriter::IsOpen() const
    {
        return m_Ok;
    }

    bool PakWriter::AddFile( const std::string& key, const std::filesystem::path& sourceFile )
    {
        std::ifstream in( sourceFile, std::ios::binary );
        if ( !in )
            return false;
        std::string data( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
        return AddData( key, data.data(), data.size() );
    }

    // The one place a blob is appended and indexed. AddData is this plus the reserved-key refusal, and
    // Finalize calls it directly for the deletion list — so the list is hashed and indexed by exactly
    // the same code as content, and cannot drift from it.
    bool PakWriter::WriteBlob( const std::string& key, const void* data, size_t size )
    {
        if ( !m_Ok )
            return false;

        m_Out.seekp( static_cast<std::streamoff>( m_Cursor ) );
        m_Out.write( static_cast<const char*>( data ), static_cast<std::streamsize>( size ) );
        if ( !m_Out )
        {
            // Sticky, so the failure cannot be walked past. An archive that lost one blob must not go
            // on collecting entries for an index it can never make true; every later call now refuses
            // and Finalize returns 0.
            m_Ok = false;
            return false;
        }

        m_Entries.push_back( { key, m_Cursor, static_cast<uint64_t>( size ), PakContentHash( data, size ) } );
        m_Cursor += size;
        return true;
    }

    bool PakWriter::AddData( const std::string& key, const void* data, size_t size )
    {
        if ( key == kDeletedEntriesKey )
            return false; // reserved: SetDeletedKeys owns it
        return WriteBlob( key, data, size );
    }

    bool PakWriter::SetDeletedKeys( const std::vector<std::string>& keys )
    {
        for ( const auto& key : keys )
            if ( !DeletableKey( key ) )
                return false;
        m_Deleted = keys;
        return true;
    }

    size_t PakWriter::Finalize()
    {
        if ( !m_Ok )
            return 0;

        if ( !m_Deleted.empty() )
        {
            // "Ships X" and "deletes X" in one archive is a contradiction with no right answer at
            // resolution time, so it is refused HERE, where the producer can still be told, rather than
            // guessed at by every reader for ever.
            for ( const auto& key : m_Deleted )
                for ( const auto& entry : m_Entries )
                    if ( entry.Key == key )
                        return 0;

            std::string list;
            for ( const auto& key : m_Deleted )
            {
                list += key;
                list += '\n';
            }
            // Through the ordinary blob path, so the deletion list carries the same per-entry content
            // hash as everything else — which is what turns a damaged list into a NAMED open failure
            // instead of a shorter list nobody notices.
            if ( !WriteBlob( std::string( kDeletedEntriesKey ), list.data(), list.size() ) )
                return 0;
        }

        const uint64_t indexOffset = m_Cursor;
        m_Out.seekp( static_cast<std::streamoff>( indexOffset ) );
        for ( const auto& entry : m_Entries )
        {
            WritePod<uint32_t>( m_Out, static_cast<uint32_t>( entry.Key.size() ) );
            m_Out.write( entry.Key.data(), static_cast<std::streamsize>( entry.Key.size() ) );
            WritePod<uint64_t>( m_Out, entry.Offset );
            WritePod<uint64_t>( m_Out, entry.Size );
            WritePod<uint64_t>( m_Out, entry.Hash );
        }

        m_Out.seekp( 0 );
        m_Out.write( kMagicV2, 4 );
        WritePod<uint32_t>( m_Out, static_cast<uint32_t>( m_Entries.size() ) );
        WritePod<uint64_t>( m_Out, indexOffset );

        // close() EXPLICITLY, BEFORE the count is returned. This is the archive's single flush: the
        // header from the constructor, every blob and the index above are all still only as real as the
        // filebuf says, and a full disk or a vanished volume surfaces HERE. Left to ~ofstream it would
        // surface after this function had already handed its caller a number of entries, which is
        // exactly the "the index names a blob that is not on disk" the class comment is about.
        m_Out.close();
        // Finished either way — a second Finalize has a closed stream and nothing left to confirm, so
        // it must report failure rather than re-answer from m_Entries.
        m_Ok = false;
        if ( !m_Out )
            return 0;
        return m_Entries.size();
    }

    // ---------------------------------------------------------------- PakReader

    // The archive's own bytes for the magic field, printable so a message can quote them. A file that
    // is not a .dpak is usually another format with a recognisable marker ("PK.." for a zip, "\x7fELF"
    // for a binary), and naming what WAS found is the difference between "your archive is corrupt" and
    // "you renamed the wrong file".
    static std::string QuoteMagic( const char ( &magic )[4], std::streamsize got )
    {
        std::string out = "\"";
        for ( std::streamsize i = 0; i < got; ++i )
        {
            const unsigned char c = static_cast<unsigned char>( magic[i] );
            if ( c >= 0x20 && c < 0x7f )
                out += static_cast<char>( c );
            else
                out += "\\x" + std::string( 1, "0123456789abcdef"[c >> 4] ) +
                       std::string( 1, "0123456789abcdef"[c & 0xf] );
        }
        return out + "\"";
    }

    // EVERY `return` BELOW WRITES m_OpenError FIRST. All ten failure branches used to be bare
    // `return`s that collapsed into one word at the call site ("corrupt"), and the caller that has
    // to act on it is a player with a shipped game — see PakFile.hpp on OpenError(). Structure only:
    // the header, the index, and whether each entry's span lies inside the data region. Whether the
    // BYTES are intact is checked per entry in Read(), because proving it here would mean reading the
    // whole archive at startup.
    PakReader::PakReader( const std::filesystem::path& pakPath ) : m_Path( pakPath )
    {
        std::error_code      sizeEc;
        const std::uintmax_t fileSize = std::filesystem::file_size( m_Path, sizeEc );
        const uint64_t       bytes    = sizeEc ? 0 : static_cast<uint64_t>( fileSize );

        std::ifstream in( m_Path, std::ios::binary );
        if ( !in )
        {
            m_OpenError = std::filesystem::exists( m_Path, sizeEc )
                               ? "the file exists but could not be opened for reading (permissions?)"
                               : "there is no such file";
            return;
        }

        char magic[4] = {};
        in.read( magic, 4 );
        const std::streamsize magicRead = in.gcount();
        const bool            v1        = in && std::memcmp( magic, kMagicV1, 4 ) == 0;
        const bool            v2        = in && std::memcmp( magic, kMagicV2, 4 ) == 0;
        if ( !v1 && !v2 )
        {
            m_OpenError = fmt::format( "not a Desert archive: it begins with {}, expected \"DPK1\" or "
                                       "\"DPK2\" (the file is {} bytes)",
                                       QuoteMagic( magic, magicRead ), bytes );
            return;
        }

        uint32_t entryCount  = 0;
        uint64_t indexOffset = 0;
        if ( !ReadPod( in, entryCount ) || !ReadPod( in, indexOffset ) )
        {
            m_OpenError = fmt::format( "the {}-byte header is incomplete — the whole file is only {} bytes",
                                       kHeaderSize, bytes );
            return;
        }

        // The blobs occupy [kHeaderSize, indexOffset) and the index everything after it. An index
        // offset outside the file is the signature of a TRUNCATED DOWNLOAD, which is the single most
        // likely way a shipped patch arrives damaged, and it has to be named as that rather than as
        // "corrupt" — the remedy is to fetch it again, not to reinstall.
        if ( indexOffset < kHeaderSize || indexOffset > bytes )
        {
            m_OpenError = fmt::format( "the index is declared at offset {} but the file is {} bytes — it is "
                                       "truncated or incomplete",
                                       indexOffset, bytes );
            return;
        }

        in.seekg( static_cast<std::streamoff>( indexOffset ) );
        for ( uint32_t i = 0; i < entryCount; ++i )
        {
            uint32_t pathLen = 0;
            if ( !ReadPod( in, pathLen ) )
            {
                m_OpenError =
                     fmt::format( "the index ends after {} of {} entries — the file is truncated or incomplete", i,
                                  entryCount );
                return;
            }
            if ( pathLen == 0 || pathLen > kMaxKeyLength )
            {
                m_OpenError = fmt::format( "index entry {} of {} declares a path length of {} bytes "
                                           "(allowed 1..{}) — the index is corrupt",
                                           i + 1, entryCount, pathLen, kMaxKeyLength );
                return;
            }
            std::string key( pathLen, '\0' );
            in.read( key.data(), pathLen );
            Span span;
            if ( !in || !ReadPod( in, span.Offset ) || !ReadPod( in, span.Size ) )
            {
                m_OpenError = fmt::format( "index entry {} of {} is cut short — the index is truncated", i + 1,
                                           entryCount );
                return;
            }
            if ( v2 && !ReadPod( in, span.Hash ) ) // v1 has no hash column -> stays 0
            {
                m_OpenError = fmt::format( "index entry {} of {} ('{}') has no content hash — the index is "
                                           "truncated",
                                           i + 1, entryCount, key );
                return;
            }
            // A span that leaves the data region would read the index back as file content, or run off
            // the end of the file. Checked HERE rather than at the read, because one bad span means the
            // index itself is damaged and nothing in this archive can be trusted.
            if ( span.Offset < kHeaderSize || span.Size > indexOffset || span.Offset > indexOffset - span.Size )
            {
                m_OpenError = fmt::format( "index entry {} of {} ('{}') points at bytes {}..{}, outside the "
                                           "{}-byte content region — the index is corrupt",
                                           i + 1, entryCount, key, span.Offset, span.Offset + span.Size,
                                           indexOffset - kHeaderSize );
                return;
            }
            if ( !m_Index.emplace( std::move( key ), span ).second )
            {
                // The message deliberately does NOT quote the key: whether a failed emplace moved from
                // its argument is unspecified, so `key` may be empty here and printing it would make
                // the one branch that reports corruption print something untrue. Rejected rather than
                // tolerated because a duplicate key made EntryCount() disagree with the count in the
                // header, and left two spans for one name for Contains() and Read() to choose between.
                m_OpenError = fmt::format( "index entry {} of {} repeats a key already in the archive — the "
                                           "index is corrupt",
                                           i + 1, entryCount );
                return;
            }
        }
        m_HasHashes = v2;

        // THE DELETION LIST IS PARSED AT OPEN, NOT AT THE FIRST LOOKUP. A patch whose list is corrupt,
        // unreadable or self-contradictory must fail to OPEN, with a reason — because the alternative
        // is a patch that mounts, silently masks nothing, and leaves the game serving exactly the files
        // the update was published to remove. That is the same shape as the archive that used to mount
        // "successfully" while unreadable, and it is worse here: the update looks applied.
        if ( m_Index.contains( std::string( kDeletedEntriesKey ) ) )
        {
            const auto list = ReadEntry( std::string( kDeletedEntriesKey ) );
            if ( !list )
            {
                m_OpenError = fmt::format( "the deletion list ('{}') could not be read — its content hash does "
                                           "not match the index, so the archive is damaged",
                                           kDeletedEntriesKey );
                return;
            }
            // Unwrapped once, here, where the refusal above is still in view. Five dereferences inside the
            // loop meant five readers (and every static analyser) had to carry that refusal across a back
            // edge to know the optional was engaged.
            const std::string& text  = *list;
            size_t             start = 0;
            while ( start < text.size() )
            {
                const size_t end = text.find( '\n', start );
                std::string  key = text.substr( start, ( end == std::string::npos ? text.size() : end ) - start );
                start            = ( end == std::string::npos ) ? text.size() : end + 1;
                if ( key.empty() )
                    continue;
                if ( key == kDeletedEntriesKey )
                {
                    m_OpenError = fmt::format( "the deletion list names '{}', its own reserved key — the "
                                               "archive is corrupt",
                                               kDeletedEntriesKey );
                    return;
                }
                if ( m_Index.contains( key ) )
                {
                    m_OpenError = fmt::format( "'{}' is both shipped and deleted by this archive — there is no "
                                               "right answer to that, so it is refused rather than guessed",
                                               key );
                    return;
                }
                if ( m_DeletedLookup.insert( key ).second )
                    m_Deleted.push_back( std::move( key ) );
            }
        }

        m_Ok = true;
    }

    bool PakReader::IsOpen() const
    {
        return m_Ok;
    }

    const std::string& PakReader::OpenError() const
    {
        return m_OpenError;
    }

    size_t PakReader::EntryCount() const
    {
        // CONTENT entries. The deletion list occupies an index record but is not a file, and this count
        // is what the mount log and every "N entries" message quote.
        return m_Index.size() - ( m_Index.contains( std::string( kDeletedEntriesKey ) ) ? 1u : 0u );
    }

    bool PakReader::Contains( const std::string& key ) const
    {
        return key != kDeletedEntriesKey && m_Index.contains( key );
    }

    std::optional<uint64_t> PakReader::EntrySize( const std::string& key ) const
    {
        if ( key == kDeletedEntriesKey )
            return std::nullopt;
        const auto it = m_Index.find( key );
        if ( it == m_Index.end() )
            return std::nullopt;
        return it->second.Size;
    }

    std::optional<uint64_t> PakReader::EntryHash( const std::string& key ) const
    {
        if ( key == kDeletedEntriesKey )
            return std::nullopt;
        const auto it = m_Index.find( key );
        if ( it == m_Index.end() )
            return std::nullopt;
        return it->second.Hash;
    }

    const std::vector<std::string>& PakReader::DeletedKeys() const
    {
        return m_Deleted;
    }

    bool PakReader::IsDeleted( const std::string& key ) const
    {
        return m_DeletedLookup.contains( key );
    }

    std::optional<std::string> PakReader::Read( const std::string& key ) const
    {
        if ( key == kDeletedEntriesKey )
            return std::nullopt; // reserved: DeletedKeys() is the way in
        return ReadEntry( key );
    }

    std::optional<std::string> PakReader::ReadEntry( const std::string& key ) const
    {
        const auto it = m_Index.find( key );
        if ( it == m_Index.end() )
            return std::nullopt;

        // Own stream per read: trivially thread-safe (asset preloading runs on the JobSystem).
        std::ifstream in( m_Path, std::ios::binary );
        if ( !in )
        {
            // The file opened once, at construction, so failing here means it has gone away or become
            // unreadable UNDER a running game — rare, and exactly the sort of thing that must not
            // arrive at the caller as a plain "not found".
            LOG_ERROR( "[Pak] {} can no longer be opened; entry '{}' cannot be read", m_Path.string(), key );
            return std::nullopt;
        }

        std::string data( static_cast<size_t>( it->second.Size ), '\0' );
        in.seekg( static_cast<std::streamoff>( it->second.Offset ) );
        in.read( data.data(), static_cast<std::streamsize>( it->second.Size ) );
        if ( !in )
        {
            LOG_ERROR( "[Pak] {}: entry '{}' could not be read ({} bytes at offset {})", m_Path.string(), key,
                       it->second.Size, it->second.Offset );
            return std::nullopt;
        }

        // THE FORMAT HAS CARRIED A PER-ENTRY CONTENT HASH SINCE v2 AND NOTHING HAS EVER COMPARED IT.
        // Its own header comment promises it drives "post-download integrity checks"; the only reader
        // of EntryHash was the patch generator. That gap is not cosmetic: the constructor's checks are all
        // structural, so a flipped bit anywhere in the DATA region leaves the header, the index and
        // every span perfectly valid. The archive mounts, this read succeeds, and the game runs on
        // corrupt content without one line anywhere — the exact silent-fallback shape §1.4 forbids.
        //
        // Here, and not at mount, because the bytes are already in hand: verifying costs one pass over
        // a buffer that was just read, whereas proving the whole archive intact at startup would mean
        // reading all of it before the first frame.
        //
        // WHAT IT COSTS, measured rather than assumed (2026-09-07, -O2, 256 MB, best of 5, machine
        // shared with other agents; the five runs spanned 0.78-0.80 GB/s): 0.80 GB/s single-threaded.
        // FNV-1a is a serial chain — each byte multiplies the previous state — so that is the ceiling,
        // and it is BELOW this machine's storage bandwidth rather than above it. For a package of size
        // S the added load cost is therefore about S/0.8 GB/s, spread across the preloader's workers
        // (Read opens its own stream per call precisely so it can run on any thread). Against the
        // alternative — a game that loads corrupt bytes and says nothing — that is worth paying.
        // What would change the answer: if this ever shows up in a load profile, the fix is a faster
        // hash — a hardware CRC32C or xxHash breaks the serial dependency and should be well clear of
        // storage bandwidth, though that was NOT measured here — and that means a v3 magic, not a
        // silent skip of the check.
        if ( m_HasHashes )
        {
            const uint64_t actual = PakContentHash( data.data(), data.size() );
            if ( actual != it->second.Hash )
            {
                LOG_ERROR( "[Pak] {}: entry '{}' is CORRUPT — content hash {:#x}, index says {:#x} ({} "
                           "bytes at offset {}). The archive is damaged or was modified after packing.",
                           m_Path.string(), key, actual, it->second.Hash, it->second.Size, it->second.Offset );
                return std::nullopt;
            }
        }
        return data;
    }

    std::vector<std::string> PakReader::KeysWithPrefix( const std::string& prefix ) const
    {
        std::vector<std::string> keys;
        for ( const auto& [key, span] : m_Index )
            if ( key != kDeletedEntriesKey && ( prefix.empty() || key.rfind( prefix, 0 ) == 0 ) )
                keys.push_back( key );
        return keys;
    }
} // namespace Common::Utils
