#include <Engine/Assets/ContentRegistry.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/MeshAsset.hpp>
#include <Engine/Core/WorkSink.hpp>
#include <Engine/Geometry/MeshBounds.hpp>

#include <Common/Content/ContentScan.hpp>
#include <Common/Content/ScanProgress.hpp>
#include <Common/Core/Logger.hpp>

#include <format>
#include <map>
#include <mutex>

// THE COOK — the one function of the content registry that still walks a directory, and the only walk
// left in this engine. It is a translation unit of its own because it reads dependency edges through
// `AssetEviction`, which reaches the renderer; see ContentRegistryInternals.hpp for what that costs
// everything else if the two share a file.
namespace Desert::Assets::ContentRegistry
{
    std::string RefreshOutcome::Describe() const
    {
        return std::format( "{} row(s) in the content registry: {} added, {} removed, {} dependency edge(s) "
                            "recorded, {} row(s) with bounds; {}",
                            Rows, Added, Removed, Edges, Bounded, Written ? "written" : "unchanged, not written" );
    }

    // THE EDITOR'S REGISTRY BUILDS ITSELF, as UE's asset registry does at editor start: a walk of the content
    // roots reading each file's header only (`Common::Content::GatherContentRegistry`), with this machine's cache
    // (`RegistryCachePath`, outside git) sparing the headers of unchanged files. No cache is the ordinary first
    // start, not a warning. The cache is written back when the gather read anything. `refused`, when given,
    // receives the sentence of every file that could not enter (the packager refuses on them; the editor only
    // reports them). `meshesWithoutHeaderBounds`, when given, receives the keys of meshes whose header states no
    // box (cooked before it did): the engine-linking caller reads those bodies once and hands the boxes back
    // through `NoteBounds`, and the cache keeps them. @p sink, when given, is told the files the walk listed
    // (Expect, once) and each file as an item around its cache check or header read.
    Common::ResultStr<std::size_t> GatherWatched( std::vector<std::string>* const refused,
                                                  std::vector<std::string>* const meshesWithoutHeaderBounds,
                                                  Core::WorkSink* const           sink )
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
                LOG_WARN( "[ContentRegistry] the local cache '{}' is unusable and is rebuilt from the content: {}",
                          cachePath.string(), parsed.GetError() );
        }

        uint64_t                      checking = 0;
        Common::Content::ScanProgress progress;
        if ( sink )
        {
            progress.Listed = [sink]( const std::size_t files ) { sink->Expect( files ); };
            progress.Begin  = [sink, &checking]( const std::filesystem::path& file )
            { checking = sink->BeginItem( file.filename().string() ); };
            progress.End = [sink, &checking] { sink->FinishItem( checking ); };
        }
        Common::Content::GatheredRegistry gathered = Common::Content::GatherContentRegistry( cache, progress );
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
            Detail::Get_().Dirty = true;
        return Common::MakeSuccess( bound );
    }

    Common::ResultStr<RefreshOutcome> Refresh( AssetManager& manager, Core::WorkSink* const sink )
    {
        RefreshOutcome outcome;

        // THE COOK'S WALK, AND THE ONLY CONTENT SCAN LEFT IN THIS ENGINE. It is here rather than in
        // the boot on purpose: `[ContentScan] boot finished` is the number this tier is judged by, and
        // a walk before that line would have made the registry a place the cost moved to rather than a
        // place it stopped being paid.
        //
        // Through `Common::Content::ScanContentRoots`, which is the SAME walk the tool, the CI gate
        // and the packager use. It had three copies for about an hour of this task's life, which is
        // one hour more than the shape deserves.
        // Each file is an item of @p sink while its header is read: on a cold disk that read is the cook.
        uint64_t                      reading = 0;
        Common::Content::ScanProgress progress;
        if ( sink )
        {
            progress.Listed = [sink]( const std::size_t files ) { sink->Expect( files ); };
            progress.Begin  = [sink, &reading]( const std::filesystem::path& file )
            { reading = sink->BeginItem( file.filename().string() ); };
            progress.End = [sink, &reading] { sink->FinishItem( reading ); };
        }
        const std::map<std::string, Common::Content::ContentFile> onDisk =
             Common::Content::ScanContentRoots( progress );

        {
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            for ( const auto& [key, file] : onDisk )
            {
                if ( state.Registry.FindByKey( key ) != nullptr )
                    continue;

                auto entry = Common::Content::RegistryRowFor( key, file );
                if ( !entry )
                {
                    LOG_ERROR( "[ContentRegistry] '{}' was found on disk and its header refused: {}", key,
                               entry.GetError() );
                    continue;
                }
                if ( const auto inserted = state.Registry.Insert( entry.GetValue() ); !inserted )
                {
                    LOG_ERROR( "[ContentRegistry] '{}' was found on disk and could not be entered: {}", key,
                               inserted.GetError() );
                    continue;
                }
                ++outcome.Added;
                state.Dirty = true;
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
            Detail::State&                    state = Detail::Get_();
            const std::lock_guard<std::mutex> lock( state.Mutex );

            // THE EDGES ARE NOT LEARNED HERE. A row's dependencies are its header's GUIDs, written by the
            // gather (ContentScan.cpp, RegistryRowFor); a second writer from loaded assets put runtime
            // handles into the same column (a shader's path-derived number among them) and the cache
            // then kept them, so a fresh scan and the cache disagreed about one file.
            for ( const Common::Utils::AssetRegistryEntry& row : state.Registry.Entries() )
                outcome.Edges += row.Dependencies.size();

            // ── BOUNDS: every loaded mesh's box ─────────────────────────────────────────────────────
            //
            // The box the draw side uses (Geometry::LocalBounds over the submeshes), so the partitioner and
            // the renderer cannot disagree about how big a mesh is. The mesh cook states the same box when
            // it writes the file (ContentRegistry::NoteBounds); this is the safety net for a mesh that came
            // from a `git pull` rather than this machine's import. A mesh not loaded this session keeps
            // whatever its row says — the convergence rule the edges follow: learned once, carried after.
            for ( const auto& [metadata, asset] : manager.RegisteredAssets() )
            {
                if ( !asset || !asset->IsReadyForUse() || metadata.AssetType != AssetTypeID::Mesh )
                    continue;
                const auto*                              mesh = dynamic_cast<const MeshAsset*>( asset.get() );
                const Common::Utils::AssetRegistryEntry* row =
                     state.Registry.FindByHandle( static_cast<uint64_t>( metadata.Handle ) );
                if ( mesh == nullptr || row == nullptr )
                    continue;

                std::optional<Common::Math::AABB> box = Geometry::LocalBounds( mesh->GetSubmeshes() );
                if ( Geometry::IsEmpty( *box ) )
                    box.reset();
                if ( !Common::Utils::SameBounds( row->Bounds, box ) )
                {
                    const std::string key = row->Key;
                    state.Registry.SetBounds( key, box );
                    state.Dirty = true;
                }
            }

            for ( const Common::Utils::AssetRegistryEntry& row : state.Registry.Entries() )
                outcome.Bounded += row.Bounds.has_value() ? 1 : 0;

            outcome.Rows = state.Registry.Count();
        }

        if ( Dirty() )
        {
            if ( const auto written = Save(); !written )
                return Common::MakeError<RefreshOutcome>( written.GetError() );
            outcome.Written = true;
        }

        return Common::MakeSuccess( RefreshOutcome( outcome ) );
    }
} // namespace Desert::Assets::ContentRegistry
