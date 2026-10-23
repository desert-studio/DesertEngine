#include "ContentManifest.hpp"

#include "PakFile.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <format>
#include <fstream>
#include <system_error>

namespace Common::Utils
{
    namespace
    {
        constexpr std::string_view kMagic         = "DesertContentManifest";
        constexpr int              kFormatVersion = 1;

        // A key has to survive a round trip through one line of text. Newlines are legal in POSIX
        // filenames, and a key carrying one would split into two records that both parse — a corrupt
        // manifest that reads as a valid one. Carriage return is refused with it so a manifest written
        // on one platform and parsed on the other cannot lose a character off the end of a key.
        bool KeyIsSpellable( std::string_view key )
        {
            return !key.empty() && key.find( '\n' ) == std::string_view::npos &&
                   key.find( '\r' ) == std::string_view::npos;
        }

        std::string HexU64( uint64_t value )
        {
            std::string out( 16, '0' );
            for ( int i = 15; i >= 0; --i )
            {
                out[static_cast<size_t>( i )] = "0123456789abcdef"[value & 0xful];
                value >>= 4;
            }
            return out;
        }
    } // namespace

    void ContentManifest::Insert( ContentManifestEntry entry )
    {
        const auto at = std::lower_bound( m_Entries.begin(), m_Entries.end(), entry.Key,
                                          []( const ContentManifestEntry& e, const std::string& key )
                                          { return e.Key < key; } );
        if ( at != m_Entries.end() && at->Key == entry.Key )
            *at = std::move( entry ); // last writer wins; the key is the identity
        else
            m_Entries.insert( at, std::move( entry ) );
    }

    bool ContentManifest::Remove( const std::string& key )
    {
        const auto at =
             std::lower_bound( m_Entries.begin(), m_Entries.end(), key,
                               []( const ContentManifestEntry& e, const std::string& k ) { return e.Key < k; } );
        if ( at == m_Entries.end() || at->Key != key )
            return false;
        m_Entries.erase( at );
        return true;
    }

    const std::vector<ContentManifestEntry>& ContentManifest::Entries() const
    {
        return m_Entries;
    }

    size_t ContentManifest::Count() const
    {
        return m_Entries.size();
    }

    bool ContentManifest::Empty() const
    {
        return m_Entries.empty();
    }

    const ContentManifestEntry* ContentManifest::Find( const std::string& key ) const
    {
        const auto at =
             std::lower_bound( m_Entries.begin(), m_Entries.end(), key,
                               []( const ContentManifestEntry& e, const std::string& k ) { return e.Key < k; } );
        return ( at != m_Entries.end() && at->Key == key ) ? &*at : nullptr;
    }

    Common::ResultStr<ContentManifest> ContentManifest::FromDirectory( const std::filesystem::path& root,
                                                                       const std::string&           keyPrefix )
    {
        std::error_code ec;
        if ( !std::filesystem::is_directory( root, ec ) )
            return Common::MakeFormattedError<ContentManifest>( "{} is not a directory", root.string() );

        ContentManifest manifest;
        for ( auto it = std::filesystem::recursive_directory_iterator( root, ec );
              it != std::filesystem::recursive_directory_iterator(); it.increment( ec ) )
        {
            if ( ec )
                return Common::MakeFormattedError<ContentManifest>( "{} could not be walked: {}", root.string(),
                                                                    ec.message() );
            if ( !it->is_regular_file( ec ) )
                continue;

            const std::filesystem::path rel = std::filesystem::relative( it->path(), root, ec );
            if ( ec )
                return Common::MakeFormattedError<ContentManifest>(
                     "{} could not be made relative to {}: {}", it->path().string(), root.string(), ec.message() );

            std::string key = rel.generic_string();
            if ( !keyPrefix.empty() )
                key = keyPrefix + "/" + key;
            if ( !KeyIsSpellable( key ) )
                return Common::MakeFormattedError<ContentManifest>(
                     "{} cannot be recorded: a manifest key may not contain a line break", it->path().string() );

            std::ifstream in( it->path(), std::ios::binary );
            if ( !in )
                return Common::MakeFormattedError<ContentManifest>( "{} could not be opened for reading",
                                                                    it->path().string() );
            const std::string data( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
            if ( !in && !in.eof() )
                return Common::MakeFormattedError<ContentManifest>( "{} could not be read to the end",
                                                                    it->path().string() );

            manifest.Insert( { std::move( key ), static_cast<uint64_t>( data.size() ),
                               PakContentHash( data.data(), data.size() ) } );
        }
        return Common::MakeSuccess( std::move( manifest ) );
    }

    ContentManifest ContentManifest::FromPak( const PakReader& pak )
    {
        ContentManifest manifest;
        // KeysWithPrefix already hides the reserved deletion-list entry: a patch's list of removals is
        // not content and must not appear in a record of what the source handed over.
        for ( const auto& key : pak.KeysWithPrefix( "" ) )
            manifest.Insert( { key, pak.EntrySize( key ).value_or( 0 ), pak.EntryHash( key ).value_or( 0 ) } );
        return manifest;
    }

    std::string ContentManifest::Serialize() const
    {
        std::string out;
        out.reserve( m_Entries.size() * 48 + 32 );
        out += kMagic;
        out += ' ';
        out += std::to_string( kFormatVersion );
        out += '\n';
        for ( const auto& entry : m_Entries )
        {
            out += HexU64( entry.Hash );
            out += ' ';
            out += std::to_string( entry.Size );
            out += ' ';
            out += entry.Key;
            out += '\n';
        }
        return out;
    }

    Common::ResultStr<ContentManifest> ContentManifest::Parse( std::string_view text )
    {
        size_t lineStart = 0;
        size_t lineNo    = 0;

        auto nextLine = [&]( std::string_view& line ) -> bool
        {
            if ( lineStart >= text.size() )
                return false;
            const size_t end = text.find( '\n', lineStart );
            line = text.substr( lineStart, ( end == std::string_view::npos ? text.size() : end ) - lineStart );
            lineStart = ( end == std::string_view::npos ) ? text.size() : end + 1;
            if ( !line.empty() && line.back() == '\r' )
                line.remove_suffix( 1 );
            ++lineNo;
            return true;
        };

        std::string_view header;
        if ( !nextLine( header ) )
            return Common::MakeError<ContentManifest>( "the manifest is empty — its header line is missing" );
        const std::string expected = std::format( "{} {}", kMagic, kFormatVersion );
        if ( header != expected )
            return Common::MakeFormattedError<ContentManifest>(
                 "not a Desert content manifest: line 1 is \"{}\", expected \"{}\"", std::string( header ),
                 expected );

        ContentManifest  manifest;
        std::string_view line;
        while ( nextLine( line ) )
        {
            if ( line.empty() )
                continue;

            const size_t hashEnd = line.find( ' ' );
            if ( hashEnd == std::string_view::npos )
                return Common::MakeFormattedError<ContentManifest>(
                     "line {} has no size column — expected \"<hash> <size> <key>\"", lineNo );
            const size_t sizeEnd = line.find( ' ', hashEnd + 1 );
            if ( sizeEnd == std::string_view::npos )
                return Common::MakeFormattedError<ContentManifest>(
                     "line {} has no key column — expected \"<hash> <size> <key>\"", lineNo );

            const std::string_view hashText = line.substr( 0, hashEnd );
            const std::string_view sizeText = line.substr( hashEnd + 1, sizeEnd - hashEnd - 1 );
            const std::string_view keyText  = line.substr( sizeEnd + 1 );

            ContentManifestEntry entry;
            if ( std::from_chars( hashText.data(), hashText.data() + hashText.size(), entry.Hash, 16 ).ec !=
                 std::errc{} )
                return Common::MakeFormattedError<ContentManifest>( "line {}: \"{}\" is not a 64-bit hex hash",
                                                                    lineNo, std::string( hashText ) );
            if ( std::from_chars( sizeText.data(), sizeText.data() + sizeText.size(), entry.Size ).ec !=
                 std::errc{} )
                return Common::MakeFormattedError<ContentManifest>( "line {}: \"{}\" is not a byte count", lineNo,
                                                                    std::string( sizeText ) );
            if ( !KeyIsSpellable( keyText ) )
                return Common::MakeFormattedError<ContentManifest>( "line {}: the key is empty", lineNo );

            entry.Key = std::string( keyText );
            manifest.Insert( std::move( entry ) );
        }
        return Common::MakeSuccess( std::move( manifest ) );
    }

    bool ContentDiff::Empty() const
    {
        return Added.empty() && Changed.empty() && Removed.empty();
    }

    ContentDiff CompareManifests( const ContentManifest& from, const ContentManifest& to )
    {
        // Both sides are sorted by key, so this is one merge walk rather than a lookup per entry.
        ContentDiff diff;
        auto        a    = from.Entries().begin();
        auto        b    = to.Entries().begin();
        const auto  aEnd = from.Entries().end();
        const auto  bEnd = to.Entries().end();

        while ( a != aEnd || b != bEnd )
        {
            if ( b == bEnd || ( a != aEnd && a->Key < b->Key ) )
            {
                diff.Removed.push_back( a->Key );
                ++a;
            }
            else if ( a == aEnd || b->Key < a->Key )
            {
                diff.Added.push_back( b->Key );
                ++b;
            }
            else
            {
                // Size is compared alongside the hash on purpose: it is free, it is already recorded,
                // and it turns the one shape a 64-bit non-cryptographic hash cannot rule out — a
                // collision — from "reported identical" into "reported changed". Wrong in the harmless
                // direction rather than the silent one.
                if ( a->Hash != b->Hash || a->Size != b->Size )
                    diff.Changed.push_back( a->Key );
                ++a;
                ++b;
            }
        }
        return diff;
    }

    Common::ResultStr<PatchBuild> BuildPatchPak( const std::filesystem::path& baseManifestFile,
                                                 const std::filesystem::path& newerPak,
                                                 const std::filesystem::path& outPatch )
    {
        std::ifstream in( baseManifestFile, std::ios::binary );
        if ( !in )
        {
            // The reason, not just the path. This refusal is what a release engineer reads when a patch
            // will not build, and "cannot open" answers the question with the question — the same
            // correction PakTool's `list` already carries for an archive.
            const std::error_code ec( errno, std::generic_category() );
            return Common::MakeFormattedError<PatchBuild>(
                 "the base manifest {} could not be read ({}). A patch needs the manifest of the release "
                 "it updates; without it there is no \"before\" side to compare against, and it cannot be "
                 "reconstructed after the fact — it is recorded when that release is packaged.",
                 baseManifestFile.string(), ec.message() );
        }
        const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );

        auto parsed = ContentManifest::Parse( text );
        if ( !parsed )
            return Common::MakeFormattedError<PatchBuild>( "the base manifest {} is not a manifest: {}",
                                                           baseManifestFile.string(), parsed.GetError() );
        const ContentManifest base = parsed.ExtractValue();

        PakReader newer( newerPak );
        if ( !newer.IsOpen() )
            return Common::MakeFormattedError<PatchBuild>( "the new release archive {} could not be opened: {}",
                                                           newerPak.string(), newer.OpenError() );

        PatchBuild built;
        built.Diff = CompareManifests( base, ContentManifest::FromPak( newer ) );
        if ( built.Diff.Empty() )
        {
            // No archive, and the caller is TOLD that rather than left to find an empty file — or, as
            // before, a stale one from a previous run that a `remove` may or may not have reached.
            std::error_code ec;
            std::filesystem::remove( outPatch, ec );
            return Common::MakeSuccess( std::move( built ) );
        }

        // The writing is done in its own scope so that a failure part-way through can DELETE what it
        // started, and delete it after the writer has closed the handle (removing an open file is legal
        // on POSIX and refused on Windows). A half-written .dpak left behind by a refusal is the worst
        // artifact this function could produce: it has a patch's name, a patch's extension and some of a
        // patch's contents, and the archive it does not finish is the one a player would mount.
        std::string failure;
        {
            PakWriter writer( outPatch );
            if ( !writer.IsOpen() )
                return Common::MakeFormattedError<PatchBuild>( "the patch archive {} could not be created",
                                                               outPatch.string() );

            for ( const auto* keys : { &built.Diff.Added, &built.Diff.Changed } )
                for ( const auto& key : *keys )
                {
                    if ( !failure.empty() )
                        break;
                    const auto data = newer.Read( key );
                    if ( !data )
                        failure = "entry '" + key + "' could not be read out of " + newerPak.string() +
                                  " — the patch would be missing a file it says it delivers";
                    else if ( !writer.AddData( key, data->data(), data->size() ) )
                        failure = "entry '" + key + "' could not be written into " + outPatch.string();
                }

            if ( failure.empty() && !writer.SetDeletedKeys( built.Diff.Removed ) )
                failure = "a deleted key cannot be recorded (empty, reserved, or containing a line break)";

            if ( failure.empty() && writer.Finalize() == 0 )
                failure = "the patch archive " + outPatch.string() + " could not be finalized";
        }

        if ( !failure.empty() )
        {
            std::error_code ec;
            std::filesystem::remove( outPatch, ec );
            return Common::MakeFormattedError<PatchBuild>( "{}", failure );
        }

        built.Written = true;
        return Common::MakeSuccess( std::move( built ) );
    }
} // namespace Common::Utils
