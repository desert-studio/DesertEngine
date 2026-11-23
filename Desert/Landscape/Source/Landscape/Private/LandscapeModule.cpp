#include <Landscape/Public/LandscapeECSSystem.hpp>
#include <Landscape/Public/LandscapeGrassTypeService.hpp>
#include <Landscape/Public/LandscapeLayerInfoService.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/LandscapeLayerInfoAsset.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>
#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <memory>
#include <string>

// THE LANDSCAPE MODULE (UE: the Landscape runtime module's StartupModule). What the landscape adds to the engine
// is registered here, through the engine's generic hooks: the layer-info and grass-type services, the
// `.delayerinfo` hot reload, the grass types a layer info keeps reachable, the layer name a terrain material's
// layer input is matched by, and the tiles' and grass's render collector. Engine names only
// Landscape::StartupModule (Engine/Core/Modules.cpp).

namespace Desert::Landscape
{
    namespace
    {
        void WatchLayerInfos( Assets::AssetManager&                            assets,
                              std::vector<std::shared_ptr<Assets::AssetBase>>& watched )
        {
            for ( const auto& [handle, asset] : assets.FindAllByType<Assets::LandscapeLayerInfoAsset>() )
                if ( asset )
                    watched.push_back( asset );
        }

        // No service to notify: LandscapeLayerInfoService hands out the asset's own data, and the landscape
        // system and the paint tool read it every frame / every stroke, so re-reading the asset IS the reload.
        void PollLayerInfos( Assets::AssetManager&                                      assets,
                             const std::function<bool( const std::filesystem::path& )>& changed )
        {
            for ( const auto& [handle, asset] : assets.FindAllByType<Assets::LandscapeLayerInfoAsset>() )
            {
                if ( !asset )
                    continue;
                const auto& path = asset->GetMetadata().Filepath;
                if ( !changed( path ) )
                    continue;
                const std::string key = path.generic_string();
                // A FAILED RE-READ KEEPS THE OLD NUMBERS (the asset's data is replaced only by a parse that
                // succeeded): a poll can land mid-write.
                if ( const auto reloaded = asset->Load(); !reloaded )
                {
                    LOG_ERROR( "[HotReload] Landscape layer info '{}' could not be re-read: {}", key,
                               reloaded.GetError() );
                    continue;
                }
                LOG_INFO( "[HotReload] Landscape layer info '{}' reloaded — the next frame and stroke use it.",
                          key );
            }
        }

        // THE GRASS A LAYER GROWS (JFA-OUTLINE-MISSING): a layer info is a root (a landscape paints with it), and
        // the grass types it names must stay with it, or the sweep releases them and every grass instance
        // vanishes.
        void VisitLayerInfoReferences( Assets::AssetManager& assets, std::uint64_t handle,
                                       const std::function<void( const std::string&, const char* )>& visitGuid )
        {
            if ( const auto layer =
                      assets.ProbeByHandle<Assets::LandscapeLayerInfoAsset>( Assets::AssetHandle( handle ) ) )
                if ( layer->IsReadyForUse() )
                    for ( const auto& grass : layer->GetData().GrassTypes )
                        visitGuid( grass.Guid, "a reachable landscape layer grows it" );
        }

        const std::string* LayerNameOf( std::uint64_t handle )
        {
            const auto* info = Runtime::GetLandscapeLayerInfoService()->Get( Assets::AssetHandle( handle ) );
            return info ? &info->LayerName : nullptr;
        }
    } // namespace

    void StartupModule()
    {
        Core::ModuleHookLists& hooks = Core::ModuleHooks();
        hooks.Services.push_back( { []( const std::weak_ptr<Assets::AssetManager>& assets )
                                    { Runtime::GetLandscapeLayerInfoService()->BindAssetManager( assets ); },
                                    []() { Runtime::GetLandscapeLayerInfoService()->Clear(); } } );
        hooks.Services.push_back( { []( const std::weak_ptr<Assets::AssetManager>& assets )
                                    { Runtime::GetLandscapeGrassTypeService()->BindAssetManager( assets ); },
                                    []() { Runtime::GetLandscapeGrassTypeService()->Clear(); } } );
        hooks.HotReloads.push_back( { &WatchLayerInfos, &PollLayerInfos } );
        hooks.AssetReferences.push_back( { &VisitLayerInfoReferences } );
        hooks.AssetParamLabels.push_back( { "LandscapeLayerInfoAsset", &LayerNameOf } );
        // The tiles' GPU copies are the scene's ledger rows (Scene::GetLedgerInstance).
        hooks.RenderCollectors.push_back(
             []( Core::Scene& scene ) { scene.AddSystem<ECS::LandscapeECSSystem>( scene.GetLedgerInstance() ); } );
    }
} // namespace Desert::Landscape
