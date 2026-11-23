#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterBodyGather.hpp>
#include <Water/Public/WaterBodyQuery.hpp>
#include <Water/Public/WaterRenderer.hpp>
#include <Water/Public/WaterWavesAsset.hpp>
#include <Water/Public/WaterWavesService.hpp>
#include <Water/Public/WaterZoneCommand.hpp>
#include <Water/Public/WaterZoneComponent.hpp>
#include <Water/Public/WaterZoneECSSystem.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/LandscapeRootOf.hpp>
#include <Engine/ECS/System/PhysicsSceneFeature.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/SingleLayerWaterPass.hpp>

#include <filesystem>
#include <memory>

// THE WATER MODULE (UE: the Water plugin's StartupModule). Everything water adds to the engine is registered here,
// through the engine's generic hooks: the wave service, the `.dwaves` asset type, the bodies' and zones' pools,
// the zone collector, the single layer water pass and the water subsystem of a Play's physics scene. Engine names
// only Water::StartupModule (Engine/Core/Modules.cpp).

namespace Desert::Graphic::Render
{
    void WaterZoneCommand::Execute( SceneRenderer& renderer )
    {
        // By reference: read until EndScene, which this command outlives. The pass is this module's own.
        if ( auto* water = dynamic_cast<System::WaterRenderer*>( renderer.GetSingleLayerWaterPass() ) )
            water->Submit( Zone );
    }
} // namespace Desert::Graphic::Render

namespace Desert::Water
{
    namespace
    {
        // The water of a Play's physics scene (UE UWaterSubsystem): the bodies the step's bodies are in, over the
        // landscape's ground, and the wave clock, which starts with the Play at zero.
        class WaterPhysicsFeature final : public ECS::IPhysicsSceneFeature
        {
        public:
            void BeginFrame() override
            {
            }

            void Sync( entt::registry& registry ) override
            {
                // This frame's bodies over this frame's ground. The body list is retained (WaterBodyGather:
                // re-walked only when a body is added, patched or removed) and its values read every frame, so a
                // moved or edited body is the water at once; nothing is allocated in a steady frame.
                m_Water.SetWorld( m_Gather.Gather( registry, []( const Assets::AssetHandle& waves )
                                                   { return Runtime::GetWaterWavesService()->Get( waves ); } ),
                                  ECS::DrawableLandscapeTiles( registry ).RayTiles );
            }

            void PreStep( float dt ) override
            {
                // The water's clock advances by the FIXED step, once per substep.
                m_Water.Advance( dt );
            }

            void AfterStep( entt::registry& /*registry*/ ) override
            {
            }

            void PublishEvents( entt::registry& /*registry*/ ) override
            {
            }

        private:
            WaterSubsystem m_Water;
            // Refuses a body whose wave set cannot be read, once per entity per Play.
            ECS::WaterBodyGather m_Gather;
        };
    } // namespace

    void StartupModule()
    {
        ECS::RegisterPhysicsSceneFeature(
             []( Physics::PhysicsWorld& ) -> std::unique_ptr<ECS::IPhysicsSceneFeature>
             { return std::make_unique<WaterPhysicsFeature>(); } );
        Graphic::System::RegisterSingleLayerWaterPass(
             []( Graphic::SceneRenderer* renderer, const std::shared_ptr<Graphic::Framebuffer>& target )
                  -> std::shared_ptr<Graphic::System::ISingleLayerWaterPass>
             { return std::make_shared<Graphic::System::WaterRenderer>( renderer, target ); } );

        Core::ModuleHookLists& hooks = Core::ModuleHooks();
        hooks.Services.push_back( { []( const std::weak_ptr<Assets::AssetManager>& assets )
                                    { Runtime::GetWaterWavesService()->BindAssetManager( assets ); },
                                    []() { Runtime::GetWaterWavesService()->Clear(); } } );
        // A water body's wave set (WaterBodyData::WaterWaves), announced, not read: WaterWavesService reads the
        // `.dwaves` when Play gathers the bodies, and the asset's constructor adopts the handle its header GUID
        // names - so a reference stored BY GUID is the row under that handle, wherever the file has since moved.
        hooks.AssetTypes.push_back(
             { "WaterWavesAsset",
               []( Assets::AssetManager& m, const std::filesystem::path& full ) -> std::uint64_t
               {
                   auto a = m.FindByPath<Assets::WaterWavesAsset>( full );
                   if ( !a )
                       a = m.CreateAsset<Assets::WaterWavesAsset>( full, /*loadAfterCreate=*/false );
                   return a ? static_cast<std::uint64_t>( a->GetMetadata().Handle ) : 0;
               },
               []( const Assets::AssetManager& m, std::uint64_t handle ) -> bool {
                   return static_cast<bool>(
                        m.FindByHandle<Assets::WaterWavesAsset>( Assets::AssetHandle( handle ) ) );
               } } );
        // The bodies are gathered by the physics feature and the zone collector inside parallel groups.
        hooks.Prepares.push_back(
             []( entt::registry& r )
             {
                 r.prepare<ECS::WaterBodyComponent>();
                 r.prepare<ECS::WaterZoneComponent>();
             } );
        // The water zones and the bodies inside them (WATER-W6): one command per zone.
        hooks.RenderCollectors.push_back( []( Core::Scene& scene )
                                          { scene.AddSystem<ECS::WaterZoneECSSystem>(); } );
    }
} // namespace Desert::Water
