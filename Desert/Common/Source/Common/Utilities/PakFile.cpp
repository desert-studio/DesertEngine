#include <algorithm>
#include "PakFile.hpp"

#include "Crc32c.hpp"
#include "Lz4Block.hpp"

#include <Common/Core/Logger.hpp>

#include <bit>
#include <climits>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace Common::Utils
{
    namespace
    {
        constexpr char     kMagicV1[4] = { 'D', 'P', 'K', '1' }; // pre-hash (still readable)
        constexpr char     kMagicV2[4] = { 'D', 'P', 'K', '2' }; // + u64 content hash per entry
        constexpr char     kMagicV3[4]     = { 'D', 'P', 'K', '3' }; // + storedSize / crc / codec per entry
        constexpr char     kMagicPrefix[3] = { 'D', 'P', 'K' };      // so a FUTURE version is named, not guessed
        constexpr uint64_t kHeaderSize = 4 + sizeof( uint32_t ) + sizeof( uint64_t );

        // THE FORMAT IS A BYTE ORDER, A SET OF FIELD WIDTHS AND NOTHING ELSE, so both are pinned at
        // the BUILD rather than described in a comment. Every field below is written by copying a
        // fixed-width object's representation into the stream, so a big-endian build would silently
        // emit an archive that every shipping target rejects, and a host with a different int width
        // would emit one nothing can parse at all. Both shipping targets are little-endian with 8-bit
        // bytes; a port that breaks either is told here, at compile time, and not at a player's load.
        static_assert( std::endian::native == std::endian::little, "the .dpak index is little-endian" );
        static_assert( CHAR_BIT == 8, "the .dpak index's sizes are in 8-bit bytes" );
        static_assert( sizeof( uint32_t ) == 4 && sizeof( uint64_t ) == 8 );
        static_assert( sizeof( PakCodec ) == 4, "the codec column is a u32 in the file" );
        static_assert( kHeaderSize == 16, "the header is magic + u32 + u64" );

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

    PakWriter::PakWriter( const std::filesystem::path& pakPath ) : PakWriter( pakPath, Mode::Create )
    {
    }

    PakWriter::PakWriter( const std::filesystem::path& pakPath, Mode mode ) : m_Path( pakPath )
    {
        std::error_code ec;
        std::filesystem::create_directories( pakPath.parent_path(), ec );

        if ( mode == Mode::Append )
        {
            // THE ARCHIVE IS OPENED AND VALIDATED BEFORE A BYTE IS ADDED TO IT. Appending to a file
            // that does not parse would produce an archive that is damaged in two places instead of
            // one, and the second damage would be ours.
            PakReader existing( pakPath );
            if ( !existing.IsOpen() )
            {
                LOG_ERROR( "[Pak] cannot append to {}: {}", pakPath.string(), existing.OpenError() );
                return;
            }
            if ( existing.Version() != PakVersion::V3 )
            {
                // Refused rather than upgraded in place: rewriting a v1/v2 index as v3 means moving
                // every byte of the file, which is a repack, and a repack is not an append. The caller
                // that wants one calls it by its name.
                LOG_ERROR( "[Pak] cannot append to {}: it is a v{} archive and appending writes a v3 "
                           "index; repack it instead",
                           pakPath.string(), static_cast<uint32_t>( existing.Version() ) );
                return;
            }
            if ( !existing.DeletedKeys().empty() )
            {
                // A patch archive states a complete removal set. Appending content to one would leave
                // a file whose deletion list was authored against a different set of entries, and
                // there is no way to tell from the file which entries the list was meant to cover.
                LOG_ERROR( "[Pak] cannot append to {}: it is a patch archive (it carries {} deletion(s))",
                           pakPath.string(), existing.DeletedKeys().size() );
                return;
            }

            // EVERY COLUMN IS CHECKED, THOUGH THE KEY CAME FROM THIS SAME READER. It reads like a
            // formality and it is not: these six values are copied straight into the index this mode is
            // about to WRITE, so a column that came back empty would be stored as a zero and the new
            // index would describe the old entry incorrectly — an archive that opens, mounts, and hands
            // back the wrong bytes. There is no later point that could notice.
            //
            // So a gap abandons the append instead of completing it. The file is still untouched here:
            // `m_Out` is opened below, and the rule this mode exists for is that the old index is never
            // overwritten. Refusing now costs a repack; continuing costs a silently wrong archive.
            for ( const auto& key : existing.KeysWithPrefix( "" ) )
            {
                const auto offset     = existing.EntryOffset( key );
                const auto storedSize = existing.EntryStoredSize( key );
                const auto size       = existing.EntrySize( key );
                const auto hash       = existing.EntryHash( key );
                const auto crc        = existing.EntryCrc( key );
                const auto codec      = existing.EntryCodec( key );
                if ( !offset || !storedSize || !size || !hash || !crc || !codec )
                {
                    LOG_ERROR( "[Pak] cannot append to {}: its index lists '{}' but cannot describe it "
                               "fully, so the entry cannot be carried into the new index; repack instead",
                               pakPath.string(), key );
                    m_Entries.clear();
                    return;
                }
                m_Entries.push_back( { key, *offset, *storedSize, *size, *hash, *crc, *codec } );
            }

            m_Out.open( m_Path, std::ios::binary | std::ios::in | std::ios::out );
            if ( !m_Out )
                return;
            // PAST THE OLD INDEX, NOT OVER IT — the rule the whole mode exists for (PakFile.hpp).
            // The old index ends at the end of the file, so that is where the new blobs begin.
            m_Out.seekp( 0, std::ios::end );
            m_Cursor = static_cast<uint64_t>( m_Out.tellp() );
            m_Ok     = static_cast<bool>( m_Out ) && m_Cursor >= kHeaderSize;
            return;
        }

        // in|out|trunc rather than plain trunc: this one stream both appends blobs and later seeks back
        // to patch the header, which is what makes a second handle (and a second unchecked destructor)
        // unnecessary. See PakFile.hpp on why there is exactly one stream.
        m_Out.open( m_Path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc );
        if ( !m_Out )
            return;

        // Placeholder header; Finalize() rewrites it with the real index offset.
        m_Out.write( kMagicV3, 4 );
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

        // THE DATA DECIDES, not a table of extensions. Compress, and keep the result only if it
        // cleared the threshold; everything else is stored verbatim and costs the reader nothing.
        // An extension list would have to be maintained against a content tree that grows, and it
        // would be wrong the first time a .desce became a container or a .tex became BCn.
        const char*       stored     = static_cast<const char*>( data );
        uint64_t          storedSize = size;
        PakCodec          codec      = PakCodec::Store;
        std::vector<char> compressed;
        if ( size > 0 )
        {
            compressed.resize( Lz4BlockBound( size ) );
            const size_t packed = Lz4BlockCompress( data, size, compressed.data(), compressed.size() );
            if ( packed > 0 && static_cast<uint64_t>( packed ) * kCompressionDenominator <=
                                    static_cast<uint64_t>( size ) * kCompressionNumerator )
            {
                stored     = compressed.data();
                storedSize = packed;
                codec      = PakCodec::LZ4;
            }
        }

        m_Out.seekp( static_cast<std::streamoff>( m_Cursor ) );
        m_Out.write( stored, static_cast<std::streamsize>( storedSize ) );
        if ( !m_Out )
        {
            // Sticky, so the failure cannot be walked past. An archive that lost one blob must not go
            // on collecting entries for an index it can never make true; every later call now refuses
            // and Finalize returns 0.
            m_Ok = false;
            return false;
        }

        // Two values over two different buffers, and the difference is the whole v3 design: the HASH
        // is of the content (identity, diffed by releases), the CRC is of what actually lies on the
        // disk (integrity, checked at every read).
        m_Entries.push_back( { key, m_Cursor, storedSize, static_cast<uint64_t>( size ),
                               PakContentHash( data, size ), Crc32c( stored, storedSize ), codec } );
        m_Cursor += storedSize;
        return true;
    }

    bool PakWriter::AddData( const std::string& key, const void* data, size_t size )
    {
        if ( key == kDeletedEntriesKey )
            return false; // reserved: SetDeletedKeys owns it

        // A KEY IS WRITTEN ONCE. Nothing checked this before, and the writer would happily store two
        // blobs under one name: the index then carries both, a lookup returns whichever the search
        // reaches first, and WHICH ONE that is depends on insertion order — so the same inputs can
        // produce an archive that serves different bytes than the last pack did, with nothing anywhere
        // saying so. Refusing by name is the only point at which both entries are still visible.
        //
        // Found on Windows CI: a corpus round-trip packed 43 entries for 42 distinct keys, because two
        // files in different directories share a filename and the caller keyed on the filename alone.
        // The count mismatch was the only symptom, and it was three layers away from the cause.
        if ( std::find_if( m_Entries.begin(), m_Entries.end(),
                           [&key]( const Entry& e ) { return e.Key == key; } ) != m_Entries.end() )
        {
            LOG_ERROR( "[Pak] {}: '{}' is already in this archive. A key names one blob; packing a "
                       "second under the same name would make which bytes a reader gets depend on "
                       "search order.",
                       m_Path.string(), key );
            return false;
        }
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
            WritePod<uint64_t>( m_Out, entry.StoredSize );
            WritePod<uint64_t>( m_Out, entry.Size );
            WritePod<uint64_t>( m_Out, entry.Hash );
            WritePod<uint32_t>( m_Out, entry.Crc );
            WritePod<uint32_t>( m_Out, static_cast<uint32_t>( entry.Codec ) );
        }

        // THE HEADER IS THE LAST THING WRITTEN, and on an append it is the ONLY thing in the file that
        // changes. Everything above went after the old index, so until these sixteen bytes land the
        // file still describes exactly the archive it described before — which is what makes an
        // interrupted append cost nothing and need no rollback. Flushed before the seek so the
        // process cannot hand the operating system a header that names an index it has not yet given
        // it. (Against a power cut that ordering is the device's to keep, not ours; what survives
        // there is the other half of the design — a torn append is caught at open, by name, because
        // the index it points at is short, or at read, by name, because an entry's CRC does not hold.)
        m_Out.flush();
        m_Out.seekp( 0 );
        m_Out.write( kMagicV3, 4 );
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
        const bool            v3        = in && std::memcmp( magic, kMagicV3, 4 ) == 0;
        if ( !v1 && !v2 && !v3 )
        {
            // A VERSION THIS BUILD DOES NOT KNOW IS REFUSED BY ITS NAME, not as "corrupt". The two
            // have opposite remedies — one is "this game is older than this content", the other is
            // "download it again" — and the magic is the only place the difference is legible. The
            // check is the three-character prefix, so every future version is named by the build that
            // predates it instead of being reported as a file that is not an archive at all.
            if ( magicRead == 4 && std::memcmp( magic, kMagicPrefix, 3 ) == 0 )
            {
                m_OpenError = fmt::format( "this is a {} archive and this build reads \"DPK1\", \"DPK2\" "
                                           "and \"DPK3\" — the content is newer than the program "
                                           "reading it (the file is {} bytes)",
                                           QuoteMagic( magic, magicRead ), bytes );
                return;
            }
            m_OpenError = fmt::format( "not a Desert archive: it begins with {}, expected \"DPK1\", "
                                       "\"DPK2\" or \"DPK3\" (the file is {} bytes)",
                                       QuoteMagic( magic, magicRead ), bytes );
            return;
        }
        m_Version = v3 ? PakVersion::V3 : ( v2 ? PakVersion::V2 : PakVersion::V1 );

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
            // ONE BRANCH PER VERSION, reading the columns that version has in the order Finalize
            // writes them. An earlier draft read the shared prefix once and then patched the fields
            // that moved, which made a v3 record's THIRD u64 arrive in a variable named Hash — true,
            // and unreadable. A format with three versions is three record layouts; spelling them out
            // is what makes adding a fourth a visible edit.
            bool complete = static_cast<bool>( in ) && ReadPod( in, span.Offset );
            if ( complete && v3 )
            {
                uint32_t codec = 0;
                complete       = ReadPod( in, span.StoredSize ) && ReadPod( in, span.Size ) &&
                           ReadPod( in, span.Hash ) && ReadPod( in, span.Crc ) && ReadPod( in, codec );
                if ( complete )
                {
                    if ( codec > static_cast<uint32_t>( PakCodec::LZ4 ) )
                    {
                        // A codec this build cannot run is named, for the same reason the magic is:
                        // the remedy is a newer build, not a repaired download.
                        m_OpenError = fmt::format( "index entry {} of {} ('{}') is stored with codec {}, "
                                                   "which this build does not implement",
                                                   i + 1, entryCount, key, codec );
                        return;
                    }
                    span.Codec = static_cast<PakCodec>( codec );
                }
            }
            else if ( complete )
            {
                // v1 and v2 have ONE size column, and it means both things at once: nothing before v3
                // is compressed, so the stored bytes are the content.
                complete  = ReadPod( in, span.StoredSize ) && ( !v2 || ReadPod( in, span.Hash ) );
                span.Size = span.StoredSize;
            }
            if ( !complete )
            {
                m_OpenError = fmt::format( "index entry {} of {} is cut short — the index is truncated", i + 1,
                                           entryCount );
                return;
            }
            if ( span.Codec == PakCodec::Store && span.Size != span.StoredSize )
            {
                // A stored entry whose two sizes disagree is an index that contradicts itself, and
                // believing either column would hand the caller a buffer of the wrong length.
                m_OpenError = fmt::format( "index entry {} of {} ('{}') is stored uncompressed but declares "
                                           "{} content bytes in {} stored bytes — the index is corrupt",
                                           i + 1, entryCount, key, span.Size, span.StoredSize );
                return;
            }
            // A span that leaves the data region would read the index back as file content, or run off
            // the end of the file. Checked HERE rather than at the read, because one bad span means the
            // index itself is damaged and nothing in this archive can be trusted.
            if ( span.Offset < kHeaderSize || span.StoredSize > indexOffset ||
                 span.Offset > indexOffset - span.StoredSize )
            {
                m_OpenError = fmt::format( "index entry {} of {} ('{}') points at bytes {}..{}, outside the "
                                           "{}-byte content region — the index is corrupt",
                                           i + 1, entryCount, key, span.Offset, span.Offset + span.StoredSize,
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

    PakVersion PakReader::Version() const
    {
        return m_Version;
    }

    const std::filesystem::path& PakReader::ArchivePath() const
    {
        return m_Path;
    }

    // One lookup, one refusal of the reserved key, and each accessor names the column it wants. Six
    // copies of the same five lines was the alternative, and the copy that drifts is the one nobody
    // reads again.
    template <typename T>
    std::optional<T> PakReader::Column( const std::string& key, T Span::*column ) const
    {
        if ( key == kDeletedEntriesKey )
            return std::nullopt;
        const auto it = m_Index.find( key );
        if ( it == m_Index.end() )
            return std::nullopt;
        return it->second.*column;
    }

    std::optional<uint64_t> PakReader::EntrySize( const std::string& key ) const
    {
        return Column( key, &Span::Size );
    }

    std::optional<uint64_t> PakReader::EntryStoredSize( const std::string& key ) const
    {
        return Column( key, &Span::StoredSize );
    }

    std::optional<PakCodec> PakReader::EntryCodec( const std::string& key ) const
    {
        return Column( key, &Span::Codec );
    }

    std::optional<uint64_t> PakReader::EntryHash( const std::string& key ) const
    {
        return Column( key, &Span::Hash );
    }

    std::optional<uint32_t> PakReader::EntryCrc( const std::string& key ) const
    {
        return Column( key, &Span::Crc );
    }

    std::optional<uint64_t> PakReader::EntryOffset( const std::string& key ) const
    {
        return Column( key, &Span::Offset );
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

        std::string stored( static_cast<size_t>( it->second.StoredSize ), '\0' );
        in.seekg( static_cast<std::streamoff>( it->second.Offset ) );
        in.read( stored.data(), static_cast<std::streamsize>( it->second.StoredSize ) );
        if ( !in )
        {
            LOG_ERROR( "[Pak] {}: entry '{}' could not be read ({} bytes at offset {})", m_Path.string(), key,
                       it->second.StoredSize, it->second.Offset );
            return std::nullopt;
        }

        // THE ENTRY IS VERIFIED BEFORE ANYTHING IS DONE WITH IT, and the failure names which check it
        // was. Before v3 the archive had NOTHING to verify against and the reader said nothing about
        // it either — a flipped bit anywhere in the data region left the header, the index and every
        // span perfectly valid, so the archive mounted, the read succeeded, and the game ran on
        // corrupt content without one line anywhere.
        //
        // WHAT IT COSTS, measured rather than assumed (2026-09-22, -O2, 256 MiB, 5 repeats, this
        // machine): CRC-32C runs at 8.17 GB/s against FNV-1a's 0.77, which took the whole-archive
        // verification of the shipping cooked tree from 297.4 ms to ~28 ms and moved it from four
        // fifths of the read path to a twelfth of it. Crc32c.hpp carries what the change did to what
        // is DETECTED, which is the half of this that is not about speed.
        if ( m_Version == PakVersion::V3 )
        {
            const uint32_t actual = Crc32c( stored.data(), stored.size() );
            if ( actual != it->second.Crc )
            {
                LOG_ERROR( "[Pak] {}: entry '{}' is CORRUPT — stored CRC32C {:#010x}, index says {:#010x} "
                           "({} bytes at offset {}). The archive is damaged or was modified after packing.",
                           m_Path.string(), key, actual, it->second.Crc, it->second.StoredSize,
                           it->second.Offset );
                return std::nullopt;
            }
        }
        else if ( m_Version == PakVersion::V2 )
        {
            // A v2 archive carries only the content hash, so that is what it is checked with — at v2's
            // price. Kept rather than dropped because an archive already on a disk somewhere must not
            // become LESS checked by the arrival of a newer format.
            const uint64_t actual = PakContentHash( stored.data(), stored.size() );
            if ( actual != it->second.Hash )
            {
                LOG_ERROR( "[Pak] {}: entry '{}' is CORRUPT — content hash {:#x}, index says {:#x} ({} "
                           "bytes at offset {}). The archive is damaged or was modified after packing.",
                           m_Path.string(), key, actual, it->second.Hash, it->second.Size, it->second.Offset );
                return std::nullopt;
            }
        }

        if ( it->second.Codec == PakCodec::Store )
            return stored;

        std::string data( static_cast<size_t>( it->second.Size ), '\0' );
        if ( !Lz4BlockDecompress( stored.data(), stored.size(), data.data(), data.size() ) )
        {
            // Reachable only when the stored bytes are exactly what was packed (the CRC above already
            // said so) and still do not decode — i.e. the packer and the reader disagree about the
            // format. A refusal, not a truncated buffer: half an asset is worse than none.
            LOG_ERROR( "[Pak] {}: entry '{}' passed its CRC but does not decode — {} stored bytes were "
                       "packed as codec {} and should yield {} bytes",
                       m_Path.string(), key, it->second.StoredSize, static_cast<uint32_t>( it->second.Codec ),
                       it->second.Size );
            return std::nullopt;
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
