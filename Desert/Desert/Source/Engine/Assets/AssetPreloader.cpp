#include "AssetPreloader.hpp"
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>

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
#include "UIThemeAsset.hpp"

namespace Desert::Assets
{
    // The two mesh extensions come from Common::Constants::Extensions rather than being spelled again
    // here. They used to be literals in this file AND named constants in Constants.hpp, and the two
    // disagreed — the named ones were swapped — for as long as nobody read them. One value, one home.
    const std::array<std::string_view, 1> SUPPORTED_SKINNED_MESH_EXTENSIONS = {
         Common::Constants::Extensions::SKINNED_MESH };
    const std::array<std::string_view, 1> SUPPORTED_STATIC_MESH_EXTENSIONS = {
         Common::Constants::Extensions::STATIC_MESH };
    constexpr std::array<std::string_view, 1> SUPPORTED_SKELETON_EXTENSIONS     = { ".skeleton" };
    // (now written by import too). Both register as SurfaceMaterialAsset.
    constexpr std::array<std::string_view, 1> SUPPORTED_MATERIAL_EXTENSIONS     = { ".demat" };
    constexpr std::array<std::string_view, 1> SUPPORTED_ANIMATION_EXTENSIONS    = { ".anim" };
    constexpr std::array<std::string_view, 1> SUPPORTED_TEXTURE_EXTENSIONS      = { ".tex" };
    constexpr std::array<std::string_view, 1> SUPPORTED_SKYBOX_EXTENSIONS       = { ".hdr" };
    constexpr std::array<std::string_view, 1> SUPPORTED_SHADERS_EXTENSIONS      = { ".shader" };
    constexpr std::array<std::string_view, 1> SUPPORTED_CLOUD_NOISE_EXTENSIONS  = { ".dcnv" };
    constexpr std::array<std::string_view, 1> SUPPORTED_CLOUD_TYPE_EXTENSIONS   = { ".decloudtype" };
    constexpr std::array<std::string_view, 1> SUPPORTED_CLOUD_BODY_EXTENSIONS   = { ".dcmv" };
    constexpr std::array<std::string_view, 1> SUPPORTED_CLOUD_LAYOUT_EXTENSIONS = { ".dclayout" };
    constexpr std::array<std::string_view, 1> SUPPORTED_UI_THEME_EXTENSIONS     = { ".detheme" };

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
        template <typename AssetType, typename Extensions, typename... Args>
        size_t ProcessAssetFiles( const std::filesystem::path& rootPath, const Extensions& supportedExtensions,
                                  const std::weak_ptr<AssetManager>& assetManager, AssetPriority priority,
                                  Args&&... args )
        {
            size_t matched = 0;
            // Candidates = the loose files on disk PLUS everything a mounted .dpak holds under this
            // root (packaged game: the disk dirs typically do not exist at all), deduplicated with a
            // loose file overriding its pak twin. The enumeration itself is the ONE shared
            // implementation every content scanner uses — the font/icon services once hand-rolled the
            // disk half only, and a packaged game scanned nothing.
            for ( const auto& candidate : Common::Utils::FileSystem::ListFilesRecursive( rootPath ) )
            {
                std::string ext = candidate.extension().string();
                std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );

                if ( std::find( supportedExtensions.begin(), supportedExtensions.end(), ext ) ==
                     supportedExtensions.end() )
                    continue;

                ++matched;

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
                        LOG_ERROR( "'{}' was found by the asset scan and could not be loaded, so it is NOT "
                                   "in the project; the parse error is logged above. Everything that "
                                   "references it will resolve to nothing.",
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

    void AssetPreloader::PreloadCookedAssetsAndMaterials()
    {
        // Meshes are scanned as UNPARSED shells (loadAfterCreate=false): the handle is path-derived in the
        // ctor, so the big .stmesh parse + GPU build are deferred to the first Get (lazy). Textures/materials
        // are cheap to parse (small metadata) so they load now to expose their stored handle / external id,
        // but their GPU build is still deferred (RegisterAsset, below).
        ProcessAssetFiles<StaticMeshAsset>( Common::Constants::Path::MESH_PATH_COOKED,
                                            SUPPORTED_STATIC_MESH_EXTENSIONS, m_AssetManager, AssetPriority::Low,
                                            /*loadAfterCreate=*/false );

        ProcessAssetFiles<TextureAsset>( Common::Constants::Path::TEXTURE_PATH_COOKED,
                                         SUPPORTED_TEXTURE_EXTENSIONS, m_AssetManager, AssetPriority::Low );

        // The count is kept because the animation library's population needs it, and needing it is what
        // makes the ordering a compile-time fact rather than a line-order convention: `PopulateLibrary` at
        // the tail of this function cannot be moved above this statement, because its argument would not
        // exist yet. See Animation::PopulateLibrary for the defect that argument is there to state.
        const size_t animationFilesFound = ProcessAssetFiles<AnimationAsset>(
             Common::Constants::Path::MESH_PATH_COOKED, SUPPORTED_ANIMATION_EXTENSIONS, m_AssetManager,
             AssetPriority::Low );

        ProcessAssetFiles<SkeletonAsset>( Common::Constants::Path::MESH_PATH_COOKED,
                                          SUPPORTED_SKELETON_EXTENSIONS, m_AssetManager, AssetPriority::Low );

        // Materials are editable CONTENT (the project's Materials/ dir): imported (per-mesh
        // subfolders) and editor-created both land here, in the unified .demat format.
        ProcessAssetFiles<SurfaceMaterialAsset>( Common::Constants::Path::MATERIAL_PATH,
                                                 SUPPORTED_MATERIAL_EXTENSIONS, m_AssetManager,
                                                 AssetPriority::Low );

        ProcessAssetFiles<SkinnedMeshAsset>( Common::Constants::Path::MESH_PATH_COOKED,
                                             SUPPORTED_SKINNED_MESH_EXTENSIONS, m_AssetManager,
                                             AssetPriority::Low, /*loadAfterCreate=*/false );

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
        ProcessAssetFiles<SkyboxAsset>( Common::Constants::Path::SKYBOX_PATH, SUPPORTED_SKYBOX_EXTENSIONS,
                                        m_AssetManager, AssetPriority::Medium );

        if ( auto manager = m_AssetManager.lock() )
        {
            for ( const auto& [handle, skyboxAsset] : manager->FindAllByType<Assets::SkyboxAsset>() )
            {
                // Eagerly build + cache each skybox's IBL (radiance / irradiance / prefilter compute) at
                // load — Register() constructs the MaterialSkybox which runs the compute once and caches
                // it. Then selecting an HDR skybox in the editor is instant (no per-select compute stall).
                // Runs after PreloadShaders (the compute shaders must be registered first).
                Runtime::ResourceRegistry::GetSkyboxService()->Register( skyboxAsset );
            }
        }
    }

    void AssetPreloader::PreloadCloudNoiseVolumes()
    {
        // Loaded eagerly, unlike meshes: a volume is 8 MiB of bytes with no parse to speak of, and the
        // renderer needs its contents on the first frame the component asks for it. Deferring would buy a
        // stall exactly where the sky first appears.
        ProcessAssetFiles<CloudNoiseVolumeAsset>( Common::Constants::Path::CLOUD_NOISE_PATH,
                                                  SUPPORTED_CLOUD_NOISE_EXTENSIONS, m_AssetManager,
                                                  AssetPriority::Medium );

        if ( auto manager = m_AssetManager.lock() )
        {
            auto* service = Runtime::ResourceRegistry::GetCloudNoiseService();
            for ( const auto& [handle, volumeAsset] : manager->FindAllByType<Assets::CloudNoiseVolumeAsset>() )
            {
                if ( const auto result = service->Register( volumeAsset ); !result )
                {
                    LOG_ERROR( "[Clouds] Noise volume '{}' could not be uploaded: {}",
                               volumeAsset->GetMetadata().Filepath.string(), result.GetError() );
                    continue;
                }

                // The default is chosen by FILE NAME, and it is a project-owned file rather than something
                // compiled in: a project that ships its own CloudNoise_Default.dcnv replaces the engine's
                // without touching code, which is the same way every other built-in default here works.
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
        ProcessAssetFiles<CloudTypeAsset>( Common::Constants::Path::CLOUD_TYPE_PATH,
                                           SUPPORTED_CLOUD_TYPE_EXTENSIONS, m_AssetManager,
                                           AssetPriority::Medium );

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
        ProcessAssetFiles<UIThemeAsset>( Common::Constants::Path::UI_THEME_PATH, SUPPORTED_UI_THEME_EXTENSIONS,
                                         m_AssetManager, AssetPriority::Medium );

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

    void AssetPreloader::PreloadCloudModellingVolumes()
    {
        // Loaded eagerly like the noise volumes and for the same reason: 4 MiB of bytes with no parse to
        // speak of, and the renderer needs the contents on the first frame a hero cloud asks for them.
        //
        // NO DEFAULT IS NOMINATED, unlike the noise volumes, and the absence is the decision: an empty
        // hero-cloud slot means the artist has not chosen a body, and the right answer is no cloud rather
        // than a cloud they did not put there. Runtime::CloudModellingService::HasBody says the same thing.
        ProcessAssetFiles<CloudModellingVolumeAsset>( Common::Constants::Path::CLOUD_VOLUME_PATH,
                                                      SUPPORTED_CLOUD_BODY_EXTENSIONS, m_AssetManager,
                                                      AssetPriority::Medium );

        if ( auto manager = m_AssetManager.lock() )
        {
            auto* service = Runtime::ResourceRegistry::GetCloudModellingService();
            for ( const auto& [handle, bodyAsset] : manager->FindAllByType<Assets::CloudModellingVolumeAsset>() )
            {
                if ( const auto result = service->Register( bodyAsset ); !result )
                    LOG_ERROR( "[Clouds] Modelling volume '{}' could not be registered: {}",
                               bodyAsset->GetMetadata().Filepath.string(), result.GetError() );
            }
        }
    }

    void AssetPreloader::PreloadCloudLayouts()
    {
        // Loaded eagerly like the volumes beside it, and it is cheaper than any of them: 1.25 MiB at the
        // shipped 512 square, with no parse beyond a CRC. The BAKE needs the pixels the first time a
        // painted layer places a cloud, and the bake runs on the first frame.
        //
        // NO DEFAULT IS NOMINATED, and here the absence is the shipped state rather than an edge case: an
        // empty slot means the sky places its clouds procedurally, which is what every scene in this
        // repository does and what the phase's acceptance criterion requires stay byte-identical.
        ProcessAssetFiles<CloudLayoutAsset>( Common::Constants::Path::CLOUD_LAYOUT_PATH,
                                             SUPPORTED_CLOUD_LAYOUT_EXTENSIONS, m_AssetManager,
                                             AssetPriority::Medium );

        if ( auto manager = m_AssetManager.lock() )
        {
            auto* service = Runtime::ResourceRegistry::GetCloudLayoutService();
            for ( const auto& [handle, layoutAsset] : manager->FindAllByType<Assets::CloudLayoutAsset>() )
            {
                if ( const auto result = service->Register( layoutAsset ); !result )
                    LOG_ERROR( "[Clouds] Cloud layout '{}' could not be registered: {}",
                               layoutAsset->GetMetadata().Filepath.string(), result.GetError() );
            }
        }
    }

    void AssetPreloader::PreloadShaders()
    {
        // Timed as a phase: Register() compiles every stage of every pass, so this line is the whole
        // "shader startup cost" in one number — against it, the per-miss lines ShaderCompiler prints
        // say how much was real compilation rather than cache reads.
        const auto start = std::chrono::steady_clock::now();

        ProcessAssetFiles<ShaderAsset>( Common::Constants::Path::SHADERDIR_PATH,
                                        SUPPORTED_SHADERS_EXTENSIONS, m_AssetManager, AssetPriority::Medium );

        size_t count = 0;
        if ( auto manager = m_AssetManager.lock() )
        {
            for ( const auto& [handle, shaderAsset] : manager->FindAllByType<Assets::ShaderAsset>() )
            {
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