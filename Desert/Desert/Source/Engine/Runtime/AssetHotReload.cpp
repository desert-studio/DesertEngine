#include <Engine/Runtime/AssetHotReload.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Mesh/SurfaceMaterialAsset.hpp>
#include <Engine/Assets/Shader/ShaderAsset.hpp>
#include <Engine/Assets/CloudNoiseVolumeAsset.hpp>
#include <Engine/Assets/CloudTypeAsset.hpp>
#include <Engine/Assets/CloudModellingVolumeAsset.hpp>
#include <Engine/Assets/UIThemeAsset.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/Materials/DataDrivenMaterial.hpp>
#include <Engine/Graphic/Materials/MaterialFactory.hpp>
#include <Engine/Graphic/Materials/Mesh/PBR/MaterialPBR.hpp>
#include <Engine/Graphic/PipelineCache.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Material/MaterialService.hpp>
#include <Engine/Runtime/Services/Shader/ShaderService.hpp>
#include <Engine/Core/ShaderCompiler/DShader/DShaderParser.hpp>
#include <Engine/Core/ShaderCompiler/ShaderCacheKey.hpp>

namespace Desert::Runtime
{
    namespace
    {
        // Refresh every static/skinned mesh component that references @p materialHandle so
        // MeshECSSystem rebuilds its runtime instances against the new runtime material.
        void RefreshComponentsUsingMaterial( Core::Scene* scene, const Assets::AssetHandle& materialHandle )
        {
            if ( !scene )
                return;
            auto& registry = scene->GetRegistry();

            registry.view<ECS::StaticMeshComponent>().each(
                 [&]( ECS::StaticMeshComponent& mesh )
                 {
                     for ( const auto& slot : mesh.MaterialSlots )
                         if ( slot == materialHandle )
                         {
                             mesh.RuntimeMaterialInstances.clear();
                             // Dropped, not emptied: a draw recorded this frame may still hold the old
                             // binding, and it must keep the instances it names alive until it has run.
                             mesh.RuntimeSlots.reset();
                             break;
                         }
                 } );

            registry.view<ECS::SkinnedMeshComponent>().each(
                 [&]( ECS::SkinnedMeshComponent& mesh )
                 {
                     for ( const auto& slot : mesh.MaterialSlots )
                         if ( slot == materialHandle )
                         {
                             mesh.RuntimeMaterialInstances.clear();
                             break;
                         }
                 } );
        }
    } // namespace

    void AssetHotReload::Tick( const Common::Timestep& ts, Assets::AssetManager& assetManager,
                               Core::Scene* scene )
    {
        m_Accum += ts.GetSeconds();
        if ( m_Accum < kPollInterval )
            return;
        m_Accum = 0.0f;

        PollMaterials( assetManager, scene );
        PollShaders( assetManager, scene );
        PollCloudNoiseVolumes( assetManager );
        PollCloudTypes( assetManager );
        PollCloudModellingVolumes( assetManager );
        PollUIThemes( assetManager );
        m_ContentWatch.Poll();
        m_FirstScan = false;
    }

    void AssetHotReload::PollCloudModellingVolumes( Assets::AssetManager& assetManager )
    {
        auto* service = ResourceRegistry::GetCloudModellingService();

        for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::CloudModellingVolumeAsset>() )
        {
            if ( !asset )
                continue;

            const auto& path = asset->GetMetadata().Filepath;
            if ( !TouchWatched( path ) )
                continue;

            const std::string key = path.generic_string();

            // A FAILED RE-READ LEAVES THE OLD VOXELS UPLOADED, deliberately. A half-written file caught
            // mid-bake would otherwise take the hero cloud out of the sky, and the artist would be
            // debugging a disappearance instead of reading the error that is already in the log.
            if ( const auto reloaded = asset->Load(); !reloaded )
            {
                LOG_ERROR( "[HotReload] Cloud modelling volume '{}' could not be re-read: {}", key,
                           reloaded.GetError() );
                continue;
            }

            if ( const auto uploaded = service->Register( asset ); !uploaded )
            {
                LOG_ERROR( "[HotReload] Cloud modelling volume '{}' was re-read but not uploaded: {}", key,
                           uploaded.GetError() );
                continue;
            }

            LOG_INFO( "[HotReload] Cloud modelling volume '{}' reloaded — the next frame marches the new body.",
                      key );
        }
    }

    void AssetHotReload::PollCloudNoiseVolumes( Assets::AssetManager& assetManager )
    {
        auto* service = ResourceRegistry::GetCloudNoiseService();

        for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::CloudNoiseVolumeAsset>() )
        {
            if ( !asset )
                continue;
            const auto& path = asset->GetMetadata().Filepath;

            std::error_code ec;
            const auto      mtime = std::filesystem::last_write_time( path, ec );
            if ( ec )
                continue; // deleted/missing — leave the in-memory volume alone

            const std::string key = path.generic_string();
            auto              it  = m_KnownTimes.find( key );
            if ( it == m_KnownTimes.end() )
            {
                m_KnownTimes[key] = mtime;
                continue; // first sighting — baseline only
            }
            if ( it->second == mtime || m_FirstScan )
            {
                it->second = mtime;
                continue;
            }
            it->second = mtime;
            Assets::ContentRegistry::Update( path );

            // A FAILED RE-READ LEAVES THE OLD VOLUME BOUND, deliberately. The panel writes a volume with a
            // single truncating stream write, so a poll that lands mid-write sees a short file; refusing it
            // and keeping the bytes that are already on the device costs one poll interval, where accepting
            // a half-file would put a seam across the sky and then never mention it again.
            if ( const auto reloaded = asset->Load(); !reloaded )
            {
                LOG_ERROR( "[HotReload] Cloud noise volume '{}' could not be re-read: {}", key,
                           reloaded.GetError() );
                continue;
            }

            if ( const auto uploaded = service->Register( asset ); !uploaded )
            {
                LOG_ERROR( "[HotReload] Cloud noise volume '{}' was re-read but not uploaded: {}", key,
                           uploaded.GetError() );
                continue;
            }

            // Nothing else to notify. VolumetricCloudRenderer asks the service for its volume every frame
            // rather than caching it, so the next frame binds the new image without anyone telling it.
            LOG_INFO( "[HotReload] Cloud noise volume '{}' reloaded — the next frame marches the new one.", key );
        }
    }

    void AssetHotReload::PollCloudTypes( Assets::AssetManager& assetManager )
    {
        auto* service = ResourceRegistry::GetCloudTypeService();

        for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::CloudTypeAsset>() )
        {
            if ( !asset )
                continue;

            const auto& path = asset->GetMetadata().Filepath;
            if ( !TouchWatched( path ) )
                continue;

            const std::string key = path.generic_string();

            // A FAILED RE-READ LEAVES THE OLD NUMBERS REGISTERED, for the same reason the volume above
            // keeps its bytes: the panel writes with one truncating stream write, so a poll that lands
            // mid-write sees half a document. Refusing it costs one poll interval; accepting it would put
            // the built-in default in a slot the artist is actively editing and never say why.
            if ( const auto reloaded = asset->Load(); !reloaded )
            {
                LOG_ERROR( "[HotReload] Cloud type '{}' could not be re-read: {}", key, reloaded.GetError() );
                continue;
            }

            // Re-resolved because the EDIT MAY HAVE BEEN THE NOISE VOLUME. Load parses the new path;
            // nothing binds it to an asset until this runs, and without it a type edited to name a
            // different volume would keep the old one until the next launch.
            asset->ResolveDependencies( assetManager );

            if ( const auto registered = service->Register( asset ); !registered )
            {
                LOG_ERROR( "[HotReload] Cloud type '{}' was re-read but not registered: {}", key,
                           registered.GetError() );
                continue;
            }

            LOG_INFO( "[HotReload] Cloud type '{}' reloaded — the next frame rebuilds its profile table.", key );
        }
    }

    void AssetHotReload::PollUIThemes( Assets::AssetManager& assetManager )
    {
        auto* service = ResourceRegistry::GetUIThemeService();

        for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::UIThemeAsset>() )
        {
            if ( !asset )
                continue;

            const auto& path = asset->GetMetadata().Filepath;
            if ( !TouchWatched( path ) )
                continue;

            const std::string key = path.generic_string();

            // A FAILED RE-READ LEAVES THE OLD TABLE REGISTERED, for the same reason a cloud type does: an
            // editor saving the file writes it in one go, so a poll that lands mid-write sees half a
            // document. Refusing it costs one poll interval; accepting it would strip every themed colour
            // off the screen while the author is editing and never say why.
            if ( const auto reloaded = asset->Load(); !reloaded )
            {
                LOG_ERROR( "[HotReload] UI theme '{}' could not be re-read: {}", key, reloaded.GetError() );
                continue;
            }

            if ( const auto registered = service->Register( asset ); !registered )
            {
                LOG_ERROR( "[HotReload] UI theme '{}' was re-read but not registered: {}", key,
                           registered.GetError() );
                continue;
            }

            LOG_INFO( "[HotReload] UI theme '{}' reloaded — the next frame draws it.", key );
        }
    }

    void AssetHotReload::PollMaterials( Assets::AssetManager& assetManager, Core::Scene* scene )
    {
        auto* materialService = ResourceRegistry::GetMaterialService();

        for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::SurfaceMaterialAsset>() )
        {
            if ( !asset )
                continue;
            const auto& path = asset->GetMetadata().Filepath;

            std::error_code ec;
            const auto      mtime = std::filesystem::last_write_time( path, ec );
            if ( ec )
                continue; // deleted/missing — leave the in-memory asset alone

            const std::string key = path.generic_string();
            auto              it  = m_KnownTimes.find( key );
            if ( it == m_KnownTimes.end() )
            {
                m_KnownTimes[key] = mtime;
                continue; // first sighting — baseline only
            }
            if ( it->second == mtime || m_FirstScan )
            {
                it->second = mtime;
                continue;
            }
            it->second = mtime;
            Assets::ContentRegistry::Update( path );

            // Only the shader NAME is snapshotted before the re-parse. A `wasCustom` flag was taken here
            // too and then never read: whether the asset crossed between PBR and data-driven is already
            // answered below by `classMatches`, which compares the CURRENT `custom` flag against the C++
            // class of each live runtime material — a stronger question, because it also catches a variant
            // built as the wrong class for a reason other than an edit.
            const auto oldShader = asset->GetShaderName();

            if ( const auto res = asset->Load(); !res )
            {
                LOG_ERROR( "[HotReload] Failed to re-parse material '{}': {}", key, res.GetError() );
                continue;
            }
            // Load leaves a stated shader unresolved: its GUID is looked up in the manager, here, because the
            // edit may have been the shader itself.
            asset->ResolveDependencies( assetManager );

            LOG_INFO( "[HotReload] Material '{}' reloaded", key );

            if ( !materialService )
                continue;

            // EVERY built variant, not "the" material: one `.demat` is now a material per vertex path,
            // and a live parameter edit that reached only the static one would leave a character wearing
            // the previous value while the crate beside it updated. Nothing is built here — a path no
            // draw has asked for has nothing to refresh.
            //
            // It looks up THIS handle and does not walk an instance's parent chain, which changes what
            // happens when a material-INSTANCE file is edited — and changes it for the better. `Get` used
            // to resolve an instance to its PARENT's material, so an edited instance had its own params
            // written over the parent's runtime material, and every sibling using that parent silently
            // took them. An instance owns no runtime material, so the list is empty here, and the branch
            // below does the correct thing instead: refresh the components, whose cached instances
            // re-apply their overrides.
            const auto variants = materialService->GetBuiltVariants( handle );

            const bool custom       = asset->UsesCustomShader();
            bool       classMatches = !variants.empty();
            for ( auto* runtime : variants )
                classMatches =
                     classMatches && ( custom ? dynamic_cast<Graphic::DataDrivenMaterial*>( runtime ) != nullptr
                                              : dynamic_cast<Graphic::MaterialPBR*>( runtime ) != nullptr );
            const bool sameShader = classMatches && asset->GetShaderName() == oldShader;

            if ( sameShader && !custom )
            {
                for ( auto* runtime : variants )
                    Graphic::MaterialFactory::ApplyPBRAsset( *static_cast<Graphic::MaterialPBR*>( runtime ),
                                                             *asset );
            }
            else if ( sameShader )
            {
                for ( auto* runtime : variants )
                    Graphic::MaterialFactory::ApplyShaderAsset(
                         *static_cast<Graphic::DataDrivenMaterial*>( runtime ), *asset );
            }
            else
            {
                // Shader (or material class) changed — rebuild the runtime material and refresh
                // every component using it.
                materialService->Invalidate( handle );
                RefreshComponentsUsingMaterial( scene, handle );
            }
        }
    }

    bool AssetHotReload::TouchWatched( const std::filesystem::path& path )
    {
        std::error_code ec;
        const auto      mtime = std::filesystem::last_write_time( path, ec );
        if ( ec )
            return false; // deleted/missing — leave the in-memory version alone

        const std::string key = path.generic_string();
        auto              it  = m_KnownTimes.find( key );
        if ( it == m_KnownTimes.end() )
        {
            m_KnownTimes[key] = mtime; // first sighting — baseline only
            return false;
        }

        const bool changed = it->second != mtime;
        it->second         = mtime;
        // The registry row follows the bytes: a rewritten header may state another GUID, other edges or
        // another size, and the pickers read the row, not the object this poll is about to reload.
        if ( changed )
            Assets::ContentRegistry::Update( path );
        return changed;
    }

    void AssetHotReload::PollShaders( Assets::AssetManager& assetManager, Core::Scene* scene )
    {
        auto* shaderService = ResourceRegistry::GetShaderService();
        if ( !shaderService )
            return;

        for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::ShaderAsset>() )
        {
            if ( !asset )
                continue;
            const auto& path = asset->GetMetadata().Filepath;

            // A shader is its .shader file AND every .glslh that file pulls in — the same closure the
            // SPIR-V cache key hashes, so the two cannot disagree about what a shader is made of.
            // Watching only the .shader was a real hole: a shared header could be edited and nothing
            // recompiled until the next restart, which is the same silent staleness an under-specified
            // cache key produces, arriving through the other door.
            bool changed = TouchWatched( path );
            for ( const auto& include : Core::CollectShaderIncludes( asset->GetShaderContent(), path ) )
            {
                changed = TouchWatched( include ) || changed;
            }

            if ( !changed || m_FirstScan )
                continue;

            const std::string key = path.generic_string();

            if ( const auto res = asset->Load(); !res )
            {
                LOG_ERROR( "[HotReload] Failed to re-read shader '{}': {}", key, res.GetError() );
                continue;
            }

            // Pre-validate the DSL structure: the preprocessor treats a malformed file as a fatal
            // engine error (DESERT_VERIFY), which is right at startup but must not kill a live
            // editor over a half-saved file. GLSL errors inside the stages fail gracefully later.
            if ( Core::Preprocess::DShaderParser::IsDShader( asset->GetShaderContent() ) )
            {
                if ( auto parsed = Core::Preprocess::DShaderParser::Parse( asset->GetShaderContent() );
                     !parsed.IsSuccess() )
                {
                    LOG_ERROR( "[HotReload] Shader '{}': {} — keeping the previous version", key,
                               parsed.GetError() );
                    continue;
                }
            }

            // A VOLUME MEDIUM HAS NO Shader OBJECT TO RELOAD — it is a program FRAGMENT, compiled into
            // the four programs that sample the cloud field. What it has is TEXT, and the service holds
            // a copy of it; refreshing that copy is the whole of hot-reloading a medium. The cloud
            // renderer notices on the next frame, because it compares the variant's content hash rather
            // than the material's handle, and rebuilds the three pipelines it owns.
            if ( shaderService->RefreshMediumSource( handle, asset->GetShaderContent() ) )
            {
                LOG_INFO( "[HotReload] Volume medium '{}' re-read; the cloud programs recompile against "
                          "it on the next frame that resolves the material.",
                          key );
                continue;
            }

            auto shader = shaderService->Get( handle );
            if ( !shader )
                continue;

            if ( const auto res = shader->Reload(); !res )
            {
                // Compile errors land here (and in the Logs panel). Existing pipelines keep the
                // previous working code; save again to retry.
                LOG_ERROR( "[HotReload] Shader '{}' failed to compile: {}", shader->GetName(),
                           res.GetError() );
                continue;
            }

            // AND EVERY LIVE VARIANT OF THE SAME FILE. A cloud march compiled against an authored
            // medium is a different Shader object built from these same bytes; reloading only the
            // registered program would leave it on the code it was built with, and the symptom would be
            // "editing the shader stopped working once I authored a medium" — a staleness that names
            // the wrong cause.
            if ( const int variants = shaderService->ReloadVariantsOf( handle ); variants > 0 )
                LOG_INFO( "[HotReload] Shader '{}': {} variant(s) recompiled with it.", shader->GetName(),
                          variants );

            // Renderer-owned pipelines (the batched PBR/shadow set, every compute pipeline, the
            // fog apply) are built once at init and keep the code they were built with until
            // a restart. That is now merely STALE and no longer unsafe: the pipeline holds strong
            // references to the descriptor set layouts it was built from, so recompiling under it
            // cannot leave it bound to a layout that has been destroyed (see
            // VulkanDescriptorSetLayout.hpp). Said on every recompile, because "the shader did not
            // change anything" is otherwise indistinguishable from "the shader did not compile".
            LOG_INFO( "[HotReload] Shader '{}' recompiled. Graph pipelines pick it up next frame; "
                      "renderer-owned pipelines (compute, batched PBR/shadow, fog apply) keep "
                      "the previous code until the editor is restarted.",
                      shader->GetName() );

            // Rebuild cached pipelines against the new modules.
            if ( scene )
                if ( auto* sceneRenderer = scene->GetSceneRenderer() )
                {
                    Graphic::Renderer::GetInstance().WaitDeviceIdle();
                    sceneRenderer->GetPipelineCache().InvalidateByShader( shader.get() );
                }
        }
    }
} // namespace Desert::Runtime
