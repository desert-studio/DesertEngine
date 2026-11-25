#pragma once

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <Engine/ECS/Transform.hpp>

// No component here (BUILD-MOD-2): an entity handle names no component type; callers include the
// component headers they use (UE: AActor.h includes no UActorComponent subclass).

namespace Desert::ECS
{
    struct LocalTransform; // Engine/ECS/TransformComponent.hpp

    class Entity final
    {
    public:
        Entity() : m_Handle( entt::null ), m_Registry( nullptr ) {}
        explicit Entity( entt::entity handle, entt::registry& registry )
             : m_Handle( handle ), m_Registry( &registry )
        {
        }
        ~Entity() = default;

        operator bool() const { return m_Handle != entt::null && m_Registry != nullptr; }
        bool operator!() const { return !((bool)*this); }

        template <typename EntityT>
        bool HasComponent() const
        {
            return m_Registry->has<EntityT>( m_Handle );
        }

        template <typename EntityT, typename... Args>
        decltype( auto ) AddComponent( Args&&... args ) const
        {
            if constexpr ( sizeof...( Args ) == 0 )
            {
                return m_Registry->emplace_or_replace<EntityT>( m_Handle );
            }
            else
            {
                return m_Registry->emplace_or_replace<EntityT>( m_Handle, std::forward<Args>( args )... );
            }
        }

        template <typename EntityT>
        EntityT& GetComponent() const
        {
            return m_Registry->get<EntityT>( m_Handle );
        }

        // The entity's TransformComponent writers: ECS::SetLocalTransform and its one-field forms.
        bool SetLocalTransform( const LocalTransform& local ) const;
        bool SetLocalTranslation( const glm::dvec3& translation ) const;
        bool SetLocalRotation( const glm::vec3& rotation ) const;
        bool SetLocalScale( const glm::vec3& scale ) const;

        // The entity's VisibilityComponent writer (adds it when absent): ECS::SetVisible.
        bool SetVisible( bool visible ) const;

        template <typename EntityT>
        void RemoveComponent() const
        {
            if ( m_Registry->has<EntityT>( m_Handle ) )
                m_Registry->remove<EntityT>( m_Handle );
        }

        bool operator==( const Entity& other )
        {
            return m_Handle == other.m_Handle;
        }

        auto GetHandle() const
        {
            return m_Handle;
        }

        entt::registry* GetRegistry() const
        {
            return m_Registry;
        }

        WorldTransform GetWorldTransform() const;

    private:
        entt::entity    m_Handle;
        entt::registry* m_Registry;
    };
} // namespace Desert::ECS