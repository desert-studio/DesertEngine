#include "AssetPreloader.hpp"

#include <Engine/Assets/ContentRegistry.hpp>

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

#include <array>
#include <chrono>

#include "Shader/ShaderAsset.hpp"
#include "Mesh/StaticMeshAsset.hpp"
#include "Mesh/SkinnedMeshAsset.hpp"
#include "Mesh/AnimationAsset.hpp"
#include "TextureAsset.hpp"
#include "CloudNoiseVolumeAsset.hpp"
#include "CloudTypeAsset.hpp"
#include "CloudModellingVolumeAsset.hpp"
#include "CloudLayoutAsset.hpp"
#include "AnimGraphAsset.hpp"
#include "ControlRigAsset.hpp"
#include "RetargetAsset.hpp"
#include "UIThemeAsset.hpp"
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
        // The kinds `PreloadCookedAssetsAndMaterials` creates, in its order: the stage's work, counted.
        constexpr std::array kCookedAssetKinds = {
             Common::Content::ContentKind::StaticMesh, Common::Content::ContentKind::Texture,
             Common::Content::ContentKind::Animation,  Common::Content::ContentKind::Skeleton,
             Common::Content::ContentKind::Material,   Common::Content::ContentKind::SkinnedMesh };

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
        RowProgress rows{ &progress, 0, CookedAssetRowCount() };
        // Meshes are scanned as UNPARSED shells (loadAfterCreate=false): the handle is path-derived in the
        // ctor, so the big .stmesh parse + GPU build are deferred to the first Get (lazy). Textures/materials
        // are cheap to parse (small metadata) so they load now to expose their stored handle / external id,
        // but their GPU build is still deferred (RegisterAsset, below).
        ProcessAssetKind<StaticMeshAsset>( Common::Content::ContentKind::StaticMesh, m_AssetManager,
                                           AssetPriority::Low, &rows,
                                           /*loadAfterCreate=*/false );

        ProcessAssetKind<TextureAsset>( Common::Content::ContentKind::Texture, m_AssetManager, AssetPriority::Low,
                                        &rows );

        // The count is kept because the animation library's population needs it, and needing it is what
        // makes the ordering a compile-time fact rather than a line-order convention: `PopulateLibrary` at
        // the tail of this function cannot be moved above this statement, because its argument would not
        // exist yet. See Animation::PopulateLibrary for the defect that argument is there to state.
        const size_t animationFilesFound = ProcessAssetKind<AnimationAsset>(
             Common::Content::ContentKind::Animation, m_AssetManager, AssetPriority::Low, &rows );

        ProcessAssetKind<SkeletonAsset>( Common::Content::ContentKind::Skeleton, m_AssetManager,
                                         AssetPriority::Low, &rows );

        // Materials are editable CONTENT (the project's Materials/ dir): imported (per-mesh
        // subfolders) and editor-created both land here, in the unified .demat format.
        ProcessAssetKind<SurfaceMaterialAsset>( Common::Content::ContentKind::Material, m_AssetManager,
                                                AssetPriority::Low, &rows );

        ProcessAssetKind<SkinnedMeshAsset>( Common::Content::ContentKind::SkinnedMesh, m_AssetManager,
                                            AssetPriority::Low, &rows,
                                            /*loadAfterCreate=*/false );

        if ( auto manager = m_AssetManager.lock() )
        {
            // WHAT THESE THREE LOOPS ARE FOR, now that it is no longer "so that scene loading works".
            //
            // They register EVERY asset under the two content roots, including the great majority no scene
            // references: that is what the Content Browser, the thumbnail sweep, the material and mesh
            // pickers and the drag-and-drop targets read. A scene's OWN references are registered by the
            // scene parse itself (Engine/Core/Serialize/ComponentRegistry.cpp — search
            // EnsureMeshRegistered), which is where they belong, because that is the only place that knows
            // a scene asked for them.
            //
            // IT USED TO BE BOTH, AND ONLY ONE OF THE TWO JOBS WAS WRITTEN DOWN. The parse registered a
            // reference only when it CREATED the record, so every reference to an asset these loops had
            // already created was resolved to a live handle no service could answer for — and nothing
            // broke, only because `EditorLayer::OnUpdate` holds every scene load until these stages have
            // run. That ordering is still true and still wanted; it is no longer LOAD-BEARING, and a
            // safety net nobody can see is a safety net somebody removes.
            //
            // Register SHELLS only — the GPU build (texture upload / mesh buffers / material instance) is
            // deferred to the first Get (lazy, cascades from a spawned entity). Textures/materials are
            // loaded first (cheap metadata) so their stored handle / external id is known for the map key.
            for ( const auto& [handle, textureAsset] : manager->FindAllByType<Assets::TextureAsset>() )
            {
                if ( !textureAsset->IsReadyForUse() )
                    textureAsset->Load();
                Runtime::ResourceRegistry::GetTextureService()->RegisterAsset( textureAsset );
            }

            for ( const auto& [handle, meshAsset] : manager->FindAllByType<Assets::MeshAsset>() )
            {
                // The manager travels WITH the shell. A .skmesh names its skeleton by a signature stored
                // inside the file, so the resolve CreateAsset already ran above saw a signature of 0 and
                // bound nothing; the deferred load is the first moment the answer exists, and this is what
                // lets it ask again. Without it a skinned mesh loaded from a scene is invisible and says
                // "MeshFactory: Skeleton dependency invalid" once per frame forever.
                if ( const auto registered = Runtime::ResourceRegistry::GetMeshService()->RegisterAsset(
                          meshAsset, m_AssetManager ); // unparsed shell
                     !registered )
                {
                    LOG_ERROR( "Mesh shell '{}' could not be registered: {}",
                               meshAsset->GetMetadata().Filepath.string(), registered.GetError() );
                }
            }

            for ( const auto& [handle, materialAsset] : manager->FindAllByType<Assets::MaterialAsset>() )
            {
                if ( !materialAsset->IsReadyForUse() )
                    materialAsset->Load();
                Runtime::ResourceRegistry::GetMaterialService()->RegisterAsset( materialAsset );
            }

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

    void AssetPreloader::PreloadCloudNoiseVolumes()
    {
        // ANNOUNCED, NOT READ — and the comment this replaces is the reason the whole tier exists.
        //
        // It said: "Loaded eagerly, unlike meshes: a volume is 8 MiB of bytes with no parse to speak of,
        // and the renderer needs its contents on the first frame the component asks for it. Deferring
        // would buy a stall exactly where the sky first appears." Every clause of that was true. What it
        // did not say is what the eagerness cost when the component never asks: measured on this machine
        // this stage was **1312.7 ms of a 5707.0 ms boot**, `CloudNoise_Default.dcnv` alone **607.12 ms
        // by its own time**, and a scene with no clouds paid all of it.
        //
        // The stall the comment feared is real, and it is not answered by making the read lazy — that
        // only moves it into a frame. It is answered by making the read ASYNCHRONOUS and by giving "not
        // here yet" somewhere to live: `CloudNoiseService::Require` returns Pending, the read runs on a
        // `JobSystem` worker, and the host holds its loading overlay up until the content it asked for
        // has settled. The cost stays in the loading screen where it belongs and stops being paid by
        // scenes that do not want it.
        //
        // THE SCAN ITSELF STAYS, and it is not vestigial: it is what mints every `.dcnv`'s handle, so
        // the path->handle index still answers for a volume nothing has read (`Common::AssetPathIndex`),
        // and the Content Browser and the component slot can still OFFER the project's volumes.
        ProcessAssetKind<CloudNoiseVolumeAsset>( Common::Content::ContentKind::CloudNoiseVolume, m_AssetManager,
                                                 AssetPriority::Medium, nullptr, /*loadAfterCreate=*/false );

        if ( auto manager = m_AssetManager.lock() )
        {
            auto* service = Runtime::ResourceRegistry::GetCloudNoiseService();
            for ( const auto& [handle, volumeAsset] : manager->FindAllByType<Assets::CloudNoiseVolumeAsset>() )
            {
                service->Announce( volumeAsset );

                // The default is chosen by FILE NAME, and it is a project-owned file rather than something
                // compiled in: a project that ships its own CloudNoise_Default.dcnv replaces the engine's
                // without touching code, which is the same way every other built-in default here works.
                //
                // Note it is nominated from the NAME and not from anything inside the file, which is what
                // lets this still work when nothing has been read.
                if ( volumeAsset->GetMetadata().Filepath.filename().string() == kCloudNoiseDefaultVolumeName )
                    service->SetDefault( handle );
            }
        }
    }

    void AssetPreloader::PreloadCloudTypes()
    {
        // Loaded eagerly like the volumes, and for a smaller version of the same reason: a type is a few
        // hundred bytes of JSON, the renderer needs its numbers on the first frame the layer asks for
        // them, and a scene that names one must find it already there rather than resolve to the built-in
        // default for the first second of every session.
        //
        // THERE IS NO "DEFAULT TYPE" FILE to nominate here, unlike the volumes. The empty slot resolves to
        // Assets::CloudTypeDefaultShape — twelve numbers compiled in — because a type costs nothing to
        // synthesise where a 128^3 volume costs ten seconds, and because the sky of a project that has
        // deleted every file in Clouds/Types must still be the sky it was.
        ProcessAssetKind<CloudTypeAsset>( Common::Content::ContentKind::CloudType, m_AssetManager,
                                          AssetPriority::Medium, nullptr );

        if ( auto manager = m_AssetManager.lock() )
        {
            auto* service = Runtime::ResourceRegistry::GetCloudTypeService();
            for ( const auto& [handle, typeAsset] : manager->FindAllByType<Assets::CloudTypeAsset>() )
            {
                if ( const auto result = service->Register( typeAsset ); !result )
                    LOG_ERROR( "[Clouds] Cloud type '{}' could not be registered: {}",
                               typeAsset->GetMetadata().Filepath.string(), result.GetError() );
            }
        }
    }

    void AssetPreloader::PreloadUIThemes()
    {
        // Loaded eagerly for the same reason a cloud type is: a theme is a few kilobytes of JSON, the
        // first frame of a themed canvas needs its numbers, and a canvas that names one must find it
        // already there rather than draw its elements' own colours for the first second of every session
        // — which would look exactly like a theme that does not work.
        ProcessAssetKind<UIThemeAsset>( Common::Content::ContentKind::UITheme, m_AssetManager,
                                        AssetPriority::Medium, nullptr );

        if ( auto manager = m_AssetManager.lock() )
        {
            auto* service = Runtime::ResourceRegistry::GetUIThemeService();
            for ( const auto& [handle, themeAsset] : manager->FindAllByType<Assets::UIThemeAsset>() )
            {
                if ( const auto result = service->Register( themeAsset ); !result )
                    LOG_ERROR( "[UI] Theme '{}' could not be registered: {}",
                               themeAsset->GetMetadata().Filepath.string(), result.GetError() );
            }
        }
    }

    void AssetPreloader::PreloadControlRigs()
    {
        // Loaded eagerly for the reason a cloud type is: a rig is a few kilobytes of JSON and the entity's
        // rig slot has to be able to OFFER the project's rigs, which it does by asking the manager for
        // every asset of this type. There is no service register loop beside this call the way the cloud
        // stages have one: a rig has no process-wide runtime form — the pipeline stage is built per
        // ENTITY, against that entity's own skeleton, by AnimationECSSystem.
        ProcessAssetKind<ControlRigAsset>( Common::Content::ContentKind::ControlRig, m_AssetManager,
                                           AssetPriority::Medium, nullptr );
    }

    void AssetPreloader::PreloadAnimGraphs()
    {
        // Loaded eagerly for the RIG's reason and not the shader graph's, and the difference is the
        // decision. A `.dgraph` gets no preloader at all: nothing but an open editor window ever reads one,
        // so parsing every graph in the project at boot would be work for a reader that does not exist. A
        // `.danimgraph` is named by a COMPONENT SLOT, and that slot has to be able to OFFER the project's
        // graphs — which it does by asking the manager for every asset of this type. An entity's own graph
        // is resolved by path at scene load whether or not this ran; what this buys is the picker.
        //
        // There is no service register loop beside this call: a graph has no process-wide runtime form —
        // the evaluator is per ENTITY, built by AnimationECSSystem from the object this asset owns.
        ProcessAssetKind<AnimGraphAsset>( Common::Content::ContentKind::AnimGraph, m_AssetManager,
                                          AssetPriority::Medium, nullptr );
    }

    void AssetPreloader::PreloadRetargets()
    {
        // Loaded eagerly for the rig's reason — the entity's retarget slot has to be able to OFFER the
        // project's retargets, which it does by asking the manager for every asset of this type — and for
        // one more of its own: loading is what runs `ResolveDependencies`, which is what binds the source
        // rig. A retarget registered but not read is a retarget with no source rig, and the character
        // naming it plays its clip on its own proportions with nothing said.
        //
        // There is no service register loop beside this call: a retarget has no process-wide runtime form
        // — the retargeter is per ENTITY, against that entity's own skeleton, built by AnimationECSSystem.
        ProcessAssetKind<RetargetAsset>( Common::Content::ContentKind::Retarget, m_AssetManager,
                                         AssetPriority::Medium, nullptr );
    }

    void AssetPreloader::PreloadCloudModellingVolumes()
    {
        // ANNOUNCED, NOT READ. The comment this replaces said the reads were free because there is "no
        // parse to speak of"; measured, this stage cost **913.0 ms of a 5707.0 ms boot** for three
        // sculpted bodies of 4 MiB each, and a scene with no hero cloud in it paid every millisecond.
        // The service's own header already promised the right rule for the ATLAS — "a frame pays 4.00 MiB
        // for each body an entity actually names, not for the library" — and the boot was the one place
        // that rule did not hold.
        //
        // NO DEFAULT IS NOMINATED, unlike the noise volumes, and the absence is the decision: an empty
        // hero-cloud slot means the artist has not chosen a body, and the right answer is no cloud rather
        // than a cloud they did not put there. `CloudModellingService::RequireBody` says the same thing by
        // answering Null — never Pending — for an empty handle.
        ProcessAssetKind<CloudModellingVolumeAsset>( Common::Content::ContentKind::CloudModellingVolume,
                                                     m_AssetManager, AssetPriority::Medium, nullptr,
                                                     /*loadAfterCreate=*/false );

        if ( auto manager = m_AssetManager.lock() )
        {
            auto* service = Runtime::ResourceRegistry::GetCloudModellingService();
            for ( const auto& [handle, bodyAsset] : manager->FindAllByType<Assets::CloudModellingVolumeAsset>() )
                service->Announce( bodyAsset );
        }
    }

    void AssetPreloader::PreloadCloudLayouts()
    {
        // ANNOUNCED, NOT READ, and this stage is the clearest case in the project for why. Measured on
        // this machine it cost **689.0 ms of a 5707.0 ms boot** to read ten paintings totalling 10.3 MiB
        // — and EVERY SCENE IN THIS REPOSITORY LEAVES BOTH LAYOUT SLOTS EMPTY, so every one of those
        // milliseconds was spent on pixels nothing points at. The comment this replaces called the stage
        // "cheaper than any of them", which was true per file and beside the point.
        //
        // NO DEFAULT IS NOMINATED, and here the absence is the shipped state rather than an edge case: an
        // empty slot means the sky places its clouds procedurally, which is what every scene in this
        // repository does and what the phase's acceptance criterion requires stay byte-identical. That is
        // also why an empty handle resolves to Null and never to Pending — there is nothing to wait for.
        ProcessAssetKind<CloudLayoutAsset>( Common::Content::ContentKind::CloudLayout, m_AssetManager,
                                            AssetPriority::Medium, nullptr,
                                            /*loadAfterCreate=*/false );

        if ( auto manager = m_AssetManager.lock() )
        {
            auto* service = Runtime::ResourceRegistry::GetCloudLayoutService();
            for ( const auto& [handle, layoutAsset] : manager->FindAllByType<Assets::CloudLayoutAsset>() )
                service->Announce( layoutAsset );
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

    void AssetPreloader::PreloadShaders( const ItemProgress& progress )
    {
        // Timed as a phase: Register() compiles every stage of every pass, so this line is the whole
        // "shader startup cost" in one number — against it, the per-miss lines ShaderCompiler prints
        // say how much was real compilation rather than cache reads.
        const auto start = std::chrono::steady_clock::now();

        ProcessAssetKind<ShaderAsset>( Common::Content::ContentKind::Shader, m_AssetManager, AssetPriority::Medium,
                                       nullptr );

        size_t count = 0;
        if ( auto manager = m_AssetManager.lock() )
        {
            const auto shaders = manager->FindAllByType<Assets::ShaderAsset>();
            for ( const auto& [handle, shaderAsset] : shaders )
            {
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
    }

} // namespace Desert::Assets