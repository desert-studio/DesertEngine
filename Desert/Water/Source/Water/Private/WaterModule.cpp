#include <Water/Public/BuoyancyComponent.hpp>
#include <Water/Public/BuoyancySimulation.hpp>
#include <Water/Public/WaterBodyComponent.hpp>
#include <Water/Public/WaterBodyGather.hpp>
#include <Water/Public/WaterBodyQuery.hpp>
#include <Water/Public/WaterFallEffects.hpp>
#include <Water/Public/WaterRenderer.hpp>
#include <Water/Public/WaterShoreFieldSource.hpp>
#include <Water/Public/WaterWaveSources.hpp>
#include <Water/Public/WaterWavesAsset.hpp>
#include <Water/Public/WaterWavesService.hpp>
#include <Water/Public/WaterZoneCommand.hpp>
#include <Water/Public/WaterZoneComponent.hpp>
#include <Water/Public/WaterZoneECSSystem.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/System/PhysicsSceneFeature.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Graphic/Systems/Scene/SingleLayerWaterPass.hpp>
#include <Common/Core/Logger.hpp>
#include <Engine/Physics/ColliderComponent.hpp>
#include <Engine/Physics/RigidBodyComponent.hpp>

#include <PhysicsCore/PhysicsWorld.hpp>

#include <filesystem>
#include <algorithm>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

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
        // landscape's ground, and the wave clock, the scene's game time; and the buoyant bodies floating in it
        // (UE UBuoyancyComponent's FBuoyancyComponentSim, run in the solver's pre-step).
        class WaterPhysicsFeature final : public ECS::IPhysicsSceneFeature
        {
        public:
            explicit WaterPhysicsFeature( Physics::PhysicsWorld& world ) : m_World( world )
            {
            }

            void BeginFrame() override
            {
            }

            void Sync( entt::registry& registry ) override
            {
                // This frame's bodies over this frame's ground. The body list is retained (WaterBodyGather:
                // re-walked only when a body is added, patched or removed) and its values read every frame, so a
                // moved or edited body is the water at once; nothing is allocated in a steady frame.
                // The scene's gravity (the physics world's, SceneSettings::Gravity): a river's fall accelerates
                // by it (WaterBodyState::Gravity).
                m_Water.SetGravity( m_World.GetGravity() );
                // Bodies float on the surface the GPU drew: the world's one wave record, the renderer's.
                const auto* const sources = registry.try_ctx<std::shared_ptr<ECS::WaterWaveSources>>();
                if ( sources == nullptr )
                    return; // a world never prepared has no water
                m_Water.SetWorld( m_Gather.Gather( registry, **sources ), ShoreField( registry ) );
                SyncBuoyant( registry );
            }

            void BeginSteps( double gameTimeSeconds ) override
            {
                // THE ONE WATER CLOCK (UE UWaterSubsystem::GetWaterTimeSeconds = the world's time): the scene's
                // game time at the steps' start, the clock the renderer draws the waves and the FFT passes at
                // (which also stamps the FFT readback the queries extrapolate from).
                m_Water.SetTime( gameTimeSeconds );
            }

            void PreStep( float dt ) override
            {
                // The water's clock advances by the FIXED step, once per substep; the pontoons then read the
                // surface of THIS step and push for this step only (a pre-step force acts on its step alone).
                m_Water.Advance( dt );
                const std::span<const glm::vec4> pontoons( m_Pontoons );
                const std::span<PontoonHold>     holds( m_Holds );
                for ( Buoyant& buoyant : m_Buoyant )
                    buoyant.SubmergedShare =
                         ApplyBuoyancy( m_World, buoyant.Body,
                                        pontoons.subspan( buoyant.FirstPontoon, buoyant.PontoonCount ),
                                        holds.subspan( buoyant.FirstPontoon, buoyant.PontoonCount ), m_Water );
            }

            void AfterStep( entt::registry& registry ) override
            {
                for ( const Buoyant& buoyant : m_Buoyant )
                    if ( auto* component = registry.try_get<ECS::BuoyancyComponent>( buoyant.Entity ) )
                        component->SubmergedShare = buoyant.SubmergedShare;
            }

            void PublishEvents( entt::registry& /*registry*/ ) override
            {
            }

        private:
            // One buoyant body of this frame: its live physics body and its pontoons, a range of m_Pontoons.
            struct Buoyant
            {
                entt::entity        Entity         = entt::null;
                Physics::BodyHandle Body           = Physics::kInvalidBody;
                std::size_t         FirstPontoon   = 0;
                std::size_t         PontoonCount   = 0;
                float               SubmergedShare = 0.0f;
            };

            // The field the water is drawn over, baked here when the world has none yet.
            static std::shared_ptr<const WaterShoreField> ShoreField( entt::registry& registry )
            {
                const auto* const source = registry.try_ctx<std::shared_ptr<ECS::WaterShoreFieldSource>>();
                return source != nullptr ? ( *source )->UpdateNow( registry ) : nullptr;
            }

            // This frame's buoyant bodies, read from their components here (on the frame, before the steps) so
            // the pre-step reads no registry. A dynamic body only: a static or kinematic one ignores forces. The
            // storage is reused, so a steady frame allocates nothing.
            void SyncBuoyant( entt::registry& registry )
            {
                std::swap( m_Buoyant, m_LastBuoyant );
                std::swap( m_Holds, m_LastHolds );
                m_Buoyant.clear();
                m_Pontoons.clear();
                m_Holds.clear();
                auto buoyants = registry.view<ECS::BuoyancyComponent, ECS::RigidBodyComponent>();
                for ( const entt::entity entity : buoyants )
                {
                    const ECS::BuoyancyComponent&  buoyancy  = buoyants.get<ECS::BuoyancyComponent>( entity );
                    const ECS::RigidBodyComponent& rigidBody = buoyants.get<ECS::RigidBodyComponent>( entity );
                    if ( rigidBody.RuntimeBody == Physics::kInvalidBody ||
                         rigidBody.Data.Type != Physics::BodyType::Dynamic || m_Refused.contains( entity ) )
                        continue;
                    const std::size_t first = m_Pontoons.size();
                    if ( buoyancy.Data.PontoonSource == ECS::BuoyancyPontoonSource::Manual )
                    {
                        const auto manual = ECS::ManualPontoonsOf( buoyancy.Data );
                        m_Pontoons.insert( m_Pontoons.end(), manual.begin(), manual.end() );
                    }
                    else
                    {
                        const auto* collider = registry.try_get<ECS::ColliderComponent>( entity );
                        std::string error    = collider != nullptr ? std::string{} : std::string( "it has no Collider" );
                        if ( collider == nullptr ||
                             !CutColliderPontoons( collider->Data, buoyancy.Data.ColliderCellsPerAxis, m_Pontoons,
                                                   error ) )
                        {
                            m_Refused.insert( entity );
                            LOG_ERROR( "[Water] entity {} does not float: its Pontoon Source is Collider and {}",
                                       static_cast<uint32_t>( entity ), error );
                            continue;
                        }
                    }
                    if ( m_Pontoons.size() == first )
                        continue; // a Manual list with no pontoon: nothing to float on
                    m_Holds.resize( m_Pontoons.size() );
                    KeepHolds( entity, first, m_Pontoons.size() - first );
                    m_Buoyant.push_back( Buoyant{ .Entity         = entity,
                                                  .Body           = rigidBody.RuntimeBody,
                                                  .FirstPontoon   = first,
                                                  .PontoonCount   = m_Pontoons.size() - first,
                                                  .SubmergedShare = 0.0f } );
                }
            }

            // The pontoons' last ready answers move with them from frame to frame (the bodies' order is the view's,
            // so the last frame's row is at this one's index unless a body came or went).
            void KeepHolds( const entt::entity entity, const std::size_t first, const std::size_t count )
            {
                const auto row  = m_LastBuoyant.begin() + static_cast<std::ptrdiff_t>(
                                                             std::min( m_Buoyant.size(), m_LastBuoyant.size() ) );
                const auto same = [entity]( const Buoyant& b ) { return b.Entity == entity; };
                const auto last = row != m_LastBuoyant.end() && same( *row )
                                       ? row
                                       : std::find_if( m_LastBuoyant.begin(), m_LastBuoyant.end(), same );
                if ( last != m_LastBuoyant.end() && last->PontoonCount == count )
                    std::copy_n( m_LastHolds.begin() + static_cast<std::ptrdiff_t>( last->FirstPontoon ), count,
                                 m_Holds.begin() + static_cast<std::ptrdiff_t>( first ) );
            }

            Physics::PhysicsWorld& m_World;
            WaterSubsystem         m_Water;
            ECS::WaterBodyGather   m_Gather;
            std::vector<Buoyant>   m_Buoyant;
            std::vector<glm::vec4> m_Pontoons;
            std::vector<PontoonHold> m_Holds; // one per pontoon
            std::vector<Buoyant>     m_LastBuoyant; // the last frame's, for KeepHolds
            std::vector<PontoonHold> m_LastHolds;
            // Buoyant bodies whose collider cannot be cut into pontoons, refused once per entity per Play.
            std::unordered_set<entt::entity> m_Refused;
        };

        // The world's wave record: made with the scene, in its registry's context, for every reader. It ends with
        // the world's entities (Scene::Clear): the next scene's ocean is another one.
        class WaterWaveSceneState final : public Core::ISceneModuleState
        {
        public:
            explicit WaterWaveSceneState( entt::registry& registry )
                 : m_Sources( std::make_shared<ECS::WaterWaveSources>() )
            {
                registry.set<std::shared_ptr<ECS::WaterWaveSources>>( m_Sources );
            }

            // Refreshed by the module's Prepares hook, before the frame's systems.
            void Tick( Core::Scene& /*scene*/, entt::registry& /*registry*/, double /*seconds*/ ) override
            {
            }

            void Reset() override
            {
            }

            void Clear() override
            {
                m_Sources->Reset();
            }

        private:
            std::shared_ptr<ECS::WaterWaveSources> m_Sources;
        };
    } // namespace

    void StartupModule()
    {
        ECS::RegisterPhysicsSceneFeature(
             []( Physics::PhysicsWorld& world ) -> std::unique_ptr<ECS::IPhysicsSceneFeature>
             { return std::make_unique<WaterPhysicsFeature>( world ); } );
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
        // WATER-UNDERWATER: a saved .dwaves is watched here -> ContentRegistry::Update marks it written ->
        // WaterWavesService re-reads it (WrittenSince) and rebuilds the set's waves without a restart; the poll
        // itself has nothing to reload.
        hooks.HotReloads.push_back(
             { []( Assets::AssetManager& assetManager, std::vector<std::shared_ptr<Assets::AssetBase>>& watched )
               {
                   for ( const auto& [handle, asset] : assetManager.FindAllByType<Assets::WaterWavesAsset>() )
                       if ( asset )
                           watched.push_back( asset );
               },
               []( Assets::AssetManager&, const std::function<bool( const std::filesystem::path& )>& ) {} } );
        // The bodies are gathered by the physics feature and the zone collector inside parallel groups.
        hooks.Prepares.push_back(
             []( entt::registry& r )
             {
                 r.prepare<ECS::WaterBodyComponent>();
                 r.prepare<ECS::BuoyancyComponent>();
                 r.prepare<ECS::WaterZoneComponent>();
                 // Its one creation, before the parallel readers (WaterShoreFieldSource).
                 if ( r.try_ctx<std::shared_ptr<ECS::WaterShoreFieldSource>>() == nullptr )
                     r.set<std::shared_ptr<ECS::WaterShoreFieldSource>>(
                          std::make_shared<ECS::WaterShoreFieldSource>() );
                 // Once a frame, serially and before any system: the record every reader of the frame sees - a
                 // paste, an undo and a scene without a zone are validated here like a load.
                 r.ctx<std::shared_ptr<ECS::WaterWaveSources>>()->Refresh( r );
             } );
        hooks.SceneLoadChecks.push_back(
             []( const Core::Scene& scene, std::string_view /*sceneName*/ )
             {
                 const entt::registry& r = scene.GetRegistry();
                 if ( const auto* const sources = r.try_ctx<std::shared_ptr<ECS::WaterWaveSources>>() )
                     ( *sources )->SceneLoaded( r );
             } );
        hooks.SceneStates.push_back( []( entt::registry& registry ) -> std::unique_ptr<Core::ISceneModuleState>
                                     { return std::make_unique<WaterWaveSceneState>( registry ); } );
        // The foam, spray and mist at the foot of every river's falls.
        hooks.SceneStates.push_back( []( entt::registry& /*registry*/ ) -> std::unique_ptr<Core::ISceneModuleState>
                                     { return std::make_unique<WaterFallEffects>(); } );
        // The water zones and the bodies inside them (WATER-W6): one command per zone.
        hooks.RenderCollectors.push_back( []( Core::Scene& scene )
                                          { scene.AddSystem<ECS::WaterZoneECSSystem>( scene.GetSettings() ); } );
    }
} // namespace Desert::Water
