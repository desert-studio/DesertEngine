#include <Engine/ECS/Entity.hpp>

#include <Engine/Core/Scene.hpp>
#include <vector>

namespace Desert::ECS
{

    namespace
    {
        // One step of the subtree walk: an entity and the world it composes under.
        struct WorldStep
        {
            entt::entity Entity;
            glm::mat4    ParentWorld;
        };
    } // namespace

    glm::mat4 ParentWorldTransform( const entt::registry& registry, entt::entity entity )
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
        return glm::mat4( 1.0f );
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
            glm::mat4 world = step.ParentWorld;
            if ( auto* transform = registry.try_get<TransformComponent>( step.Entity ) )
            {
                transform->m_World         = step.ParentWorld * transform->m_Local.GetTransform();
                transform->m_WorldComposed = true;
                world                      = transform->m_World;
            }
            if ( const auto* relation = registry.try_get<RelationshipComponent>( step.Entity ) )
                for ( const entt::entity child : relation->Children )
                    if ( registry.valid( child ) )
                        pending.push_back( WorldStep{ child, world } );
        }
    }

    glm::mat4 Entity::GetWorldTransform() const
    {
        // A load of the cached world (UpdateWorldTransforms keeps it current); no allocation, no parent walk.
        if ( const auto* transform = m_Registry->try_get<TransformComponent>( m_Handle ) )
            return transform->GetWorldTransform();
        return ParentWorldTransform( *m_Registry, m_Handle );
    }

} // namespace Desert::ECS