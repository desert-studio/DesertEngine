#include <Landscape/Public/LandscapeCollision.hpp>
#include <Landscape/Public/LandscapeECSSystem.hpp>
#include <Landscape/Public/LandscapeGrassTypeAsset.hpp>
#include <Landscape/Public/LandscapeGrassTypeService.hpp>
#include <Landscape/Public/LandscapeLayerInfoService.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/AssetRootSet.hpp>
#include <Engine/Assets/LandscapeLayerInfoAsset.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/ECS/System/PhysicsSceneFeature.hpp>
#include <Engine/World/Landscape/LandscapeComponent.hpp>
#include <Engine/World/Landscape/LandscapeMaterialComponent.hpp>
#include <Common/Core/Logger.hpp>

#include <filesystem>
#include <memory>
#include <string>

// THE LANDSCAPE MODULE (UE: the Landscape runtime module's StartupModule). What the landscape adds to the engine
// is registered here, through the engine's generic hooks: the layer-info and grass-type services, the
// `.delayerinfo` / `.degrasstype` hot reload, the grass types a layer info keeps reachable, the layer name a
// terrain material's layer input is matched by, and the tiles' and grass's render collector. Engine names only
// Landscape::StartupModule (Engine/Core/Modules.cpp).

namespace Desert::Landscape
{
    namespace
    {
        template <typename TAsset>
        void WatchLoaded( Assets::AssetManager& assets, std::vector<std::shared_ptr<Assets::AssetBase>>& watched )
        {
            for ( const auto& [handle, asset] : assets.FindAllByType<TAsset>() )
                if ( asset )
                    watched.push_back( asset );
        }

        // No service to notify: LandscapeLayerInfoService and LandscapeGrassTypeService hand out the asset's own
        // data, and the landscape system and the paint tool read it every frame / every stroke (the grass compares
        // the type's Revision for what it derived from its varieties), so re-reading the asset IS the reload.
        template <typename TAsset>
        void PollReloaded( Assets::AssetManager&                                      assets,
                           const std::function<bool( const std::filesystem::path& )>& changed, const char* what,
                           const char* effect )
        {
            for ( const auto& [handle, asset] : assets.FindAllByType<TAsset>() )
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
                    LOG_ERROR( "[HotReload] Landscape {} '{}' could not be re-read: {}", what, key,
                               reloaded.GetError() );
                    continue;
                }
                LOG_INFO( "[HotReload] Landscape {} '{}' reloaded — {}.", what, key, effect );
            }
        }

        void PollLayerInfos( Assets::AssetManager&                                      assets,
                             const std::function<bool( const std::filesystem::path& )>& changed )
        {
            PollReloaded<Assets::LandscapeLayerInfoAsset>( assets, changed, "layer info",
                                                           "the next frame and stroke use it" );
        }

        void PollGrassTypes( Assets::AssetManager&                                      assets,
                             const std::function<bool( const std::filesystem::path& )>& changed )
        {
            PollReloaded<Assets::LandscapeGrassTypeAsset>( assets, changed, "grass type",
                                                           "the next frame grows it" );
        }

        // THE GRASS A LAYER GROWS (JFA-OUTLINE-MISSING): a layer info is a root (a landscape paints with it), and
        // the grass types it names must stay with it, or the sweep releases them and every grass instance
        // vanishes; the grass type in turn keeps its meshes and materials.
        void
        VisitLandscapeAssetReferences( Assets::AssetManager& assets, std::uint64_t handle,
                                       const std::function<void( const std::string&, const char* )>& visitGuid )
        {
            if ( const auto layer =
                      assets.ProbeByHandle<Assets::LandscapeLayerInfoAsset>( Assets::AssetHandle( handle ) ) )
                if ( layer->IsReadyForUse() )
                    for ( const auto& grass : layer->GetData().GrassTypes )
                        visitGuid( grass.Guid, "a reachable landscape layer grows it" );
            // A grass type keeps the meshes it draws and the materials its blades are shaded with.
            if ( const auto grassType =
                      assets.ProbeByHandle<Assets::LandscapeGrassTypeAsset>( Assets::AssetHandle( handle ) ) )
                if ( grassType->IsReadyForUse() )
                    for ( const auto& variety : grassType->GetData().GrassVarieties )
                    {
                        if ( variety.Mesh )
                            visitGuid( variety.Mesh->Guid, "a reachable grass type draws it" );
                        if ( variety.Material )
                            visitGuid( variety.Material->Guid,
                                       "a reachable grass type shades its blades with it" );
                    }
        }

        const std::string* LayerNameOf( std::uint64_t handle )
        {
            const auto* info = Runtime::GetLandscapeLayerInfoService()->Get( Assets::AssetHandle( handle ) );
            return info ? &info->LayerName : nullptr;
        }

        // The assets a landscape names are roots of the loaded scene (Core/SceneAssetRoots.cpp walks the hooks):
        // its surface material and its paint layers.
        void MarkLandscapeAssetRoots( entt::registry& registry, Assets::AssetRootSet& roots )
        {
            for ( const auto entity : registry.view<ECS::LandscapeMaterialComponent>() )
                roots.Mark( registry.get<ECS::LandscapeMaterialComponent>( entity ).Data.Material,
                            "a landscape is surfaced with it" );
            for ( const auto entity : registry.view<ECS::LandscapeComponent>() )
                for ( const Assets::AssetHandle& layer : registry.get<ECS::LandscapeComponent>( entity ).Layers )
                    roots.Mark( layer, "a landscape paints with it" );
        }

        // The landscape's part of the physics scene (UE: ULandscapeHeightfieldCollisionComponent's bodies in the
        // Chaos scene): one height-field body per drawn tile, made per Play over PhysicsECSSystem's world.
        class LandscapePhysicsFeature final : public ECS::IPhysicsSceneFeature
        {
        public:
            explicit LandscapePhysicsFeature( Physics::PhysicsWorld& world ) : m_Collision( world )
            {
            }

            void BeginFrame() override
            {
            }
            void Sync( entt::registry& registry ) override
            {
                m_Collision.Attach( registry );
                // Refused tiles get no body; LandscapeECSSystem reports them (it applies the same test).
                m_Collision.Sync( ECS::DrawableLandscapeTiles( registry ).Tiles );
            }
            void AfterStep( entt::registry& /*registry*/ ) override
            {
            }
            void PublishEvents( entt::registry& /*registry*/ ) override
            {
            }

        private:
            ECS::LandscapeCollision m_Collision;
        };
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
        hooks.AssetRoots.push_back( &MarkLandscapeAssetRoots );
        ECS::RegisterPhysicsSceneFeature( []( Physics::PhysicsWorld& world ) -> std::unique_ptr<ECS::IPhysicsSceneFeature>
                                          { return std::make_unique<LandscapePhysicsFeature>( world ); } );
        hooks.HotReloads.push_back( { &WatchLoaded<Assets::LandscapeLayerInfoAsset>, &PollLayerInfos } );
        hooks.HotReloads.push_back( { &WatchLoaded<Assets::LandscapeGrassTypeAsset>, &PollGrassTypes } );
        hooks.AssetReferences.push_back( { &VisitLandscapeAssetReferences } );
        hooks.AssetParamLabels.push_back( { "LandscapeLayerInfoAsset", &LayerNameOf } );
        // The tiles' GPU copies are the scene's ledger rows (Scene::GetLedgerInstance).
        hooks.RenderCollectors.push_back(
             []( Core::Scene& scene ) { scene.AddSystem<ECS::LandscapeECSSystem>( scene.GetLedgerInstance() ); } );
    }
} // namespace Desert::Landscape
