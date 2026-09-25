#include "ContentChunks.hpp"

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/PakFile.hpp>

#include <Common/Json/Json.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <memory>
#include <unordered_set>

namespace Common::Content
{
    namespace fs = std::filesystem;

    namespace
    {
        // The on-disk shape of the scheme. A struct of its own rather than reflecting ChunkScheme
        // directly, so the API type stays free of reflect-cpp's field names and a rename of either
        // side is a one-line translation instead of a silent format change.
        struct ChunkRuleJson
        {
            std::string              Name;
            std::vector<std::string> Roots;
        };

        struct ChunkSchemeJson
        {
            std::vector<ChunkRuleJson> Chunks;
            std::vector<std::string>   AlwaysBase;
        };

        // A chunk name becomes part of a FILENAME (ChunkArchivePath), so the characters that would
        // make it name a different file — or no file at all — are refused here rather than producing
        // an archive nobody can find. Letters, digits, '_' and '-' only.
        bool IsNameableChunk( std::string_view name )
        {
            if ( name.empty() )
                return false;
            return std::all_of( name.begin(), name.end(),
                                []( unsigned char c ) { return std::isalnum( c ) != 0 || c == '_' || c == '-'; } );
        }
    } // namespace

    ResultStr<ChunkScheme> ParseChunkScheme( std::string_view json )
    {
        // A scheme file that exists but holds nothing is not "undivided": it is a file that lost its
        // content, and reading it as an empty scheme would quietly package everything into one archive.
        // An undivided project says so on purpose: WriteChunkScheme of an empty ChunkScheme.
        const std::string text( json );
        if ( text.find_first_not_of( " \t\r\n" ) == std::string::npos )
            return MakeError<ChunkScheme>( "the chunk scheme is empty; an undivided project writes an empty "
                                           "'Chunks' list and an empty 'AlwaysBase' list, not an empty file" );

        const auto parsed = Json::Read<ChunkSchemeJson>( text );
        if ( !parsed )
            return MakeFormattedError<ChunkScheme>( "the chunk scheme could not be read: {}", parsed.GetError() );

        ChunkScheme scheme;
        scheme.AlwaysBase = parsed.GetValue().AlwaysBase;
        scheme.Chunks.reserve( parsed.GetValue().Chunks.size() );
        for ( const auto& rule : parsed.GetValue().Chunks )
            scheme.Chunks.push_back( ChunkRule{ rule.Name, rule.Roots } );
        return MakeSuccess( std::move( scheme ) );
    }

    std::string WriteChunkScheme( const ChunkScheme& scheme )
    {
        ChunkSchemeJson out;
        out.AlwaysBase = scheme.AlwaysBase;
        out.Chunks.reserve( scheme.Chunks.size() );
        for ( const auto& rule : scheme.Chunks )
            out.Chunks.push_back( ChunkRuleJson{ rule.Name, rule.Roots } );
        return Json::Write( out );
    }

    fs::path ChunkSchemePath()
    {
        // Beside the project's own content roots rather than inside them: the scheme describes how
        // content is DIVIDED and is not itself content, so a scan that enumerated it would offer a
        // build setting in an asset picker.
        return Constants::Path::CurrentProjectRoot().ProjectDir / "ContentChunks.json";
    }

    const std::vector<std::string>& ChunkPlan::Names() const
    {
        return m_Names;
    }

    std::size_t ChunkPlan::Count() const
    {
        return m_Names.size();
    }

    std::size_t ChunkPlan::ChunkFor( std::string_view stableKey ) const
    {
        // THE TOTALITY. A key nobody placed is in the base — see the header note; this line is the
        // difference between "content ships somewhere" and "content can silently ship nowhere".
        const auto it = m_Placed.find( std::string( stableKey ) );
        return it == m_Placed.end() ? BASE_CHUNK : it->second;
    }

    const std::unordered_map<std::string, std::size_t>& ChunkPlan::Placed() const
    {
        return m_Placed;
    }

    const std::vector<uint64_t>& ChunkPlan::UnresolvedEdges() const
    {
        return m_Unresolved;
    }

    ResultStr<ChunkPlan> BuildChunkPlan( const Utils::AssetRegistry& registry, const ChunkScheme& scheme )
    {
        ChunkPlan plan;

        // ── 1. The names, validated before anything is walked ────────────────────────────────────
        for ( const ChunkRule& rule : scheme.Chunks )
        {
            if ( !IsNameableChunk( rule.Name ) )
                return MakeFormattedError<ChunkPlan>(
                     "chunk name '{}' cannot be part of an archive filename — letters, digits, '_' and "
                     "'-' only",
                     rule.Name );
            if ( rule.Name == BASE_CHUNK_NAME )
                return MakeFormattedError<ChunkPlan>(
                     "'{}' is the archive every unplaced file ships in and is not something a scheme "
                     "declares",
                     BASE_CHUNK_NAME );
            if ( std::find( plan.m_Names.begin(), plan.m_Names.end(), rule.Name ) != plan.m_Names.end() )
                return MakeFormattedError<ChunkPlan>( "two chunks are called '{}'", rule.Name );
            if ( rule.Roots.empty() )
                return MakeFormattedError<ChunkPlan>(
                     "chunk '{}' names no roots, so it would ship an empty archive that reads exactly "
                     "like a correct one",
                     rule.Name );
            plan.m_Names.push_back( rule.Name );
        }

        // ── 2. The closure, per chunk, over the registry's OWN edges ─────────────────────────────
        //
        // Reachability and not a member list: this is the same trace AssetEviction runs from a root
        // set, against the cooked registry instead of a live scene.
        std::unordered_map<std::string, std::vector<std::size_t>> reachedBy;
        std::unordered_set<uint64_t>                              unresolved;

        for ( std::size_t chunk = 1; chunk < plan.m_Names.size(); ++chunk )
        {
            const ChunkRule& rule = scheme.Chunks[chunk - 1];

            std::deque<const Utils::AssetRegistryEntry*> frontier;
            std::unordered_set<std::string>              seen;

            for ( const std::string& root : rule.Roots )
            {
                const Utils::AssetRegistryEntry* entry = registry.FindByKey( root );
                if ( entry == nullptr )
                    return MakeFormattedError<ChunkPlan>(
                         "chunk '{}' is rooted at '{}', which is not in the asset registry — a chunk "
                         "rooted at nothing produces an empty archive and looks like a correct one",
                         rule.Name, root );
                if ( seen.insert( entry->Key ).second )
                    frontier.push_back( entry );
            }

            while ( !frontier.empty() )
            {
                const Utils::AssetRegistryEntry* entry = frontier.front();
                frontier.pop_front();
                reachedBy[entry->Key].push_back( chunk );

                for ( const uint64_t handle : entry->Dependencies )
                {
                    const Utils::AssetRegistryEntry* next = registry.FindByHandle( handle );
                    if ( next == nullptr )
                    {
                        // Reported, never dropped — see ChunkPlan::UnresolvedEdges.
                        unresolved.insert( handle );
                        continue;
                    }
                    if ( seen.insert( next->Key ).second )
                        frontier.push_back( next );
                }
            }
        }

        plan.m_Unresolved.assign( unresolved.begin(), unresolved.end() );
        std::sort( plan.m_Unresolved.begin(), plan.m_Unresolved.end() );

        // ── 3. One chunk each; shared assets fall back to the base ───────────────────────────────
        for ( const auto& [key, chunks] : reachedBy )
        {
            if ( chunks.size() == 1 )
                plan.m_Placed[key] = chunks.front();
        }

        // ── 4. The pins, applied LAST, because they are the exceptions ───────────────────────────
        for ( const std::string& key : scheme.AlwaysBase )
        {
            if ( registry.FindByKey( key ) == nullptr )
                return MakeFormattedError<ChunkPlan>(
                     "'{}' is pinned to the base archive but is not in the asset registry", key );
            plan.m_Placed.erase( key );
        }

        return MakeSuccess( std::move( plan ) );
    }

    fs::path ChunkArchivePath( const fs::path& baseArchive, std::string_view chunkName )
    {
        return baseArchive.parent_path() / ( "Chunk_" + std::string( chunkName ) + ".dpak" );
    }

    std::string WriteChunkManifest( const ChunkPlan& plan )
    {
        std::string text;
        for ( std::size_t i = 1; i < plan.Names().size(); ++i )
            text += plan.Names()[i] + "\n";
        return text;
    }

    ResultStr<std::vector<std::string>> ParseChunkManifest( std::string_view text )
    {
        std::vector<std::string> names;
        std::size_t              begin = 0;
        while ( begin < text.size() )
        {
            std::size_t end = text.find( '\n', begin );
            if ( end == std::string_view::npos )
                end = text.size();
            // Carriage returns: the manifest is written on one host and read on another, and a text
            // file that crosses that boundary acquires them. A name with a trailing '\r' names a
            // file that does not exist, and the refusal would arrive as "chunk missing".
            std::string_view line = text.substr( begin, end - begin );
            while ( !line.empty() && ( line.back() == '\r' || line.back() == ' ' ) )
                line.remove_suffix( 1 );
            begin = end + 1;
            if ( line.empty() )
                continue;
            if ( !IsNameableChunk( line ) )
                return MakeFormattedError<std::vector<std::string>>(
                     "the chunk list names '{}', which cannot be an archive filename", std::string( line ) );
            names.emplace_back( line );
        }
        return MakeSuccess( std::move( names ) );
    }

    ResultStr<ChunkedWriteStats>
    WriteChunkedPaks( const fs::path& baseArchive, const ChunkPlan& plan,
                      const std::vector<std::pair<std::string, fs::path>>&    files,
                      const std::vector<std::pair<std::string, std::string>>& baseBlobs )
    {
        ChunkedWriteStats stats;
        stats.Archives.resize( plan.Count() );
        stats.Entries.assign( plan.Count(), 0 );
        stats.Bytes.assign( plan.Count(), 0 );

        // unique_ptr because PakWriter owns an ofstream and is therefore not movable — a vector of
        // them could not be grown.
        std::vector<std::unique_ptr<Utils::PakWriter>> writers;
        for ( std::size_t i = 0; i < plan.Count(); ++i )
        {
            stats.Archives[i] = i == BASE_CHUNK ? baseArchive : ChunkArchivePath( baseArchive, plan.Names()[i] );
            writers.push_back( std::make_unique<Utils::PakWriter>( stats.Archives[i] ) );
            if ( !writers.back()->IsOpen() )
                return MakeFormattedError<ChunkedWriteStats>( "cannot create {}", stats.Archives[i].string() );
        }

        for ( const auto& [key, source] : files )
        {
            const std::size_t chunk = plan.ChunkFor( AssetHandle::StableKeyForPath( source ) );
            if ( !writers[chunk]->AddFile( key, source ) )
                return MakeFormattedError<ChunkedWriteStats>( "pak write failed for {} into {}", source.string(),
                                                              stats.Archives[chunk].string() );
            ++stats.Entries[chunk];
            std::error_code ec;
            const auto      size = fs::file_size( source, ec );
            if ( !ec )
                stats.Bytes[chunk] += size;
        }

        for ( const auto& [key, bytes] : baseBlobs )
        {
            if ( !writers[BASE_CHUNK]->AddData( key, bytes.data(), bytes.size() ) )
                return MakeFormattedError<ChunkedWriteStats>( "cannot write {} into {}", key,
                                                              stats.Archives[BASE_CHUNK].string() );
            ++stats.Entries[BASE_CHUNK];
        }

        // THE LIST TRAVELS WITH THE BASE. Written after the content so a caller that added nothing
        // still produces a base archive that declares its chunks — an archive whose manifest is
        // missing is indistinguishable from a game that was never divided.
        const std::string manifest = WriteChunkManifest( plan );
        if ( !writers[BASE_CHUNK]->AddData( std::string( CHUNK_MANIFEST_KEY ), manifest.data(), manifest.size() ) )
            return MakeFormattedError<ChunkedWriteStats>( "cannot write the chunk list into {}",
                                                          stats.Archives[BASE_CHUNK].string() );
        ++stats.Entries[BASE_CHUNK];

        for ( std::size_t i = 0; i < writers.size(); ++i )
        {
            // A CHUNK THAT RECEIVED NOTHING IS A REFUSAL, BY NAME. It is reachable — a chunk whose
            // every root is also reachable from another chunk loses all of them to the base — and an
            // empty archive is the one outcome that reads exactly like a correct small region. Saying
            // so here is the only place the roots and the packed file list are both in hand.
            if ( i != BASE_CHUNK && stats.Entries[i] == 0 )
                return MakeFormattedError<ChunkedWriteStats>(
                     "chunk '{}' received no files: every asset its roots reach is either shared with "
                     "another chunk or is not packaged",
                     plan.Names()[i] );

            // Finalize is the archive's ONLY verdict (PakFile.hpp) — a zero here means the bytes
            // never reached the disk, whatever the adds returned.
            if ( writers[i]->Finalize() == 0 )
                return MakeFormattedError<ChunkedWriteStats>( "failed to finalize {}",
                                                              stats.Archives[i].string() );
        }

        return MakeSuccess( std::move( stats ) );
    }
} // namespace Common::Content
