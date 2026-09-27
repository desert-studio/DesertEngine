#include "AssetPreloader.hpp"

#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/ShaderCompiler/ShaderSpirvCache.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <array>
#include <chrono>

#include "Shader/ShaderAsset.hpp"
#include "Mesh/AnimationAsset.hpp"
#include "StringTableAsset.hpp"

namespace Desert::Assets
{
    // THE EXTENSION TABLES THAT STOOD HERE ARE GONE, AND THAT IS THE POINT OF T2.4.
    //
    // Sixteen `constexpr std::array<std::string_view, 1>` constants and sixteen content roots lived in
    // this file, one pair per scan, and they were the project's only statement of what content IS. That
    // was survivable while the boot walked the directories itself — the call site WAS the list. It stops
    // being survivable the moment the enumeration moves to COOK time and the boot reads a file: the
    // producer and the consumer are then in different programs, and a kind one knows and the other does
    // not is content that exists in the editor and is missing from the shipped game. The census is
    // `Common/Content/ContentKinds.hpp` now, read by the cook's walk and by every scan below.
    //
    // What stayed at the call sites is the kind -> C++ CLASS pairing, because that is not expressible in
    // a constexpr row (a template argument is not data) and because it has never been duplicated: the
    // cook's walk knows only roots and extensions and never constructs an asset.

    AssetPreloader::AssetPreloader( const std::shared_ptr<AssetManager>& assetManager,
                                    Animation::AnimationLibrary&         animationLibrary )
         : m_AssetManager( assetManager ), m_AnimationLibrary( &animationLibrary )
    {
    }

    namespace
    {
        // RETURNS THE NUMBER OF FILES IT MATCHED, and only one caller reads it — the `.anim` scan, whose
        // count is what `Animation::PopulateLibrary` needs to tell "this project has no clips" from "the
        // clips never arrived". It is deliberately not `[[nodiscard]]`: the other ten call sites have
        // nothing to do with the number, and a warning at each of them would be noise standing in for a
        // rule that applies to one of them.
        // The kinds `PreloadCookedAssetsAndMaterials` works over: since AL1-5 only the clip rows the
        // animation library indexes (meshes and skeletons are discovered on demand by MeshService).
        constexpr std::array kCookedAssetKinds = { Common::Content::ContentKind::Animation };

        // Progress across several ProcessAssetKind calls: one count of the whole call's rows.
        struct RowProgress
        {
            const ItemProgress* Report = nullptr;
            std::size_t         Done   = 0;
            std::size_t         Total  = 0;
        };

        template <typename AssetType, typename... Args>
        size_t ProcessAssetKind( Common::Content::ContentKind       kind,
                                 const std::weak_ptr<AssetManager>& assetManager, AssetPriority priority,
                                 RowProgress* rows, Args&&... args )
        {
            size_t matched = 0;

            // THE CANDIDATES COME FROM THE COOKED REGISTRY, NOT FROM A DIRECTORY WALK, and this one
            // line is what GAP_ANALYSIS T2.4 is. It used to be
            // `ListFilesRecursive( root )` filtered by extension — eight roots, sixteen walks, every
            // boot, on both hosts, and the only thing in the engine that minted handles wholesale.
            // `Common::AssetPathIndex` made the result observable (`N handle(s) can name their own
            // path`) and said in its own header that inverting the hash was the PRECONDITION for
            // removing the walk rather than the removal; this is the removal.
            //
            // The registry's rows were published into `AssetPathIndex` before this ran, so every
            // handle already names its file. What this loop still does is build the SHELLS the Content
            // Browser, the pickers, the thumbnail sweep and the drag-and-drop targets read.
            for ( const auto& candidate : ContentRegistry::FilesOfKind( kind ) )
            {
                ++matched;
                if ( rows && rows->Report )
                    ReportItem( *rows->Report, candidate.filename().string(), rows->Done++, rows->Total );

                if ( auto manager = assetManager.lock() )
                {
                    // Assets are ALWAYS registered under their full (project-rooted) path. The old
                    // "strip the Textures/ prefix for skyboxes" hack forced every consumer to re-glue
                    // the prefix back (SceneEnvironment did) — path composition belongs to the asset
                    // layer, not to engine draw code.
                    const Common::Filepath path = candidate;
                    auto asset = manager->CreateAsset<AssetType>( priority, path, std::forward<Args>( args )... );

                    // NULL IS A REACHABLE ANSWER HERE, and this line used to go straight to
                    // `asset->GetMetadata()`. `AssetManager::CreateAsset` returns nullptr when the eager
                    // load fails (AssetManager.hpp — it logs the parse error and gives back nothing), so
                    // ONE malformed content file under a scanned root crashed the editor before its first
                    // frame, on a null dereference, for every kind this scanner loads eagerly: textures,
                    // materials, skyboxes, shaders, clips, and all four cloud kinds. Found on 2026-09-09
                    // with a deliberately broken `.anim`, which was the first malformed file the project
                    // had ever had to survive — the repository shipped none, so nothing had reached it.
                    //
                    // Continue rather than abort: one unreadable file is not a reason to start with no
                    // content at all, and the count above still includes it, which is what lets a caller
                    // say "the scan found N and only M arrived" (see Animation::PopulateLibrary).
                    if ( !asset )
                    {
                        LOG_ERROR( "'{}' is a row in the cooked asset registry and could not be loaded, so "
                                   "it is NOT in the project; the parse error is logged above. Everything "
                                   "that references it will resolve to nothing.",
                                   path.string() );
                        continue;
                    }

                    if ( !asset->GetMetadata().IsValid() )
                    {
                        LOG_ERROR( "Asset metadata is invalid for: {}", path.string() );
                    }
                }
            }
            return matched;
        }
    } // namespace

    void AssetPreloader::PreloadCookedAssetsAndMaterials( const ItemProgress& progress )
    {
        // TEXTURES AND MATERIALS ARE NOT HERE (AL1-4): TextureService and MaterialService discover a handle
        // from its content-registry row on first use and read it through AsyncAssetLoader. NEITHER ARE
        // MESHES OR SKELETONS (AL1-5): MeshService discovers a mesh the same way, and its rig by the
        // registry's Rig tag, so no shell of either kind is created here.

        // The count is kept because the animation library's population needs it, and needing it is what
        // makes the ordering a compile-time fact rather than a line-order convention: `PopulateLibrary` at
        // the tail of this function cannot be moved above this statement, because its argument would not
        // exist yet. See Animation::PopulateLibrary for the defect that argument is there to state.
        // Clips are NOT created here: the library indexes their registry rows and the loader reads a
        // clip when an animator first names it (AL1-6).
        const size_t animationFilesFound = ContentRegistry::Rows( Common::Content::ContentKind::Animation ).size();
        ReportItem( progress, "animation library", 0, animationFilesFound );

        if ( auto manager = m_AssetManager.lock() )
        {
            // THE FOURTH REGISTER LOOP, and the reason it is here rather than in a layer. The animation
            // library is an index over clip assets exactly as the three services above are indexes over
            // theirs; it was the only one a HOST published to, and that is how one host ended up with a
            // copy of the loop, the other with none at all, and the editor's copy running a whole startup
            // stage before the scan that finds the clips. Its position among these three is free — a clip
            // names no texture, mesh or material and none of them names a clip.
            if ( const auto populated =
                      Animation::PopulateLibrary( *manager, *m_AnimationLibrary, animationFilesFound );
                 !populated )
            {
                LOG_ERROR( "[AnimationLibrary] {}", populated.GetError() );
            }
        }
    }

    void AssetPreloader::ReloadCooked()
    {
        // Re-scan and re-register, and the scan is NOT confined to the cooked tree even though this
        // function's name is: PreloadCookedAssetsAndMaterials also walks MATERIAL_PATH, which is editable
        // project content. The name is the editor command's ("Rebuild Cooked Assets"), and the extra root
        // is deliberate — a re-cook that rebuilt runtime materials against reloaded textures while leaving
        // newly authored .demat files unregistered would be a rebuild that misses half of what the
        // materials it rebuilds are made of. Said out loud because the comment that stood here promised
        // "cooked files" and the code has never meant only those.
        //
        // Register reloads texture pixels from source and rebuilds runtime materials against the new
        // images (textures are re-registered before materials, so no dangling images).
        PreloadCookedAssetsAndMaterials();
    }

    void AssetPreloader::PreloadSkyboxes()
    {
        // SCANNED, NOT BAKED — and this is the same argument the three cloud stages below make, on the
        // stage that turned out to be paying the most for it.
        //
        // The loop that stood here called `SkyboxService::Register` on every `.hdr` in the project,
        // referenced or not. `Register` constructs a `MaterialSkybox`, whose constructor runs
        // `EnvironmentManager::Create`: a panorama read, three compute chains and a GGX mip convolution,
        // submitted and waited on. Its comment named the benefit it bought — "selecting an HDR skybox in
        // the editor is instant (no per-select compute stall)" — and that benefit was real. What it did
        // not say is what the eagerness cost when nobody selects anything: measured on this machine the
        // stage was **320.0 ms of a 352.4 ms staged boot**, 90.8 % of it, for ONE 32 KB `.hdr`, and it
        // held **109 391 360 B (104.3 MiB)** of device memory for the rest of the session. Without the
        // loop the stage is 0.1 ms and the staged boot is 45.7 ms. A second, unreferenced `.hdr` of
        // 8 MiB added 1119 ms and another 104 MiB on top, so the cost is per FILE and grows with the
        // content browser rather than with what the project draws.
        //
        // WHAT IT COSTS BACK, named rather than waved away: the work does not disappear, it moves to the
        // moment somebody asks for that skybox. Measured on SKY_HdrOrientation, whose scene DOES name
        // the file, the bake now runs inside the scene load and takes **234 ms** there — the same work,
        // paid once, by the scene that wanted it. Selecting an HDR skybox in the Details panel pays the
        // same 234 ms the first time that asset is chosen in a session and nothing afterwards.
        //
        // NO NEW MECHANISM WAS NEEDED TO MAKE THIS SAFE, which is the difference from the cloud stages
        // and the reason this is a deletion rather than a programme. Every site that binds a skybox
        // handle already builds the environment itself if the service has not got one — the scene
        // deserialiser (`ComponentRegistry.cpp`, `FromPath` for "SkyboxAsset"), the component's own
        // picker and the material editor's cubemap slot, each with its own `WaitDeviceIdle` before the
        // bake. So the work now happens once, for the asset somebody actually asked for, in the frame
        // they asked in; the Skybox component's panel already draws "Preview starting — the cubemap is
        // baking" for exactly that moment. The relation is pinned by
        // `Desert/Tests/Editor/AssetPreloadCensus` so that removing one of those three on-demand
        // registrations cannot quietly restore the old defect.
        //
        // THE SCAN ITSELF STAYS and is not vestigial: it mints every `.hdr`'s handle, which is what lets
        // the picker's dropdown offer the project's skyboxes (it lists `FindAllByType<SkyboxAsset>()`
        // from the asset manager, not the service) and what a scene's stored reference resolves against.
        ProcessAssetKind<SkyboxAsset>( Common::Content::ContentKind::Skybox, m_AssetManager, AssetPriority::Medium,
                                       nullptr );
    }

    }

    void AssetPreloader::PreloadStringTables()
    {
        // LOADING IS PUBLISHING for this type: StringTableAsset::Load hands its rows to the process-wide
        // Localization lookup, so there is no register loop after the scan the way the cloud stages have
        // one. That is deliberate — a table that parsed but was not published would be an asset reporting
        // success while every key it owns resolved as missing, which is the empty-successful-answer shape.
        //
        // ORDER IS FREE: a table names no other asset and no other asset names it. It is FIRST among the
        // optional stages anyway, because a missing translation is visible on the very first frame drawn
        // and the log line it produces is much easier to read before the rest of the content arrives.
        //
        // A PROJECT WITH NO Localization/ FOLDER IS NOT AN ERROR. It is a project whose UI is authored in
        // literals, which is every project that predates this stage; the scan matches nothing, no table is
        // published, and every literal element draws exactly what it drew before.
        ProcessAssetKind<StringTableAsset>( Common::Content::ContentKind::StringTable, m_AssetManager,
                                            AssetPriority::High, nullptr );
    }

    std::size_t AssetPreloader::CookedAssetRowCount()
    {
        std::size_t rows = 0;
        for ( const Common::Content::ContentKind kind : kCookedAssetKinds )
            rows += ContentRegistry::FilesOfKind( kind ).size();
        return rows;
    }

    std::size_t AssetPreloader::ShaderRowCount()
    {
        return ContentRegistry::FilesOfKind( Common::Content::ContentKind::Shader ).size();
    }

    void AssetPreloader::PreloadShaders( const ItemProgress& progress, const StopRequested& stop )
    {
        // Timed as a phase: Register() compiles every stage of every pass, so this line is the whole
        // "shader startup cost" in one number — against it, the per-miss lines ShaderCompiler prints
        // say how much was real compilation rather than cache reads.
        const auto start = std::chrono::steady_clock::now();

        ProcessAssetKind<ShaderAsset>( Common::Content::ContentKind::Shader, m_AssetManager, AssetPriority::Medium,
                                       nullptr );
        const auto                   assetsLoaded = std::chrono::steady_clock::now();
        const Core::ShaderPhaseTimes before       = Core::ReadShaderPhaseTimes();

        size_t count = 0;
        if ( auto manager = m_AssetManager.lock() )
        {
            const auto shaders = manager->FindAllByType<Assets::ShaderAsset>();
            for ( const auto& [handle, shaderAsset] : shaders )
            {
                if ( stop && stop() )
                {
                    LOG_INFO( "[AssetPreloader] shader preload stopped on request after {} of {} program(s)",
                              count, shaders.size() );
                    break;
                }
                ReportItem( progress, shaderAsset->GetMetadata().Filepath.filename().string(), count,
                            shaders.size() );
                Runtime::ResourceRegistry::GetShaderService()->Register( shaderAsset );
                ++count;
            }
        }

        const auto ms =
             std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - start )
                  .count();
        LOG_INFO( "[AssetPreloader] {} shader program(s) ready in {} ms", count, ms );
        // The split of that total. "other" is Register()'s own bookkeeping outside every timed phase; the
        // phases are summed over the programs registered HERE, so the line adds up to the total above.
        Core::ShaderPhaseTimes during  = Core::ReadShaderPhaseTimes();
        double                 timedMs = 0.0;
        for ( size_t i = 0; i < during.Nanoseconds.size(); ++i )
        {
            during.Nanoseconds[i] -= before.Nanoseconds[i];
            during.Calls[i] -= before.Calls[i];
            timedMs += during.Milliseconds( static_cast<Core::ShaderPhase>( i ) );
        }
        const double assetMs = std::chrono::duration<double, std::milli>( assetsLoaded - start ).count();
        LOG_INFO( "[AssetPreloader] shader phases: asset load {:.1f} ms, {}, other {:.1f} ms", assetMs,
                  Core::FormatShaderPhaseTimes( during ), static_cast<double>( ms ) - assetMs - timedMs );
        // Cumulative for the process, which at this point is startup: a cold cache shows as hits 0.
        const Core::ShaderCacheCounts cache = Core::ReadShaderCacheCounts();
        LOG_INFO( "[ShaderCache] shader map {} hit(s) / {} miss(es); SPIR-V {} hit(s), {} compiled, {} store "
                  "failure(s) in {}",
                  cache.MapHits, cache.MapMisses, cache.Hits, cache.Compiled, cache.StoreFailures,
                  Core::ShaderCacheDir().string() );
    }

} // namespace Desert::Assets