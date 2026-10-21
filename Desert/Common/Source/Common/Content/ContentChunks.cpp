#include "ContentChunks.hpp"

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/PakFile.hpp>
#include <Common/Utilities/VFS.hpp>

#include <Common/Json/Json.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
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

        // The manifest's mark for a region chunk mounted on demand (WP23).
        constexpr std::string_view kOnDemandPrefix = "OnDemand ";

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

    namespace
    {
        ChunkSchemeJson ToJson( const ChunkScheme& scheme )
        {
            ChunkSchemeJson out;
            out.AlwaysBase = scheme.AlwaysBase;
            out.Chunks.reserve( scheme.Chunks.size() );
            for ( const auto& rule : scheme.Chunks )
                out.Chunks.push_back( ChunkRuleJson{ rule.Name, rule.Roots } );
            return out;
        }
    } // namespace

    std::string WriteChunkScheme( const ChunkScheme& scheme )
    {
        return Json::Write( ToJson( scheme ) );
    }

    fs::path ChunkSchemePath()
    {
        // Beside the project's own content roots rather than inside them: the scheme describes how
        // content is DIVIDED and is not itself content, so a scan that enumerated it would offer a
        // build setting in an asset picker.
        return Constants::Path::CurrentProjectRoot().ProjectDir / "ContentChunks.json";
    }

    ResultStr<ChunkScheme> LoadChunkScheme( const fs::path& path )
    {
        if ( !Utils::FileSystem::Exists( path ) )
            return MakeFormattedError<ChunkScheme>(
                 "no chunk scheme at {} — packaging needs the project to state how its content is divided. "
                 "Create the default one-archive scheme (Build Settings: \"Create default ContentChunks.json\", "
                 "or the "
                 "palette command Build > Create Default ContentChunks.json) and edit it to add chunks",
                 path.string() );
        const auto text = Utils::FileSystem::ReadFileContent( path );
        if ( !text )
            return MakeFormattedError<ChunkScheme>( "{} exists but could not be read: {}", path.string(),
                                                    text.GetError() );
        auto parsed = ParseChunkScheme( text.GetValue() );
        if ( !parsed )
            return MakeFormattedError<ChunkScheme>( "{}: {}", path.string(), parsed.GetError() );
        return parsed;
    }

    BoolResultStr WriteDefaultChunkScheme( const fs::path& path )
    {
        if ( Utils::FileSystem::Exists( path ) )
            return MakeFormattedError<bool>( "{} already exists; the default scheme never replaces one",
                                             path.string() );
        // Through the facade's writer: the file is committed and hand-edited, so it gets the canonical
        // layout every other text asset has, not rfl's single line.
        const auto written = Json::WriteFileAtomic( path, ToJson( ChunkScheme{} ) );
        if ( !written )
            return MakeFormattedError<bool>( "could not write the default chunk scheme to {}: {}", path.string(),
                                             written.GetError() );
        return MakeSuccess( true );
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

    std::optional<std::size_t> ChunkPlan::FileChunk( std::string_view archiveKey ) const
    {
        const auto it = m_FileChunk.find( std::string( archiveKey ) );
        if ( it == m_FileChunk.end() )
            return std::nullopt;
        return it->second;
    }

    bool ChunkPlan::OnDemand( std::size_t chunk ) const
    {
        return chunk < m_OnDemand.size() && m_OnDemand[chunk];
    }

    BoolResultStr ValidateChunkScheme( const ChunkScheme& scheme )
    {
        std::vector<std::string_view> names{ BASE_CHUNK_NAME };
        for ( const ChunkRule& rule : scheme.Chunks )
        {
            if ( !IsNameableChunk( rule.Name ) )
                return MakeFormattedError<bool>(
                     "chunk name '{}' cannot be part of an archive filename — letters, digits, '_' and "
                     "'-' only",
                     rule.Name );
            if ( rule.Name == BASE_CHUNK_NAME )
                return MakeFormattedError<bool>(
                     "'{}' is the archive every unplaced file ships in and is not something a scheme "
                     "declares",
                     BASE_CHUNK_NAME );
            if ( std::find( names.begin(), names.end(), rule.Name ) != names.end() )
                return MakeFormattedError<bool>( "two chunks are called '{}'", rule.Name );
            if ( rule.Roots.empty() )
                return MakeFormattedError<bool>(
                     "chunk '{}' names no roots, so it would ship an empty archive that reads exactly "
                     "like a correct one",
                     rule.Name );
            names.push_back( rule.Name );
        }
        return MakeSuccess( true );
    }

    BoolResultStr SaveChunkScheme( const fs::path& path, const ChunkScheme& scheme,
                                   const Utils::AssetRegistry& registry )
    {
        // The SAME derivation the packager runs, so a scheme the panel accepted is a scheme Build
        // accepts: every refusal BuildChunkPlan knows (names, roots, pins against the registry) is a
        // refusal here, and nothing is written.
        if ( const auto plan = BuildChunkPlan( registry, scheme ); !plan )
            return MakeFormattedError<bool>( "{} was not saved: {}", path.string(), plan.GetError() );
        const auto written = Json::WriteFileAtomic( path, ToJson( scheme ) );
        if ( !written )
            return MakeFormattedError<bool>( "could not write the chunk scheme to {}: {}", path.string(),
                                             written.GetError() );
        return MakeSuccess( true );
    }

    std::vector<ChunkFolderRow> SummarizeChunkFolders( const Utils::AssetRegistry& registry,
                                                       const ChunkPlan&            plan )
    {
        std::vector<ChunkFolderRow> rows;
        for ( const Utils::AssetRegistryEntry& entry : registry.Entries() )
        {
            // The folder is the key up to its last '/', scheme prefix included ("assets:Materials"), so
            // two content roots with one folder name stay two rows.
            const std::size_t slash = entry.Key.rfind( '/' );
            std::string folder      = slash == std::string::npos ? entry.Key.substr( 0, entry.Key.find( ':' ) + 1 )
                                                                 : entry.Key.substr( 0, slash );
            auto        it          = std::find_if( rows.begin(), rows.end(),
                                                    [&]( const ChunkFolderRow& row ) { return row.Folder == folder; } );
            if ( it == rows.end() )
            {
                rows.push_back(
                     ChunkFolderRow{ std::move( folder ), std::vector<std::size_t>( plan.Count(), 0 ) } );
                it = std::prev( rows.end() );
            }
            // THE PACKAGER'S OWN QUESTION, asked of the same plan: WriteChunkedPaks files every source
            // under ChunkFor( its stable key ). A second copy of the rule here would be a panel that
            // can disagree with the archives.
            ++it->FilesPerChunk[plan.ChunkFor( entry.Key )];
        }
        std::sort( rows.begin(), rows.end(),
                   []( const ChunkFolderRow& a, const ChunkFolderRow& b ) { return a.Folder < b.Folder; } );
        return rows;
    }

    ResultStr<ChunkPlan> BuildChunkPlan( const Utils::AssetRegistry& registry, const ChunkScheme& scheme )
    {
        return BuildChunkPlan( registry, scheme, {} );
    }

    ResultStr<ChunkPlan> BuildChunkPlan( const Utils::AssetRegistry& registry, const ChunkScheme& scheme,
                                         std::span<const RegionChunkGroup> worlds )
    {
        ChunkPlan plan;

        // ── 1. The names, validated before anything is walked ────────────────────────────────────
        if ( const auto valid = ValidateChunkScheme( scheme ); !valid )
            return MakeError<ChunkPlan>( valid.GetError() );

        // Every chunk past the base: its rule (name + roots) and the world group it belongs to (none for a
        // scheme chunk). A world's chunks are derived, so only the name checks apply to them.
        constexpr std::size_t kNoGroup = static_cast<std::size_t>( -1 );
        std::vector<ChunkRule>   rules;
        std::vector<std::size_t> groupOf{ kNoGroup }; // index-matched to m_Names
        std::vector<std::size_t> sharedOf;            // per group: its Shared chunk's index
        for ( const ChunkRule& rule : scheme.Chunks )
        {
            plan.m_Names.push_back( rule.Name );
            plan.m_OnDemand.push_back( false );
            groupOf.push_back( kNoGroup );
            rules.push_back( rule );
        }
        auto addWorldChunk = [&]( ChunkRule rule, std::size_t group, bool onDemand ) -> BoolResultStr {
            if ( !IsNameableChunk( rule.Name ) || rule.Name == BASE_CHUNK_NAME )
                return MakeFormattedError<bool>( "world chunk name '{}' cannot be a chunk archive name",
                                                 rule.Name );
            if ( std::find( plan.m_Names.begin(), plan.m_Names.end(), rule.Name ) != plan.m_Names.end() )
                return MakeFormattedError<bool>( "two chunks are called '{}'", rule.Name );
            plan.m_Names.push_back( rule.Name );
            plan.m_OnDemand.push_back( onDemand );
            groupOf.push_back( group );
            rules.push_back( std::move( rule ) );
            return MakeSuccess( true );
        };
        for ( std::size_t group = 0; group < worlds.size(); ++group )
        {
            const RegionChunkGroup& world = worlds[group];
            sharedOf.push_back( plan.m_Names.size() );
            if ( auto added = addWorldChunk( ChunkRule{ world.Shared, world.SharedRoots }, group, false ); !added )
                return MakeError<ChunkPlan>( added.GetError() );
            for ( const ChunkRule& region : world.Regions )
                if ( auto added = addWorldChunk( region, group, true ); !added )
                    return MakeError<ChunkPlan>( added.GetError() );
        }
        for ( const RegionChunkGroup& world : worlds )
            for ( const auto& [key, chunkName] : world.Files )
            {
                const auto named = std::find( plan.m_Names.begin(), plan.m_Names.end(), chunkName );
                if ( named == plan.m_Names.end() )
                    return MakeFormattedError<ChunkPlan>( "world file '{}' is placed in chunk '{}', which no "
                                                          "world declares",
                                                          key, chunkName );
                plan.m_FileChunk[key] = static_cast<std::size_t>( named - plan.m_Names.begin() );
            }

        // ── 2. The closure, per chunk, over the registry's OWN edges ─────────────────────────────
        //
        // Reachability and not a member list: this is the same trace AssetEviction runs from a root
        // set, against the cooked registry instead of a live scene.
        std::unordered_map<std::string, std::vector<std::size_t>> reachedBy;
        std::unordered_set<uint64_t>                              unresolved;

        for ( std::size_t chunk = 1; chunk < plan.m_Names.size(); ++chunk )
        {
            const ChunkRule& rule = rules[chunk - 1];

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

        // ── 3. One chunk each; shared assets fall back to the base, or to their world's Shared ───
        for ( const auto& [key, chunks] : reachedBy )
        {
            if ( chunks.size() == 1 )
            {
                plan.m_Placed[key] = chunks.front();
                continue;
            }
            const std::size_t group = groupOf[chunks.front()];
            const bool        oneWorld =
                 group != kNoGroup && std::all_of( chunks.begin(), chunks.end(),
                                                   [&]( std::size_t c ) { return groupOf[c] == group; } );
            if ( oneWorld )
                plan.m_Placed[key] = sharedOf[group];
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
            text += ( plan.OnDemand( i ) ? std::string( kOnDemandPrefix ) : std::string() ) + plan.Names()[i] + "\n";
        return text;
    }

    ResultStr<ChunkManifest> ParseChunkManifest( std::string_view text )
    {
        ChunkManifest manifest;
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
            const bool onDemand = line.starts_with( kOnDemandPrefix );
            if ( onDemand )
                line.remove_prefix( kOnDemandPrefix.size() );
            if ( !IsNameableChunk( line ) )
                return MakeFormattedError<ChunkManifest>(
                     "the chunk list names '{}', which cannot be an archive filename", std::string( line ) );
            ( onDemand ? manifest.OnDemand : manifest.Mounted ).emplace_back( line );
        }
        return MakeSuccess( std::move( manifest ) );
    }

    namespace
    {
        // The on-demand chunks the mounted base registered (RegisterOnDemandChunks): name -> archive path.
        std::mutex                                             s_OnDemandLock;
        std::unordered_map<std::string, std::filesystem::path> s_OnDemand;
        // Loaded cells holding each on-demand chunk, and the chunks mounted now (a hold whose load was cancelled
        // before it read anything holds a chunk that was never mounted).
        std::unordered_map<std::string, std::size_t> s_Holds;
        std::unordered_set<std::string>              s_MountedNow;
    } // namespace

    void RegisterOnDemandChunks( const fs::path& baseArchive, std::span<const std::string> names )
    {
        std::lock_guard lock( s_OnDemandLock );
        for ( const std::string& name : names )
        {
            const fs::path archive = ChunkArchivePath( baseArchive, name );
            Utils::VFS::DeferPak( archive );
            s_OnDemand[name] = archive;
        }
    }

    BoolResultStr MountChunkOnDemand( std::string_view name )
    {
        // Under the lock for the mount itself, so a release cannot unmount between the mount and its record.
        std::lock_guard lock( s_OnDemandLock );
        if ( s_OnDemand.empty() )
            return MakeSuccess( true ); // nothing registered: editor or loose cooked files
        const std::string key( name );
        const auto        it = s_OnDemand.find( key );
        if ( it == s_OnDemand.end() )
            return MakeFormattedError<bool>( "chunk '{}' is not an on-demand chunk of the mounted game", key );
        if ( const auto held = s_Holds.find( key ); held == s_Holds.end() || held->second == 0 )
            return MakeFormattedError<bool>( "chunk '{}' is read with no loaded cell holding it", key );
        if ( auto mounted = Utils::VFS::MountDeferred( it->second ); !mounted )
            return mounted;
        s_MountedNow.insert( key );
        return MakeSuccess( true );
    }

    BoolResultStr HoldChunkOnDemand( std::string_view name )
    {
        std::lock_guard lock( s_OnDemandLock );
        if ( s_OnDemand.empty() )
            return MakeSuccess( true );
        const std::string key( name );
        if ( !s_OnDemand.contains( key ) )
            return MakeFormattedError<bool>( "chunk '{}' is not an on-demand chunk of the mounted game", key );
        ++s_Holds[key];
        return MakeSuccess( true );
    }

    BoolResultStr ReleaseChunkOnDemand( std::string_view name )
    {
        std::lock_guard lock( s_OnDemandLock );
        if ( s_OnDemand.empty() )
            return MakeSuccess( true );
        const std::string key( name );
        const auto        held = s_Holds.find( key );
        if ( held == s_Holds.end() || held->second == 0 )
            return MakeFormattedError<bool>( "chunk '{}' released with no loaded cell holding it", key );
        if ( --held->second > 0 || !s_MountedNow.contains( key ) )
            return MakeSuccess( true );
        s_MountedNow.erase( key );
        return Utils::VFS::ReturnToDeferred( s_OnDemand.at( key ) );
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
            const std::size_t chunk = plan.FileChunk( key ).value_or( plan.ChunkFor( AssetHandle::StableKeyForPath( source ) ) );
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
