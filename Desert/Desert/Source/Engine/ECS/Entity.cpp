#include <Engine/ECS/Entity.hpp>

#include <Engine/Core/Scene.hpp>
#include <vector>

namespace Desert::ECS
{

    glm::mat4 Entity::GetWorldTransform() const
    {
        std::vector<entt::entity> chain;
        entt::entity current = m_Handle;
        while ( current != entt::null )
        {
            chain.push_back( current );
            if ( m_Registry->has<RelationshipComponent>( current ) )
                current = m_Registry->get<RelationshipComponent>( current ).Parent;
            else
                break;
        }

        glm::mat4 world = glm::mat4( 1.0f );
        for ( auto it = chain.rbegin(); it != chain.rend(); ++it )
        {
            if ( m_Registry->has<TransformComponent>( *it ) )
                world = world * m_Registry->get<TransformComponent>( *it ).GetTransform();
        }
        return world;
    }

} // namespace Desert::ECS