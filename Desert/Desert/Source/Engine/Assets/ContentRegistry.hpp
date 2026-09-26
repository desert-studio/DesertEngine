#pragma once

#include <Common/Content/AssetMove.hpp>
#include <Common/Content/ContentKinds.hpp>
#include <Common/Content/ContentScan.hpp>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/AssetPathIndex.hpp>
#include <Common/Core/Logger.hpp>
#include <Common/Core/ResultStr.hpp>
#include <Common/Utilities/AssetRegistry.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Assets
{
    class AssetManager;

    // THE PROCESS'S ONE COOKED ASSET REGISTRY — what replaced the boot's directory walk.
    //
    // ── WHAT CHANGED, AND WHAT IT COST BEFORE ─────────────────────────────────────────────────────
    //
    // Both hosts used to start by walking eight content roots with `ListFilesRecursive`, once per
    // content kind, and every handle the engine could resolve was minted by that walk. `AssetPathIndex`
    // made the result observable — `368 handle(s) can name their own path` — and said in its own header
    // that inverting the hash is the PRECONDITION for removing the walk, not the removal.
    //
    // The editor now GATHERS its registry at start (`Gather`, below): one walk reading each file's header
    // only, with this machine's cache sparing the headers of unchanged files — UE's FAssetDataGatherer and
    // Intermediate/CachedAssetRegistry.bin. A packaged game reads the registry the packager cooked into its
    // pak (`LoadCooked`). Either way every row's identity is in `AssetPathIndex` BEFORE anything is created,
    // so a handle read out of a `.desce` names its file with nothing having been parsed or loaded.
    //
    // ── THE ROWS ARE TOTAL BY CONSTRUCTION, AND THAT IS THE WHOLE DESIGN ──────────────────────────
    //
    // `NoteAsset` is called by `AssetManager::CreateAsset`, i.e. at the exact moment a file becomes an
    // asset — the same place and for the same reason `AssetHandle::FromCookedPath` records its own
    // inverse. Nothing becomes content in this engine without going through that function, so no kind
    // can be missing from the registry because somebody forgot to add it to a list. The repository has
    // paid twice for the other arrangement: the packager's hand-typed tree list that forgot fonts and
    // icons, and the twelve-branch `ToPath` that returns "" for a type nobody added a branch for.
    //
    // ── WHAT THE EDITOR DOES THAT THE RUNTIME DOES NOT ────────────────────────────────────────────
    //
    // The editor is the cook. `Refresh` walks the content roots, enters anything new, drops rows whose
    // file is gone, and fills in each row's dependency edges from the live manager. It runs AFTER the
    // boot is over — after `[ContentScan] boot finished` has been printed — deliberately, because
    // putting it before would put the walk back into the number this whole tier is judged by. The
    // runtime never calls it and never writes the file.
    //
    // It is the SAFETY NET and not the mechanism: content authored in the editor gets its row from
    // `NoteAsset` the moment `CreateAsset` sees it, and content the cook writes gets one from
    // `NoteFile` the moment `WriteCookedJson` writes it. What `Refresh` catches is a file that arrived
    // on disk with nobody looking — a `git pull`, a drop into the folder while the editor was closed —
    // which is available the next time the project opens rather than in the session that found it.
    //
    // ── WHAT HAPPENS WHEN THE CACHE IS MISSING OR STALE ───────────────────────────────────────────
    //
    // Nothing but cost: the cache is derived state, so an absent or unparsable one is a full header scan
    // and a warning, never a refusal. What IS refused is a single file whose header cannot become a row —
    // named, one line each, by `Gather`. A packaged game has no content roots to fall back to, so there an
    // absent cooked registry is a refusal (`LoadCooked`).
    //
    // ── HEADER-ONLY, AND THAT IS LOAD-BEARING RATHER THAN A STYLE CHOICE ──────────────────────────
    //
    // The same reason `AssetManager` is, stated in its own header: eleven suites compile a hand-picked
    // list of engine sources precisely so that a registry can be built without linking Vulkan, and
    // giving that class a `.cpp` broke every one of them. `NoteAsset` is called from
    // `AssetManager::CreateAsset`, so the moment it lived in a translation unit, SEVEN suites stopped
    // linking with an undefined symbol — measured, in the first full sweep of this task. Everything
    // here reaches `Common` and nothing else, which every suite already links.
    //
    // The COOK is the exception and lives in `ContentRegistryCook.cpp`: it reads dependency edges
    // through `AssetEviction`, which reaches `Graphic::ResourceLedger` and through it the renderer.
    namespace ContentRegistry
    {
        // The files of one kind, as paths on THIS machine — each row's key expanded through
        // `AssetHandle::PathForStableKey`. This is the call that replaced
        // `ListFilesRecursive( root )` filtered by extension at sixteen call sites.

        // Records that `file` is content and that the engine knows it by `effectiveHandle`. Called by
        // `AssetManager::CreateAsset`; idempotent, and cheap enough to be on that path (a hash lookup
        // and, on a genuinely new file, one insert).
        //
        // A path whose extension is not one of the census's kinds is IGNORED rather than refused: the
        // manager also creates assets for `.dgraph` documents and for procedural and memory-backed
        // keys, none of which any scan enumerates. `KindForFile` is the one place that decides.

        // Records that `file` is content, WITHOUT claiming to know the handle the engine will know it
        // by. Called by the cook at the moment it writes a cooked file (`WriteCookedJson`), which is
        // the one place a `.stmesh`, `.skeleton`, `.anim` or `.tex` comes into existence.
        //
        // WHY TWO FUNCTIONS AND NOT A DEFAULT ARGUMENT. A `.tex` declares a handle of its own INSIDE
        // the file; the cook writes the bytes and the row, and the declared identity only becomes
        // known when something parses it — which is `NoteAsset`, later, from `CreateAsset`. Passing 0
        // through `NoteAsset` would mean "the identity is the path-derived one", which for a `.tex` is
        // a claim that is false and would be written into the row. Not knowing and knowing-it-is-none
        // are different answers, so they are different calls.

        // WHICH FILE A HANDLE NAMES, as a stable key — the inverse of `AssetHandle::FromCookedPath`,
        // for every handle this engine can resolve, asked WITHOUT a type and WITHOUT an AssetManager.
        //
        // THIS IS THE FUNCTION THE TWELVE-BRANCH `ToPath` COLLAPSED INTO. `Core::MakeAssetResolver`
        // used to answer it with a hand-written table of per-type lookups, each of them
        // `mgr.FindByHandle<SomeAsset>( h )->GetMetadata().Filepath`, and that arrangement had three
        // properties that stopped being survivable with T2.4:
        //
        //   * it needed the asset REGISTERED in the AssetManager, and the only thing that registered
        //     assets wholesale was the directory walk this slice removes. Every branch was one step
        //     from answering "" — which a scene saves as an empty slot and loads as unset;
        //   * a type nobody wrote a branch for got no answer at all, and the branch that says so is
        //     the only reason anyone would ever find out;
        //   * it lived three layers above where identity is minted, so nothing below the engine could
        //     ask it.
        //
        // TWO SOURCES, AND THEY ARE NOT TWO LISTS THAT MUST AGREE. The registry answers for content:
        // it is persistent, it knows a `.tex`'s DECLARED identity, and it is what a shipped game has.
        // `Common::AssetPathIndex` answers for anything this SESSION has derived a handle from,
        // including files outside every content root, which have no row by definition. Neither is a
        // subset of the other and neither can answer the other's question; the union is the answer,
        // and the registry goes first because it is the one that survives the process.
        //
        // Empty means nothing ever derived this number from a path — a `Generate()`d runtime id, which
        // genuinely has no file. Callers log their own refusal, because only they know what the number
        // was for.

        // Which kind, if any, a file belongs to — by extension, over the census. std::nullopt means
        // "not scanned content", which is an answer and not a failure.

        // THE COOK. Walks every content root, enters files that have no row, drops rows whose file is
        // gone, and re-reads dependency edges from `manager` for every asset it holds. Writes the file
        // when anything changed. Editor only.
        struct RefreshOutcome
        {
            std::size_t               Rows    = 0;
            std::size_t               Added   = 0;
            std::size_t               Removed = 0;
            std::size_t               Edges   = 0;
            std::size_t               Bounded = 0; // rows that carry a box after this refresh
            bool                      Written = false;
            [[nodiscard]] std::string Describe() const;
        };
        [[nodiscard]] Common::ResultStr<RefreshOutcome> Refresh( AssetManager& manager );

        // Writes the current rows to `Common::Utils::AssetRegistry::DefaultPath()`. Separate from
        // `Refresh` so the editor can flush rows that `NoteAsset` added during a session without
        // re-walking the disk.

        // Has a row been added or changed since the last `Gather`/`Save`? The editor flushes on this
        // rather than writing the file on every import.

        // EXISTS FOR TESTS ONLY, for `AssetPathIndex::Clear`'s reason: a suite that moves the project
        // root underneath the registry must not judge its second run against the first run's rows.

        namespace Detail
        {
            // THE MUTEX IS NOT DECORATION. `AsyncAssetLoader` runs reads on `JobSystem` workers and the
            // completion registers the asset, so `NoteAsset` is genuinely called from more than one thread.
            struct State
            {
                std::mutex                   Mutex;
                Common::Utils::AssetRegistry Registry;
                bool                         Dirty = false;
            };

            // A FUNCTION-LOCAL STATIC, for `AssetPathIndex`'s reason: `NoteAsset` is reachable from
            // `AssetManager::CreateAsset`, which a translation unit's static initialiser can reach, and a
            // namespace-scope object would then be read before its own constructor ran. It is also the
            // spelling that makes this header-only: one definition, shared across every translation unit
            // that includes this file, without a `.cpp` for anyone to have to link.
            inline State& Get_()
            {
                static State state;
                return state;
            }

            // Lower-cased so a `.TEX` on a case-preserving filesystem matches the census row, exactly as
            // `AssetPreloader`'s scan used to lower-case before comparing.
            inline std::string LowerExtension( const std::filesystem::path& file )
            {
                std::string ext = file.extension().string();
                std::transform( ext.begin(), ext.end(), ext.begin(),
                                []( unsigned char c ) { return static_cast<char>( ::tolower( c ) ); } );
                return ext;
            }
        } // namespace Detail

        namespace Detail
        {
            inline std::size_t Publish( Common::Utils::AssetRegistry registry )
            {
                State&                            state = Get_();
                const std::lock_guard<std::mutex> lock( state.Mutex );
                state.Registry = std::move( registry );
                state.Dirty    = false;
                return state.Registry.PublishIdentities();
            }
        } // namespace Detail

        // THE EDITOR'S REGISTRY BUILDS ITSELF, as UE's asset registry does at editor start: a walk of the
        // content roots reading each file's header only (`Common::Content::GatherContentRegistry`), with this
        // machine's cache (`RegistryCachePath`, outside git) sparing the headers of unchanged files. No cache
        // is the ordinary first start, not a warning. The cache is written back when the gather read anything.
        // `refused`, when given, receives the sentence of every file that could not enter (the packager
        // refuses on them; the editor only reports them). `meshesWithoutHeaderBounds`, when given, receives
        // the keys of meshes whose header states no box (cooked before it did): the engine-linking caller
        // reads those bodies once and hands the boxes back through `NoteBounds`, and the cache keeps them.
        inline Common::ResultStr<std::size_t>
        Gather( std::vector<std::string>* refused                   = nullptr,
                std::vector<std::string>* meshesWithoutHeaderBounds = nullptr )
        {
            Common::Content::RegistryCache cache;
            const std::filesystem::path    cachePath = Common::Content::RegistryCachePath();
            if ( Common::Utils::FileSystem::Exists( cachePath ) )
            {
                const auto text   = Common::Utils::FileSystem::ReadFileContent( cachePath );
                auto       parsed = text ? Common::Content::ParseRegistryCache( text.GetValue() )
                                         : Common::MakeError<Common::Content::RegistryCache>( text.GetError() );
                if ( parsed )
                    cache = parsed.GetValue();
                else
                    LOG_WARN( "[ContentRegistry] the local cache '{}' is unusable and is rebuilt from the "
                              "content: {}",
                              cachePath.string(), parsed.GetError() );
            }

            Common::Content::GatheredRegistry gathered = Common::Content::GatherContentRegistry( cache );
            for ( const std::string& refusal : gathered.Refused )
                LOG_ERROR( "[ContentRegistry] a content file could not enter the registry: {}", refusal );
            if ( refused )
                *refused = gathered.Refused;
            if ( meshesWithoutHeaderBounds )
                *meshesWithoutHeaderBounds = gathered.MeshesWithoutHeaderBounds;
            LOG_INFO( "[ContentRegistry] gathered {} row(s): {} from the local cache, {} header(s) read",
                      gathered.Registry.Count(), gathered.FromCache, gathered.Read );

            const bool        changed = gathered.Read > 0 || gathered.Registry.Count() != cache.Registry.Count();
            const std::size_t bound   = Detail::Publish( std::move( gathered.Registry ) );
            if ( changed )
            {
                Detail::Get_().Dirty = true;
            }
            return Common::MakeSuccess( bound );
        }

        // A PACKAGED GAME'S REGISTRY: the one the packager cooked into the pak at `AssetRegistry::DefaultPath`.
        // Read through the VFS; absent or unparsable is a refusal, because a shipped game has no content roots
        // to gather from.
        inline Common::ResultStr<std::size_t> LoadCooked()
        {
            auto loaded = Common::Utils::AssetRegistry::LoadFrom( Common::Utils::AssetRegistry::DefaultPath() );
            if ( !loaded )
                return Common::MakeError<std::size_t>( loaded.GetError() );
            LOG_INFO( "[ContentRegistry] {} row(s) from the cooked registry", loaded.GetValue().Count() );
            return Common::MakeSuccess( Detail::Publish( loaded.GetValue() ) );
        }

        inline const Common::Utils::AssetRegistry& Get()
        {
            return Detail::Get_().Registry;
        }

        // The current key of the asset a redirector names, through any further redirectors; "" when the
        // registry cannot say. The namer Common::Content::RefuseRedirectorBytes takes.
        inline std::string KeyOfRedirectorTarget( const Common::Content::AssetGuid& target )
        {
            const Common::Utils::AssetRegistry&      registry = Get();
            const Common::Utils::AssetRegistryEntry* row =
                 registry.FindByHandle( Common::Content::HandleForGuid( target ) );
            if ( row == nullptr )
                return {};
            auto followed = registry.FollowRedirectors( *row );
            return followed ? followed.GetValue()->Key : std::string();
        }

        inline std::vector<std::filesystem::path> FilesOfKind( Common::Content::ContentKind kind )
        {
            Detail::State& state = Detail::Get_();

            const std::lock_guard<std::mutex> lock( state.Mutex );

            std::vector<std::filesystem::path> files;
            for ( const Common::Utils::AssetRegistryEntry* row :
                  state.Registry.OfKind( Common::Content::KindName( kind ) ) )
            {
                // Expanded through the SAME inverse every other consumer of a stable key uses, which is
                // asserted to be the exact inverse of the derivation (AssetHandleStability). A registry
                // cooked on another machine therefore names this machine's files with no rewriting.
                files.push_back( Common::AssetHandle::PathForStableKey( row->Key ) );
            }
            return files;
        }

        // The header GUID the cooked registry row of `handle`'s file states; nullopt when no row or no GUID.
        inline std::optional<Common::Content::AssetGuid> GuidForHandle( uint64_t handle )
        {
            if ( handle == 0 )
                return std::nullopt;
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );
            if ( const Common::Utils::AssetRegistryEntry* row = state.Registry.FindByHandle( handle ) )
                return row->Guid;
            return std::nullopt;
        }

        inline std::string KeyForHandle( uint64_t handle )
        {
            if ( handle == 0 )
                return {};

            {
                Detail::State&                    state = Detail::Get_();
                const std::lock_guard<std::mutex> lock( state.Mutex );

                if ( const Common::Utils::AssetRegistryEntry* row = state.Registry.FindByHandle( handle ) )
                    return row->Key;
            }

            return Common::AssetPathIndex::KeyFor( handle );
        }

        inline std::optional<Common::Content::ContentKind> KindForFile( const std::filesystem::path& file )
        {
            const std::string ext = Detail::LowerExtension( file );
            if ( ext.empty() )
                return std::nullopt;
            // Inside a content root the scan's rule decides (longest root wins -- Texture and Skybox share
            // `.detex`); a file outside every root is classified by its extension, as it always was.
            if ( const auto scanned = Common::Content::KindOfContentFile( file ) )
                return scanned;

            for ( std::size_t i = 0; i < Common::Content::CONTENT_KIND_COUNT; ++i )
            {
                const auto kind = static_cast<Common::Content::ContentKind>( i );
                if ( Common::Content::KindSpec( kind ).Extension == ext )
                    return kind;
            }
            return std::nullopt;
        }

        inline void NoteAsset( const std::filesystem::path& file, uint64_t effectiveHandle )
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
            if ( existing != nullptr && existing->Guid.has_value() )
            {
                // A ROW WITH A HEADER GUID IS KNOWN BY THAT GUID'S FOLD, written by the gather; the load
                // only confirms it. A loaded asset answering to another number is a defect in its loader,
                // and overwriting the row would spread it to every reference resolved through the registry.
                if ( effectiveHandle != existing->Identity )
                    LOG_ERROR( "[ContentRegistry] '{}' loaded as handle {:016x}, but its header GUID folds to "
                               "{:016x}; the registry keeps the header's",
                               key, effectiveHandle, existing->Identity );
                return;
            }
            if ( existing != nullptr )
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

            auto described =
                 Common::Content::RegistryRowFor( key, Common::Content::DescribeContentFile( file, *kind ) );
            if ( !described )
            {
                LOG_ERROR( "[ContentRegistry] '{}' could not enter the cooked asset registry: {}", key,
                           described.GetError() );
                return;
            }
            Common::Utils::AssetRegistryEntry entry = described.GetValue();
            if ( !entry.Guid.has_value() )
            {
                const uint64_t pathHandle = entry.PathHandle();
                entry.Identity            = effectiveHandle == pathHandle ? 0 : effectiveHandle;
            }

            if ( const auto inserted = state.Registry.Insert( std::move( entry ) ); !inserted )
            {
                LOG_ERROR( "[ContentRegistry] '{}' could not enter the cooked asset registry: {}", key,
                           inserted.GetError() );
                return;
            }
            state.Dirty = true;
        }

        inline void NoteFile( const std::filesystem::path& file )
        {
            const std::optional<Common::Content::ContentKind> kind = KindForFile( file );
            if ( !kind )
                return;

            const std::string key = Common::AssetHandle::StableKeyForPath( file );
            if ( key.empty() )
                return;

            Detail::State& state = Detail::Get_();

            const std::lock_guard<std::mutex> lock( state.Mutex );

            // A KNOWN ROW IS RE-DESCRIBED FROM ITS HEADER; A HEADER-LESS ROW KEEPS ITS IDENTITY. A re-cook
            // rewrites the bytes of a file whose identity the running session already learned by parsing it;
            // clearing that here would make the row forget the number every scene reference holds, and it would do
            // it on the one path where the file is most likely to be re-read a moment later.
            //
            // THE SIZE IS DIFFERENT: it is a fact about the bytes this call was told were just written,
            // and nothing else in the engine ever corrects it. Leaving it made every re-cook a registry
            // the packager refuses — measured on this tree, where four mesh-side `.tex` rows still said
            // 22 369 984 bytes (their size before BC7) against files of 2-3 MB, and `PackageGame` named
            // all four as "the registry predates an edit". Remove + Insert rather than a setter, because
            // the row's identity index lives inside `AssetRegistry` and a copy carries it back unchanged.
            if ( const Common::Utils::AssetRegistryEntry* known = state.Registry.FindByKey( key );
                 known != nullptr )
            {
                auto described =
                     Common::Content::RegistryRowFor( key, Common::Content::DescribeContentFile( file, *kind ) );
                if ( !described )
                {
                    LOG_ERROR( "[ContentRegistry] the cook rewrote '{}' and its header refused: {}", key,
                               described.GetError() );
                    return;
                }
                // The header states the GUID and the edges again (a resaved material may name other
                // textures); a header-less row keeps the identity the session learned, and a mesh keeps
                // its box until NoteBounds states the new one.
                Common::Utils::AssetRegistryEntry updated = described.GetValue();
                if ( !updated.Guid.has_value() )
                    updated.Identity = known->Identity;
                if ( !updated.Bounds.has_value() )
                    updated.Bounds = known->Bounds;
                if ( updated.Size == known->Size && updated.Guid == known->Guid &&
                     updated.Identity == known->Identity && updated.Dependencies == known->Dependencies &&
                     updated.Versions == known->Versions )
                    return;
                state.Registry.Remove( key );
                if ( const auto inserted = state.Registry.Insert( std::move( updated ) ); !inserted )
                {
                    LOG_ERROR( "[ContentRegistry] the cook rewrote '{}' and its row could not be updated: {}", key,
                               inserted.GetError() );
                    return;
                }
                state.Dirty = true;
                return;
            }

            auto described =
                 Common::Content::RegistryRowFor( key, Common::Content::DescribeContentFile( file, *kind ) );
            if ( !described )
            {
                LOG_ERROR( "[ContentRegistry] the cook wrote '{}' and its header refused: {}", key,
                           described.GetError() );
                return;
            }
            if ( const auto inserted = state.Registry.Insert( described.GetValue() ); !inserted )
            {
                LOG_ERROR( "[ContentRegistry] the cook wrote '{}' and it could not enter the registry: {}", key,
                           inserted.GetError() );
                return;
            }
            state.Dirty = true;
        }

        // ONE ROW AS A PICKER READS IT — what the editor's asset lists enumerate instead of the objects the
        // preloader happened to create. Reading it creates nothing and loads nothing: this is UE's
        // `IAssetRegistry::GetAssetsByClass` returning FAssetData, not UObjects, and it is what lets the
        // preloader be removed later without every dropdown in the editor going empty at the same time.
        struct PickerRow
        {
            Common::AssetHandle                       Handle; // the number the engine knows the file by
            std::string                               Key;    // the stable key, `root:relative/path`
            std::filesystem::path                     Path;   // the key expanded on THIS machine
            std::optional<Common::Content::AssetGuid> Guid;   // the header's GUID, when the file states one
            // The Name tag: what the file states as its display name, read by the scan without loading it;
            // empty when the file states none (then a list shows the stem, as the asset itself does).
            std::string DisplayName;
            bool        Skinned = false; // the Skinned tag: a mesh whose header flags a skeleton
        };

        // The rows of one kind in registry order — the SAME order `FilesOfKind` hands the preloader, so a
        // list built from these is the list the loaded shells used to produce, entry for entry.
        inline std::vector<PickerRow> Rows( Common::Content::ContentKind kind )
        {
            Detail::State& state = Detail::Get_();

            const std::lock_guard<std::mutex> lock( state.Mutex );

            std::vector<PickerRow> rows;
            for ( const Common::Utils::AssetRegistryEntry* row :
                  state.Registry.OfKind( Common::Content::KindName( kind ) ) )
            {
                rows.push_back( { Common::AssetHandle( row->EffectiveHandle() ), row->Key,
                                  Common::AssetHandle::PathForStableKey( row->Key ), row->Guid, row->DisplayName,
                                  row->Skinned } );
            }
            return rows;
        }

        // THE MESH PICKERS' ROWS, split by the Skinned tag and not by extension (UE filters FAssetData by its
        // tags the same way): every mesh row of both mesh kinds whose header does (`skinned`) or does not flag
        // a skeleton. The header is read at scan time, so a mesh nobody has loaded is on the right list — the
        // loaded-object filter this replaces put unloaded skinned meshes on the static list.
        inline std::vector<PickerRow> MeshRows( bool skinned )
        {
            std::vector<PickerRow> rows;
            for ( const Common::Content::ContentKind kind :
                  { Common::Content::ContentKind::StaticMesh, Common::Content::ContentKind::SkinnedMesh } )
            {
                for ( PickerRow& row : Rows( kind ) )
                {
                    if ( row.Skinned == skinned )
                        rows.push_back( std::move( row ) );
                }
            }
            return rows;
        }

        // ONE FILE CHANGED ON DISK — appeared, was rewritten or went away — and the registry follows it
        // without a walk. Called by the editor's hot reload for exactly the files it saw move; the full
        // `Refresh` remains the safety net for what arrived while nobody was looking. A file that is not a
        // census kind is ignored, the same way `NoteFile` ignores it.
        inline void Update( const std::filesystem::path& file )
        {
            if ( !KindForFile( file ) )
                return;

            std::error_code ec;
            if ( std::filesystem::exists( file, ec ) )
            {
                NoteFile( file );
                return;
            }

            const std::string key = Common::AssetHandle::StableKeyForPath( file );
            if ( key.empty() )
                return;

            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );
            if ( state.Registry.Remove( key ) )
                state.Dirty = true;
        }

        // Records the box `file` occupies around its own origin — the mesh cook's statement at the moment
        // it writes a `.stmesh` / `.skmesh`, which is the one time the cook holds the geometry. Called after
        // `NoteFile`, which is what gives the file its row; a file with no row is not content and gets no box.
        inline void NoteBounds( const std::filesystem::path&             file,
                                const std::optional<Common::Math::AABB>& bounds )
        {
            const std::string key = Common::AssetHandle::StableKeyForPath( file );

            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            const Common::Utils::AssetRegistryEntry* row = state.Registry.FindByKey( key );
            if ( row == nullptr || Common::Utils::SameBounds( row->Bounds, bounds ) )
                return;
            state.Registry.SetBounds( key, bounds );
            state.Dirty = true;
        }

        // RENAME / MOVE OF ONE ASSET (AF10c), the editor's single route: the Common move (file renamed, a
        // redirector written at the old path, the registry's rows swapped) under this registry's lock, so a
        // reader never sees the old row gone and the new one not yet there. The moved row's path handle is
        // recorded in the path index, as `Publish` does for every row, so a handle minted from the new key
        // names it at once. `redirectorSelf` is passed back by a redo so it writes the very same redirector.
        inline Common::ResultStr<Common::Content::AssetMoveRecord>
        MoveAsset( const std::filesystem::path& from, const std::filesystem::path& to,
                   const std::optional<Common::Content::AssetGuid>& redirectorSelf = std::nullopt )
        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            auto moved = Common::Content::MoveAssetLeavingRedirector( state.Registry, from, to, redirectorSelf );
            if ( !moved )
                return moved;
            if ( const Common::Utils::AssetRegistryEntry* row =
                      state.Registry.FindByKey( Common::AssetHandle::StableKeyForPath( to ) ) )
                static_cast<void>( Common::AssetPathIndex::Record( row->PathHandle(), row->Key ) );
            state.Dirty = true;
            return moved;
        }

        inline Common::BoolResultStr UndoMove( const Common::Content::AssetMoveRecord& record )
        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            auto undone = Common::Content::UndoAssetMove( state.Registry, record );
            if ( undone )
                state.Dirty = true;
            return undone;
        }

        // RENAME / MOVE OF A FOLDER (AF10c): every row under it moves as `MoveAsset` does, one registry lock
        // over the whole folder, all or nothing (see `Common::Content::MoveFolderLeavingRedirectors`).
        inline Common::ResultStr<Common::Content::AssetFolderMoveRecord>
        MoveFolder( const std::filesystem::path& from, const std::filesystem::path& to,
                    const std::map<std::filesystem::path, Common::Content::AssetGuid>& redirectorSelves = {} )
        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            auto moved =
                 Common::Content::MoveFolderLeavingRedirectors( state.Registry, from, to, redirectorSelves );
            if ( !moved )
                return moved;
            for ( const Common::Content::AssetMoveRecord& asset : moved.GetValue().Assets )
                if ( const Common::Utils::AssetRegistryEntry* row =
                          state.Registry.FindByKey( Common::AssetHandle::StableKeyForPath( asset.To ) ) )
                    static_cast<void>( Common::AssetPathIndex::Record( row->PathHandle(), row->Key ) );
            state.Dirty = true;
            return moved;
        }

        inline Common::BoolResultStr UndoFolderMove( const Common::Content::AssetFolderMoveRecord& record )
        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            auto undone = Common::Content::UndoFolderMove( state.Registry, record );
            state.Dirty = true;
            return undone;
        }

        // True when `file` is content the registry has a row for - the files a rename must move through
        // `MoveAsset`; anything else (a folder, a source image, a note) is a plain file operation.
        inline bool HasRow( const std::filesystem::path& file )
        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            return state.Registry.FindByKey( Common::AssetHandle::StableKeyForPath( file ) ) != nullptr;
        }

        // The keys of every asset whose edges name `file`: what keeps loading through the redirector a move
        // leaves, listed by the rename dialog before the user confirms. Empty for a file with no row.
        inline std::vector<std::string> Referrers( const std::filesystem::path& file )
        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            const Common::Utils::AssetRegistryEntry* row =
                 state.Registry.FindByKey( Common::AssetHandle::StableKeyForPath( file ) );
            return row != nullptr ? Common::Content::ReferrersOf( state.Registry, *row )
                                  : std::vector<std::string>();
        }

        // THE FILE A PATH OPENS: the old path of a moved asset opens the file the registry resolves it to
        // (through every redirector), so a scene or string table still named by its old path loads. A path
        // the registry cannot resolve past itself is returned as given, and the loader's own redirector
        // refusal then names the problem.
        inline std::filesystem::path FileToOpen( const std::filesystem::path& file )
        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            const Common::Utils::AssetRegistryEntry* row = state.Registry.FindByReference( 0, file.string() );
            if ( row == nullptr || row->Key == Common::AssetHandle::StableKeyForPath( file ) )
                return file;
            return Common::AssetHandle::PathForStableKey( row->Key );
        }

        inline Common::BoolResultStr Save()
        {
            const std::filesystem::path path = Common::Content::RegistryCachePath();

            Detail::State& state = Detail::Get_();

            std::string text;
            {
                const std::lock_guard<std::mutex> lock( state.Mutex );
                text = Common::Content::SerializeRegistryCache( state.Registry );
            }

            std::error_code ec;
            std::filesystem::create_directories( path.parent_path(), ec );

            if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, text ); !written )
            {
                return Common::MakeFormattedError<bool>( "the local asset registry cache '{}' could not be "
                                                         "written: {}",
                                                         path.string(), written.GetError() );
            }

            const std::lock_guard<std::mutex> lock( state.Mutex );
            state.Dirty = false;
            return Common::MakeSuccess( true );
        }

        inline bool Dirty()
        {
            Detail::State& state = Detail::Get_();

            const std::lock_guard<std::mutex> lock( state.Mutex );

            return state.Dirty;
        }

        inline void ResetForTest()
        {
            Detail::State& state = Detail::Get_();

            const std::lock_guard<std::mutex> lock( state.Mutex );

            state.Registry = Common::Utils::AssetRegistry();
            state.Dirty    = false;
        }
    } // namespace ContentRegistry
} // namespace Desert::Assets
