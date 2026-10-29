#pragma once

#include <entt/entt.hpp>
#include <Engine/ECS/Components.hpp>

namespace Desert::ECS
{
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
        bool SetLocalTransform( const LocalTransform& local ) const
        {
            return ECS::SetLocalTransform( *m_Registry, m_Handle, local );
        }
        bool SetLocalTranslation( const glm::vec3& translation ) const
        {
            return ECS::SetLocalTranslation( *m_Registry, m_Handle, translation );
        }
        bool SetLocalRotation( const glm::vec3& rotation ) const
        {
            return ECS::SetLocalRotation( *m_Registry, m_Handle, rotation );
        }
        bool SetLocalScale( const glm::vec3& scale ) const
        {
            return ECS::SetLocalScale( *m_Registry, m_Handle, scale );
        }

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

        glm::mat4 GetWorldTransform() const;

    private:
        entt::entity    m_Handle;
        entt::registry* m_Registry;
    };
} // namespace Desert::ECS