#include <Engine/Assets/ContentRegistry.hpp>

#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/AssetManager.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_set>

namespace Desert::Assets::ContentRegistry
{
    namespace
    {
        // A FUNCTION-LOCAL STATIC, for `AssetPathIndex`'s reason: `NoteAsset` is reachable from
        // `AssetManager::CreateAsset`, which a translation unit's static initialiser can reach, and a
        // namespace-scope object would then be read before its own constructor ran.
        //
        // THE MUTEX IS NOT DECORATION. `AsyncAssetLoader` runs reads on `JobSystem` workers and the
        // completion registers the asset, so `NoteAsset` is genuinely called from more than one thread.
        struct State
        {
            std::mutex                   Mutex;
            Common::Utils::AssetRegistry Registry;
            bool                         Dirty = false;
        };

        State& Get_()
        {
            static State state;
            return state;
        }

        // Lower-cased so a `.TEX` on a case-preserving filesystem matches the census row, exactly as
        // `AssetPreloader`'s scan used to lower-case before comparing.
        std::string LowerExtension( const std::filesystem::path& file )
        {
            std::string ext = file.extension().string();
            std::transform( ext.begin(), ext.end(), ext.begin(),
                            []( unsigned char c ) { return static_cast<char>( ::tolower( c ) ); } );
            return ext;
        }
    } // namespace

    std::string RefreshOutcome::Describe() const
    {
        return std::to_string( Rows ) + " row(s) in the content registry: " + std::to_string( Added ) +
               " added, " + std::to_string( Removed ) + " removed, " + std::to_string( Edges ) +
               " dependency edge(s) recorded; " + ( Written ? "written" : "unchanged, not written" );
    }

    Common::ResultStr<std::size_t> Load()
    {
        const std::filesystem::path path = Common::Utils::AssetRegistry::DefaultPath();

        State& state = Get_();

        const std::lock_guard<std::mutex> lock( state.Mutex );

        // AN ABSENT FILE IS ZERO ROWS AND NOT A REFUSAL, and the distinction is the only one that
        // matters here: a project that has never been cooked has no registry, and refusing to start
        // would make "File / New Project" impossible. A file that EXISTS and will not parse is the
        // refusal — see LoadFrom, which separates the two.
        if ( !Common::Utils::FileSystem::Exists( path ) )
        {
            state.Registry = Common::Utils::AssetRegistry();
            state.Dirty    = false;
            LOG_WARN( "[ContentRegistry] '{}' does not exist, so this project has no cooked asset "
                      "registry and the preload has nothing to read. The editor writes one at the end "
                      "of its first session; run 'Rebuild Content Registry' to write one now.",
                      path.string() );
            return Common::MakeSuccess( std::size_t{ 0 } );
        }

        auto loaded = Common::Utils::AssetRegistry::LoadFrom( path );
        if ( !loaded )
            return Common::MakeError<std::size_t>( loaded.GetError() );

        state.Registry = std::move( loaded.GetValue() );
        state.Dirty    = false;

        // AND HERE IS THE POINT OF THE WHOLE TIER. Every row's handles are bound to its key before one
        // asset exists, so a number read out of a `.desce` names its file on a cold start with nothing
        // having been walked. The count is returned rather than logged here so the host can print it
        // beside the boot lines it is meant to be compared against.
        return Common::MakeSuccess( state.Registry.PublishIdentities() );
    }

    const Common::Utils::AssetRegistry& Get()
    {
        return Get_().Registry;
    }

    std::vector<std::filesystem::path> FilesOfKind( Common::Content::ContentKind kind )
    {
        State& state = Get_();

        const std::lock_guard<std::mutex> lock( state.Mutex );

        std::vector<std::filesystem::path> files;
        for ( const Common::Utils::AssetRegistryEntry* row : state.Registry.OfKind( Common::Content::KindName( kind ) ) )
        {
            // Expanded through the SAME inverse every other consumer of a stable key uses, which is
            // asserted to be the exact inverse of the derivation (AssetHandleStability). A registry
            // cooked on another machine therefore names this machine's files with no rewriting.
            files.push_back( Common::AssetHandle::PathForStableKey( row->Key ) );
        }
        return files;
    }

    std::optional<Common::Content::ContentKind> KindForFile( const std::filesystem::path& file )
    {
        const std::string ext = LowerExtension( file );
        if ( ext.empty() )
            return std::nullopt;

        for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
        {
            const auto kind = static_cast<Common::Content::ContentKind>( i );
            if ( Common::Content::KindSpec( kind ).Extension == ext )
                return kind;
        }
        return std::nullopt;
    }

    void NoteAsset( const std::filesystem::path& file, uint64_t effectiveHandle )
    {
        const std::optional<Common::Content::ContentKind> kind = KindForFile( file );
        if ( !kind )
            return; // not scanned content — a `.dgraph`, a procedural key, a memory-backed clip

        const std::string key = Common::AssetHandle::StableKeyForPath( file );
        if ( key.empty() )
            return;

        State& state = Get_();

        const std::lock_guard<std::mutex> lock( state.Mutex );

        const Common::Utils::AssetRegistryEntry* existing = state.Registry.FindByKey( key );
        if ( existing )
        {
            // THE IDENTITY IS THE ONE THING A KNOWN ROW CAN STILL LEARN. A `.tex` and a `.demat` carry
            // a handle of their own, and it is only known once the file has been PARSED — which is
            // after the row was first written from the filesystem. Recorded only when it differs from
            // the path-derived number, so the overwhelming majority of rows keep their `-`.
            const uint64_t pathHandle = existing->PathHandle();
            const uint64_t identity   = effectiveHandle == pathHandle ? 0 : effectiveHandle;
            if ( existing->Identity != identity )
            {
                state.Registry.SetIdentity( key, identity );
                state.Dirty = true;
            }
            return;
        }

        Common::Utils::AssetRegistryEntry entry;
        entry.Key  = key;
        entry.Kind = std::string( Common::Content::KindName( *kind ) );
        entry.Size = Common::Utils::FileSystem::GetFileSize( file );

        const uint64_t pathHandle = entry.PathHandle();
        entry.Identity            = effectiveHandle == pathHandle ? 0 : effectiveHandle;

        if ( const auto inserted = state.Registry.Insert( std::move( entry ) ); !inserted )
        {
            LOG_ERROR( "[ContentRegistry] '{}' could not enter the cooked asset registry: {}", key,
                       inserted.GetError() );
            return;
        }
        state.Dirty = true;
    }

    void NoteFile( const std::filesystem::path& file )
    {
        const std::optional<Common::Content::ContentKind> kind = KindForFile( file );
        if ( !kind )
            return;

        const std::string key = Common::AssetHandle::StableKeyForPath( file );
        if ( key.empty() )
            return;

        State& state = Get_();

        const std::lock_guard<std::mutex> lock( state.Mutex );

        // A KNOWN ROW IS LEFT EXACTLY AS IT IS, identity included. A re-cook rewrites the bytes of a
        // file whose identity the running session already learned by parsing it; clearing that here
        // would make the row forget the number every scene reference holds, and it would do it on the
        // one path where the file is most likely to be re-read a moment later.
        if ( state.Registry.FindByKey( key ) )
            return;

        Common::Utils::AssetRegistryEntry entry;
        entry.Key  = key;
        entry.Kind = std::string( Common::Content::KindName( *kind ) );
        entry.Size = Common::Utils::FileSystem::GetFileSize( file );

        if ( const auto inserted = state.Registry.Insert( std::move( entry ) ); !inserted )
        {
            LOG_ERROR( "[ContentRegistry] the cook wrote '{}' and it could not enter the registry: {}", key,
                       inserted.GetError() );
            return;
        }
        state.Dirty = true;
    }

    Common::ResultStr<RefreshOutcome> Refresh( AssetManager& manager )
    {
        RefreshOutcome outcome;

        // THE COOK'S WALK, AND THE ONLY ONE LEFT IN THE ENGINE. It is here rather than in the boot on
        // purpose: `[ContentScan] boot finished` is the number this tier is judged by, and a walk
        // before the boot line would have made the registry a place the cost moved to rather than a
        // place it stopped being paid.
        std::unordered_set<std::string> onDisk;
        {
            State&                            state = Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
            {
                const auto            kind = static_cast<Common::Content::ContentKind>( i );
                const Common::Content::ContentKindSpec spec = Common::Content::KindSpec( kind );

                for ( const auto& candidate : Common::Utils::FileSystem::ListFilesRecursive( *spec.Root ) )
                {
                    if ( LowerExtension( candidate ) != spec.Extension )
                        continue;

                    const std::string key = Common::AssetHandle::StableKeyForPath( candidate );
                    if ( key.empty() )
                        continue;
                    onDisk.insert( key );

                    if ( state.Registry.FindByKey( key ) )
                        continue;

                    Common::Utils::AssetRegistryEntry entry;
                    entry.Key  = key;
                    entry.Kind = std::string( spec.Name );
                    entry.Size = Common::Utils::FileSystem::GetFileSize( candidate );
                    if ( const auto inserted = state.Registry.Insert( std::move( entry ) ); !inserted )
                    {
                        LOG_ERROR( "[ContentRegistry] '{}' was found on disk and could not be entered: {}",
                                   key, inserted.GetError() );
                        continue;
                    }
                    ++outcome.Added;
                    state.Dirty = true;
                }
            }

            // ROWS WHOSE FILE IS GONE LEAVE. Collected first and erased after, because Remove mutates
            // the container the loop above would otherwise be walking.
            std::vector<std::string> vanished;
            for ( const Common::Utils::AssetRegistryEntry& row : state.Registry.Entries() )
            {
                if ( onDisk.find( row.Key ) == onDisk.end() )
                    vanished.push_back( row.Key );
            }
            for ( const std::string& key : vanished )
            {
                state.Registry.Remove( key );
                ++outcome.Removed;
                state.Dirty = true;
            }
        }

        // THE EDGES, READ FROM THE ONE TABLE THAT ALREADY OWNS THEM. `AssetEviction::EdgesOf` is the
        // list of "which asset classes name another asset", and `Desert/Tests/Engine/AssetEviction`
        // holds it against the classes that actually have such a field. Reading the edges here through
        // a second list would be the two-lists-that-must-agree shape this whole task is about.
        //
        // IT WALKS THE MANAGER'S OWN RECORDS AND NOT THE REGISTRY'S ROWS, and that is what keeps the
        // file from churning. An UNLOADED asset contributes no edges — reading its references would
        // mean parsing it, which is the work the demand-driven model exists to avoid — so a pass over
        // the rows would have written `no edges` for every asset this session happened not to touch,
        // and the next session would have written them back. A file that differs after every run is a
        // file nobody can diff and a gate nobody can trust. Iterating the records lets the rule be the
        // exact one: an asset that is loaded tells the truth about its edges, and an asset that is not
        // loaded has taught us nothing, so its row is left alone.
        {
            State&                            state = Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            for ( const auto& [metadata, asset] : manager.RegisteredAssets() )
            {
                if ( !asset || !asset->IsReadyForUse() )
                    continue;

                const Common::Utils::AssetRegistryEntry* row =
                     state.Registry.FindByHandle( static_cast<uint64_t>( metadata.Handle ) );
                if ( !row )
                    continue; // not scanned content (a procedural clip, a `.dgraph` document)

                std::vector<uint64_t> edges;
                AssetEviction::EdgesOf( manager, metadata.Handle,
                                        [&edges]( const Common::UUID& edge, const std::string& )
                                        {
                                            // Null edges are dropped HERE and not in EdgesOf: the sweep
                                            // wants them marked (a null in a slot is a slot that names
                                            // nothing, and marking it costs nothing), the registry must
                                            // not store a dependency on the null handle.
                                            if ( static_cast<uint64_t>( edge ) != 0 )
                                                edges.push_back( static_cast<uint64_t>( edge ) );
                                        } );

                std::sort( edges.begin(), edges.end() );
                edges.erase( std::unique( edges.begin(), edges.end() ), edges.end() );

                outcome.Edges += edges.size();
                if ( edges != row->Dependencies )
                {
                    const std::string key = row->Key; // SetDependencies invalidates `row`
                    state.Registry.SetDependencies( key, std::move( edges ) );
                    state.Dirty = true;
                }
            }

            outcome.Rows = state.Registry.Count();
        }

        if ( Dirty() )
        {
            if ( const auto written = Save(); !written )
                return Common::MakeError<RefreshOutcome>( written.GetError() );
            outcome.Written = true;
        }

        return Common::MakeSuccess( std::move( outcome ) );
    }

    Common::BoolResultStr Save()
    {
        const std::filesystem::path path = Common::Utils::AssetRegistry::DefaultPath();

        State& state = Get_();

        std::string text;
        {
            const std::lock_guard<std::mutex> lock( state.Mutex );
            text = state.Registry.Serialize();
        }

        std::error_code ec;
        std::filesystem::create_directories( path.parent_path(), ec );

        if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, text ); !written )
            return Common::MakeFormattedError<bool>( "the cooked asset registry '{}' could not be written: {}",
                                                     path.string(), written.GetError() );

        const std::lock_guard<std::mutex> lock( state.Mutex );
        state.Dirty = false;
        return Common::MakeSuccess( true );
    }

    bool Dirty()
    {
        State& state = Get_();

        const std::lock_guard<std::mutex> lock( state.Mutex );

        return state.Dirty;
    }

    void ResetForTest()
    {
        State& state = Get_();

        const std::lock_guard<std::mutex> lock( state.Mutex );

        state.Registry = Common::Utils::AssetRegistry();
        state.Dirty    = false;
    }
} // namespace Desert::Assets::ContentRegistry
