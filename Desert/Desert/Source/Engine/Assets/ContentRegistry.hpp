#pragma once

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
    // that inverting the hash is the PRECONDITION for removing the walk, not the removal. This is the
    // removal: the walk happens once, in the editor, and its answer is a file.
    //
    // Boot now reads that file and publishes every row's identity into `AssetPathIndex` BEFORE anything
    // is created, so a handle read out of a `.desce` on a cold start names its file with nothing having
    // been walked, parsed or loaded. `[ContentScan] boot finished` is the detector that says the walk
    // really is gone rather than moved.
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
    // ── WHAT HAPPENS WHEN THE FILE IS MISSING OR STALE ────────────────────────────────────────────
    //
    // `Load` REFUSES, loudly, naming the file and the command that rebuilds it. It does not fall back
    // to a scan: a fallback would restore the cost this exists to remove and would do it silently, on
    // exactly the machines where nobody is watching. A file that is on disk and not in the registry is
    // a file the engine does not have — which is the danger GAP_ANALYSIS T2.7 names, and it is answered
    // by a gate that FAILS the build (`Desert/Tests/Editor/CookedRegistryGate`), never by a scan.
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
        // Reads `Common::Utils::AssetRegistry::DefaultPath()` and publishes every row's identity into
        // `Common::AssetPathIndex`. Returns how many bindings were made, so a host can log the number
        // beside the one it is meant to reproduce.
        //
        // AN ABSENT FILE IS A SUCCESS WITH ZERO ROWS, and only for a project that has never been
        // cooked — `Refresh` writes one at the end of the first editor session. A file that exists and
        // cannot be parsed is a REFUSAL: a truncated or hand-broken registry must not read as an empty
        // project, because an empty project starts and looks almost right.

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

        // Has a row been added or changed since the last `Load`/`Save`? The editor flushes on this
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

                // WHAT THE PROJECT REGISTRY SAID, kept beside what the session now believes, so that
                // `Save` can tell a row the repository carries from a row this cook produced — and a
                // row the project carries whose identity or edges this session LEARNED from the only
                // thing that can learn them, a parse.
                //
                // Without this distinction the two are indistinguishable by the time anything writes,
                // and the editor puts its own machine's cook output into the committed file. That is
                // not hypothetical: measured at ten rows on `dev`, and it is what makes a shared gate
                // red for everybody except whoever ran the cook last.
                Common::Utils::AssetRegistry Project;
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

        inline Common::ResultStr<std::size_t> Load()
        {
            const std::filesystem::path projectPath = Common::Utils::AssetRegistry::DefaultPath();
            const std::filesystem::path cookPath    = Common::Utils::AssetRegistry::CookOutputPath();

            Detail::State& state = Detail::Get_();

            const std::lock_guard<std::mutex> lock( state.Mutex );

            state.Registry = Common::Utils::AssetRegistry();
            state.Project  = Common::Utils::AssetRegistry();
            state.Dirty    = false;

            // THE PROJECT REGISTRY FIRST — the content the project carries, committed, and reproducible
            // from what the repository tracks.
            //
            // AN ABSENT FILE IS ZERO ROWS AND NOT A REFUSAL, and the distinction is the only one that
            // matters here: a project that has never been cooked has no registry, and refusing to start
            // would make "File / New Project" impossible. A file that EXISTS and will not parse is the
            // refusal — see LoadFrom, which separates the two.
            if ( Common::Utils::FileSystem::Exists( projectPath ) )
            {
                auto loaded = Common::Utils::AssetRegistry::LoadFrom( projectPath );
                if ( !loaded )
                    return Common::MakeError<std::size_t>( loaded.GetError() );

                // No `std::move`: `GetValue()` hands back a const reference, so a move here would be a
                // copy wearing a move's spelling — which is worse than a copy, because it reads as free.
                state.Project  = loaded.GetValue();
                state.Registry = state.Project;
            }
            else
            {
                LOG_WARN( "[ContentRegistry] '{}' does not exist, so this project carries no asset "
                          "registry and the preload has nothing to read from it. Run 'Rebuild Content "
                          "Registry', or `AssetRegistryTool cook`, and commit the result.",
                          projectPath.string() );
            }

            // THEN THIS COOK'S OWN ROWS, OVERRIDING KEY BY KEY. The same rule as the pak stack and for
            // the same reason (`VFS.hpp:21` — later mounts win): what a cook wrote most recently
            // describes the bytes that are actually on the disk it wrote them to.
            std::size_t cookRows = 0;
            if ( Common::Utils::FileSystem::Exists( cookPath ) )
            {
                auto cooked = Common::Utils::AssetRegistry::LoadFrom( cookPath );
                if ( !cooked )
                    return Common::MakeError<std::size_t>( cooked.GetError() );

                for ( const Common::Utils::AssetRegistryEntry& row : cooked.GetValue().Entries() )
                {
                    state.Registry.Remove( row.Key );
                    if ( const auto inserted = state.Registry.Insert( row ); !inserted )
                    {
                        LOG_ERROR( "[ContentRegistry] '{}' from the cook registry was refused: {}", row.Key,
                                   inserted.GetError() );
                        continue;
                    }
                    ++cookRows;
                }
            }

            LOG_INFO( "[ContentRegistry] {} row(s) the project carries, {} this cook produced",
                      state.Project.Count(), cookRows );

            // AND HERE IS THE POINT OF THE WHOLE TIER. Every row's handles are bound to its key before one
            // asset exists, so a number read out of a `.desce` names its file on a cold start with nothing
            // having been walked. The count is returned rather than logged here so the host can print it
            // beside the boot lines it is meant to be compared against.
            return Common::MakeSuccess( state.Registry.PublishIdentities() );
        }

        inline const Common::Utils::AssetRegistry& Get()
        {
            return Detail::Get_().Registry;
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

            // A KNOWN ROW KEEPS ITS IDENTITY AND ITS EDGES, AND TAKES THE NEW SIZE. A re-cook rewrites the
            // bytes of a file whose identity the running session already learned by parsing it; clearing
            // that here would make the row forget the number every scene reference holds, and it would do
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
                const auto size = Common::Utils::FileSystem::GetFileSize( file );
                if ( known->Size == size )
                    return;
                Common::Utils::AssetRegistryEntry updated = *known;
                updated.Size                              = size;
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

        inline Common::BoolResultStr Save()
        {
            // WRITES THE COOK REGISTRY AND NOTHING ELSE. The committed project registry is READ-ONLY to
            // the engine; its only writer is `AssetRegistryTool cook`, which sources its list from what
            // the repository tracks. That rule is what the split exists to make expressible: before it,
            // an editor session on any developer's machine rewrote the shared file with rows for that
            // machine's cook output, and the gate then failed for everybody except its author.
            //
            // A row the project DOES carry is written here too, but only when this session changed it —
            // a texture's declared identity and an asset's dependency edges can be learned only by
            // parsing, and only the editor parses. So they survive to the next boot without the
            // committed file moving. The comparison is per field rather than by identity of the row,
            // because "changed" has to mean changed and not merely re-inserted.
            const std::filesystem::path path = Common::Utils::AssetRegistry::CookOutputPath();

            Detail::State& state = Detail::Get_();

            std::string text;
            {
                const std::lock_guard<std::mutex> lock( state.Mutex );

                Common::Utils::AssetRegistry cookOnly;
                for ( const Common::Utils::AssetRegistryEntry& row : state.Registry.Entries() )
                {
                    const Common::Utils::AssetRegistryEntry* carried = state.Project.FindByKey( row.Key );
                    if ( carried != nullptr && carried->Kind == row.Kind && carried->Size == row.Size &&
                         carried->Identity == row.Identity && carried->Dependencies == row.Dependencies &&
                         Common::Utils::SameBounds( carried->Bounds, row.Bounds ) )
                    {
                        continue;
                    }
                    if ( const auto inserted = cookOnly.Insert( row ); !inserted )
                        return Common::MakeError<bool>( inserted.GetError() );
                }
                text = cookOnly.Serialize();
            }

            std::error_code ec;
            std::filesystem::create_directories( path.parent_path(), ec );

            if ( const auto written = Common::Utils::FileSystem::WriteContentToFileAtomic( path, text ); !written )
            {
                return Common::MakeFormattedError<bool>( "the cook's asset registry '{}' could not be "
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
            state.Project  = Common::Utils::AssetRegistry();
            state.Dirty    = false;
        }
    } // namespace ContentRegistry
} // namespace Desert::Assets
