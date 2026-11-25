#include <Engine/ECS/Entity.hpp>

#include <Engine/Core/Scene.hpp>
#include <vector>
#include <Engine/ECS/RelationshipComponent.hpp>
#include <Engine/ECS/TransformComponent.hpp>
#include <Engine/ECS/VisibilityComponent.hpp>

namespace Desert::ECS
{

    namespace
    {
        // One step of the subtree walk: an entity and the world it composes under.
        struct WorldStep
        {
            entt::entity Entity;
            WorldTransform ParentWorld;
        };
    } // namespace

    WorldTransform ParentWorldTransform( const entt::registry& registry, entt::entity entity )
    {
        const auto*  relation = registry.try_get<RelationshipComponent>( entity );
        entt::entity parent   = entt::null;
        if ( relation != nullptr )
            parent = relation->Parent;
        while ( parent != entt::null && registry.valid( parent ) )
        {
            if ( const auto* transform = registry.try_get<TransformComponent>( parent ) )
                return transform->GetWorldTransform();
            const auto* up = registry.try_get<RelationshipComponent>( parent );
            if ( up == nullptr )
                break;
            parent = up->Parent;
        }
        return WorldTransform{};
    }

    bool WorldTransformPending( const TransformComponent& transform )
    {
        return !transform.m_WorldComposed;
    }

    void UpdateWorldTransforms( entt::registry& registry, entt::entity root )
    {
        if ( !registry.valid( root ) )
            return;
        // Iterative, so a deep hierarchy cannot overflow the stack; the scratch keeps its capacity per thread,
        // so a steady stream of writes allocates nothing. Not re-entered: nothing here fires a signal.
        thread_local std::vector<WorldStep> pending;
        pending.clear();
        pending.push_back( WorldStep{ root, ParentWorldTransform( registry, root ) } );
        while ( !pending.empty() )
        {
            const WorldStep step = pending.back();
            pending.pop_back();
            WorldTransform world = step.ParentWorld;
            if ( auto* transform = registry.try_get<TransformComponent>( step.Entity ) )
            {
                transform->m_World         = step.ParentWorld * transform->m_Local;
                transform->m_WorldComposed = true;
                world                      = transform->m_World;
            }
            if ( const auto* relation = registry.try_get<RelationshipComponent>( step.Entity ) )
                for ( const entt::entity child : relation->Children )
                    if ( registry.valid( child ) )
                        pending.push_back( WorldStep{ child, world } );
        }
    }

    WorldTransform Entity::GetWorldTransform() const
    {
        // A load of the cached world (UpdateWorldTransforms keeps it current); no allocation, no parent walk.
        if ( const auto* transform = m_Registry->try_get<TransformComponent>( m_Handle ) )
            return transform->GetWorldTransform();
        return ParentWorldTransform( *m_Registry, m_Handle );
    }


    bool Entity::SetLocalTransform( const LocalTransform& local ) const
    {
        return ECS::SetLocalTransform( *m_Registry, m_Handle, local );
    }
    bool Entity::SetLocalTranslation( const glm::dvec3& translation ) const
    {
        return ECS::SetLocalTranslation( *m_Registry, m_Handle, translation );
    }
    bool Entity::SetLocalRotation( const glm::vec3& rotation ) const
    {
        return ECS::SetLocalRotation( *m_Registry, m_Handle, rotation );
    }
    bool Entity::SetLocalScale( const glm::vec3& scale ) const
    {
        return ECS::SetLocalScale( *m_Registry, m_Handle, scale );
    }
    bool Entity::SetVisible( bool visible ) const
    {
        return ECS::SetVisible( *m_Registry, m_Handle, visible );
    }

} // namespace Desert::ECS