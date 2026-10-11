#include "NodeActors.hpp"

#include <Engine/ECS/Components.hpp>

namespace Desert::Editor
{
    std::vector<ECS::Entity> PlaceNodeActors( ::Desert::Core::Scene& scene, ECS::Entity root,
                                              std::span<const PlacedNodeMesh> nodes )
    {
        std::vector<ECS::Entity> placed;
        placed.reserve( nodes.size() );
        for ( const PlacedNodeMesh& node : nodes )
        {
            const ECS::Entity child = scene.CreateNewEntity( std::string( node.Name ) );
            child.AddComponent<ECS::StaticMeshComponent>().MeshHandle = node.Mesh;
            child.GetComponent<ECS::TransformComponent>().Translation = node.Placement;
            scene.Attach( root, child );
            placed.push_back( child );
        }
        return placed;
    }
} // namespace Desert::Editor
