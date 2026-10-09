#include <Engine/ECS/System/DestructibleLifetime.hpp>

#include <Engine/ECS/Components.hpp>

#include <Common/Core/Logger.hpp>

#include <glm/gtc/quaternion.hpp>

#include <format>
#include <type_traits>

namespace Desert::ECS
{
    static_assert(
         std::is_same_v<decltype( DestructibleComponent::RuntimeObject ), Destruction::DestructibleHandle> );

    DestructibleLifetime::~DestructibleLifetime()
    {
        Detach();
    }

    void DestructibleLifetime::Attach( entt::registry& registry )
    {
        if ( m_Registry == &registry )
            return;
        Detach();
        m_Registry = &registry;
        registry.on_destroy<DestructibleComponent>().connect<&DestructibleLifetime::OnDestructibleDestroyed>(
             *this );
    }

    void DestructibleLifetime::Detach()
    {
        if ( m_Registry == nullptr )
            return;
        m_Registry->on_destroy<DestructibleComponent>().disconnect<&DestructibleLifetime::OnDestructibleDestroyed>(
             *this );
        m_Registry = nullptr;
        m_Refused.clear();
    }

    void DestructibleLifetime::OnDestructibleDestroyed( entt::registry& registry, entt::entity entity )
    {
        m_Refused.erase( entity ); // the id may come back as a new entity
        auto& destructible = registry.get<DestructibleComponent>( entity );
        if ( destructible.RuntimeObject == Destruction::kInvalidDestructible )
            return;
        m_World->Remove( destructible.RuntimeObject );
        destructible.RuntimeObject = Destruction::kInvalidDestructible;
        destructible.RuntimeNodeWorld.clear();
    }

    void DestructibleLifetime::WritePoses( entt::registry& registry ) const
    {
        for ( const auto entity : registry.view<DestructibleComponent>() )
        {
            auto& destructible = registry.get<DestructibleComponent>( entity );
            if ( destructible.RuntimeObject == Destruction::kInvalidDestructible )
            {
                destructible.RuntimeNodeWorld.clear();
                continue;
            }
            const size_t nodes = m_World->GetNodeCount( destructible.RuntimeObject );
            destructible.RuntimeNodeWorld.resize( nodes );
            for ( size_t node = 0; node < nodes; ++node )
                destructible.RuntimeNodeWorld[node] =
                     m_World->GetNodeWorld( destructible.RuntimeObject, static_cast<int32_t>( node ) );
        }
    }

    void DestructibleLifetime::Refuse( entt::entity entity, const std::string& reason )
    {
        m_Refused.insert( entity );
        LOG_ERROR( "[Destruction] entity {} is not destructible: {}", static_cast<uint32_t>( entity ), reason );
    }

    void DestructibleLifetime::Sync( entt::registry& registry, const FractureLookup& lookup )
    {
        auto view = registry.view<TransformComponent, DestructibleComponent>();
        for ( const auto entity : view )
        {
            auto& destructible = view.get<DestructibleComponent>( entity );
            if ( destructible.RuntimeObject != Destruction::kInvalidDestructible || m_Refused.contains( entity ) )
                continue;
            const DestructibleData& data = destructible.Data;
            if ( data.Fracture == Assets::AssetHandle::Null() )
            {
                Refuse( entity, "its Rest Collection is empty: drag a .dfrac onto it" );
                continue;
            }
            auto fracture = lookup( data.Fracture );
            if ( !fracture.IsSuccess() )
            {
                Refuse( entity, fracture.GetError() );
                continue;
            }
            if ( !fracture.GetValue() )
                continue; // still loading

            // The world pose: parents composed, as PhysicsECSSystem places a body.
            glm::mat4    world   = view.get<TransformComponent>( entity ).GetTransform();
            entt::entity current = entity;
            while ( registry.has<RelationshipComponent>( current ) )
            {
                const auto& relationship = registry.get<RelationshipComponent>( current );
                if ( relationship.Parent == entt::null )
                    break;
                current = relationship.Parent;
                if ( registry.has<TransformComponent>( current ) )
                    world = registry.get<TransformComponent>( current ).GetTransform() * world;
            }
            glm::mat3       basis( world );
            const glm::vec3 scale( glm::length( basis[0] ), glm::length( basis[1] ), glm::length( basis[2] ) );
            if ( glm::any( glm::greaterThan( glm::abs( scale - glm::vec3( 1.0f ) ), glm::vec3( 1e-3f ) ) ) )
            {
                Refuse( entity, std::format( "its world scale is ({}, {}, {}); a fracture is simulated at the "
                                             "size it was baked, so the scale must be one",
                                             scale.x, scale.y, scale.z ) );
                continue;
            }

            Destruction::DestructibleDesc desc;
            desc.Position                             = glm::vec3( world[3] );
            desc.Rotation                             = glm::quat_cast( basis );
            desc.AnchoredNodes                        = data.AnchoredNodes;
            desc.DamageThreshold                      = data.DamageThreshold;
            desc.Settings.DensityKgPerCm3             = data.DensityKgPerCm3;
            desc.Settings.Friction                    = data.Friction;
            desc.Settings.Restitution                 = data.Restitution;
            desc.Settings.RemoveOnSleep               = data.RemoveOnSleep;
            desc.Settings.MaxSleepTime                = data.MaxSleepTime;
            desc.Settings.SlowMovingAsSleeping        = data.SlowMovingAsSleeping;
            desc.Settings.SlowMovingVelocityThreshold = data.SlowMovingVelocityThreshold;
            // Collision events are recorded only for the objects that publish them (DestructionVFXEvents).
            desc.Settings.CollisionEvents          = data.NotifyCollisions;
            desc.Settings.CollisionEventMinImpulse = data.CollisionEventMinImpulse;

            auto added = m_World->Add( fracture.GetValue(), desc );
            if ( !added.IsSuccess() )
            {
                Refuse( entity, added.GetError() );
                continue;
            }
            destructible.RuntimeObject = added.GetValue();
        }
    }
} // namespace Desert::ECS
