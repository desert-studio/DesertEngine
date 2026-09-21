#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/ContentRegistryInternals.hpp>

#include <Engine/Assets/AssetManager.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <mutex>

namespace Desert::Assets::ContentRegistry
{
    namespace Detail
    {
        // A FUNCTION-LOCAL STATIC, for `AssetPathIndex`'s reason: `NoteAsset` is reachable from
        // `AssetManager::CreateAsset`, which a translation unit's static initialiser can reach, and a
        // namespace-scope object would then be read before its own constructor ran.
        //
        // THE MUTEX IS NOT DECORATION. `AsyncAssetLoader` runs reads on `JobSystem` workers and the
        // completion registers the asset, so `NoteAsset` is genuinely called from more than one thread.
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
    } // namespace Detail

    Common::ResultStr<std::size_t> Load()
    {
        const std::filesystem::path path = Common::Utils::AssetRegistry::DefaultPath();

        Detail::State& state = Detail::Get_();

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
        return Detail::Get_().Registry;
    }

    std::vector<std::filesystem::path> FilesOfKind( Common::Content::ContentKind kind )
    {
        Detail::State& state = Detail::Get_();

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

    std::string KeyForHandle( uint64_t handle )
    {
        if ( handle == 0 )
            return {};

        {
            Detail::State&                            state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            if ( const Common::Utils::AssetRegistryEntry* row = state.Registry.FindByHandle( handle ) )
                return row->Key;
        }

        return Common::AssetPathIndex::KeyFor( handle );
    }

    std::optional<Common::Content::ContentKind> KindForFile( const std::filesystem::path& file )
    {
        const std::string ext = Detail::LowerExtension( file );
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

        Detail::State& state = Detail::Get_();

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

        Detail::State& state = Detail::Get_();

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

    Common::BoolResultStr Save()
    {
        const std::filesystem::path path = Common::Utils::AssetRegistry::DefaultPath();

        Detail::State& state = Detail::Get_();

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
        Detail::State& state = Detail::Get_();

        const std::lock_guard<std::mutex> lock( state.Mutex );

        return state.Dirty;
    }

    void ResetForTest()
    {
        Detail::State& state = Detail::Get_();

        const std::lock_guard<std::mutex> lock( state.Mutex );

        state.Registry = Common::Utils::AssetRegistry();
        state.Dirty    = false;
    }
} // namespace Desert::Assets::ContentRegistry
