#include <Destruction/Public/DestructionModule.hpp>

#include <Destruction/Public/DestructibleComponent.hpp>
#include <Destruction/Public/DestructionEvents.hpp>
#include <Destruction/Public/DestructionFieldComponents.hpp>
#include <Destruction/Public/FracturePieceDraw.hpp>
#include <Destruction/Public/FracturePreviewComponent.hpp>
#include <Destruction/Public/FractureService.hpp>
#include <Engine/Assets/AssetRootSet.hpp>
#include <Engine/Assets/Mesh/StaticMeshAsset.hpp>
#include <Engine/Core/ModuleHooks.hpp>
#include <Engine/ECS/ComponentEventFacts.hpp>
#include <Engine/ECS/System/MeshStandInDraw.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/Reflection/FunctionThunk.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>

#include <filesystem>
#include <vector>

namespace Desert::Destruction
{
    namespace
    {
        template <std::size_t N>
        std::vector<Reflection::Value> Args( std::array<Reflection::Value, N>&& payload )
        {
            return { std::make_move_iterator( payload.begin() ), std::make_move_iterator( payload.end() ) };
        }

        // The destructibles' news as the Gameplay Framework's facts: DestructibleData::OnBreak / OnCollision /
        // OnRemoved with their payloads (UE: OnChaosBreakEvent / OnChaosPhysicsCollision / OnChaosRemovalEvent).
        void Translate( const ECS::DestructionEventQueue& queue, ECS::ComponentEventFactQueue& out )
        {
            using Data = ECS::DestructibleData;
            for ( const ECS::DestructibleBreakEvent& e : queue.Breaks )
                out.Facts.push_back( { e.Self, "DestructibleData", "OnBreak",
                                       Args( Reflection::EventPayload<Data::OnBreak>(
                                            e.Node, e.Position, e.Velocity, e.MassKg, e.PieceCount ) ) } );
            for ( const ECS::DestructibleCollisionEvent& e : queue.Collisions )
                out.Facts.push_back( { e.Self, "DestructibleData", "OnCollision",
                                       Args( Reflection::EventPayload<Data::OnCollision>(
                                            e.Node, e.Position, e.Normal, e.Impulse, e.Velocity, e.MassKg ) ) } );
            for ( const ECS::DestructibleRemovalEvent& e : queue.Removals )
                out.Facts.push_back( { e.Self, "DestructibleData", "OnRemoved",
                                       Args( Reflection::EventPayload<Data::OnRemoved>( e.Node, e.Position, e.MassKg,
                                                                                         e.PieceCount ) ) } );
        }
    } // namespace

    DestructionPhysicsFeature::DestructionPhysicsFeature( Physics::PhysicsWorld& world )
         : m_World( std::make_unique<DestructionWorld>( world ) ),
           m_Lifetime( std::make_unique<ECS::DestructibleLifetime>( *m_World ) )
    {
    }

    void DestructionPhysicsFeature::BeginFrame()
    {
        // The events of this frame's steps are readable until the next frame's physics.
        m_World->ClearEvents();
    }

    void DestructionPhysicsFeature::Sync( entt::registry& registry )
    {
        m_Lifetime->Attach( registry );
        m_Lifetime->Sync(
             registry, []( const Assets::AssetHandle& fracture )
             { return Runtime::GetFractureService()->Get( fracture ); },
             []( const Assets::AssetHandle& mesh )
             {
                 // The file a run-time fracture cuts (DST-RUNTIME): the static mesh asset the entity draws.
                 const auto* asset = Runtime::ResourceRegistry::GetMeshService()->GetAsset( mesh );
                 return asset ? asset->GetMetadata().Filepath : std::filesystem::path();
             } );
    }

    void DestructionPhysicsFeature::AfterStep( entt::registry& registry )
    {
        // At Impact destructibles hit hard enough are sent to be cut around the hit (DST-RUNTIME).
        m_Lifetime->CutOnImpact( registry );
        // Every simulated destructible's piece poses from its bodies (DST-06), what its pieces draw at.
        m_Lifetime->WritePoses( registry );
    }

    void DestructionPhysicsFeature::PublishEvents( entt::registry& registry )
    {
        // The steps' breaks, collisions and removals: destruction's own queue first (its owner publishes it, gated
        // by the entities' Notify flags), then the same news as the Gameplay Framework's facts.
        m_Lifetime->PublishEvents( registry );
        const auto* queue = registry.try_ctx<ECS::DestructionEventQueue>();
        if ( queue == nullptr || queue->Publication == m_Translated )
            return;
        m_Translated = queue->Publication;
        Translate( *queue, registry.ctx_or_set<ECS::ComponentEventFactQueue>() );
    }

    DestructionWorld* FindDestructionWorld( const ECS::PhysicsECSSystem* physics )
    {
        auto* feature = physics != nullptr ? physics->FindFeature<DestructionPhysicsFeature>() : nullptr;
        return feature != nullptr ? &feature->GetWorld() : nullptr;
    }

    void StartupModule()
    {
        ECS::RegisterPhysicsSceneFeature( []( Physics::PhysicsWorld& world ) -> std::unique_ptr<ECS::IPhysicsSceneFeature>
                                          { return std::make_unique<DestructionPhysicsFeature>( world ); } );
        ECS::RegisterMeshStandInDraw( []() -> std::unique_ptr<ECS::IMeshStandInDraw>
                                      { return std::make_unique<ECS::FracturePieceDraw>(); } );

        Core::ModuleHookLists& hooks = Core::ModuleHooks();
        hooks.Services.push_back(
             { []( const std::weak_ptr<Assets::AssetManager>& assets )
               { Runtime::GetFractureService()->BindAssetManager( assets ); },
               []() { Runtime::GetFractureService()->Clear(); } } );
        // A `.dfrac` is named only by the destructibles built from it: without this root the sweep after the scene
        // load unloads it under FractureService, and Play finds a fracture with no nodes (UE: the component's
        // RestCollection is a hard reference).
        hooks.AssetRoots.push_back(
             []( entt::registry& registry, Assets::AssetRootSet& roots )
             {
                 for ( const auto entity : registry.view<ECS::DestructibleComponent>() )
                     roots.Mark( registry.get<ECS::DestructibleComponent>( entity ).Data.Fracture,
                                 "a destructible is simulated from it" );
             } );
        // The editor's Fracture preview is viewed by the stand-in draw inside MeshECSSystem's parallel group; the
        // field components by the Sequencer and gameplay.
        hooks.Prepares.push_back(
             []( entt::registry& r )
             {
                 r.prepare<ECS::FracturePreviewComponent>();
                 r.prepare<ECS::RadialImpulseFieldComponent>();
                 r.prepare<ECS::StrainFieldComponent>();
                 r.prepare<ECS::KillFieldComponent>();
                 r.prepare<ECS::AnchorFieldComponent>();
             } );
    }
} // namespace Desert::Destruction
