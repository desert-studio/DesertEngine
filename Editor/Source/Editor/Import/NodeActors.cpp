#include "NodeActors.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <glm/gtx/quaternion.hpp>

#include <limits>

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
            child.SetLocalTranslation( node.Placement );
            scene.Attach( root, child );
            placed.push_back( child );
        }
        return placed;
    }

    std::vector<ECS::Entity> PlaceSourceLights( ::Desert::Core::Scene& scene, ECS::Entity root,
                                                std::span<const Assets::Serialization::ImportRecordLight> lights )
    {
        // The metre UE's photometric falloff is stated per: Intensity / d^2 with d in metres.
        constexpr float          kInverseSquareReference = 100.0f;
        std::vector<ECS::Entity> placed;
        placed.reserve( lights.size() );
        for ( const Assets::Serialization::ImportRecordLight& light : lights )
        {
            const glm::vec3   position( light.Position[0], light.Position[1], light.Position[2] );
            const glm::vec3   direction( light.Direction[0], light.Direction[1], light.Direction[2] );
            const glm::vec3   color( light.Color[0], light.Color[1], light.Color[2] );
            const float       radius    = light.Radius.value_or( std::numeric_limits<float>::max() );
            const ECS::Entity child     = scene.CreateNewEntity( std::string( light.Name ) );
            ECS::LocalTransform transform = child.GetComponent<ECS::TransformComponent>().GetLocal();
            if ( light.Type == "Directional" )
            {
                auto& data     = child.AddComponent<ECS::DirectionLightComponent>().Data;
                data.Color     = color;
                data.Intensity = light.Intensity;
                // Aimed by Rotation (forward -Z), as UE; `direction` is in the root's frame, as a spot's is.
                transform.Rotation = glm::eulerAngles( ECS::Rules::RotationForTravel( direction ) );
            }
            else if ( light.Type == "Spot" )
            {
                auto& data            = child.AddComponent<ECS::SpotLightComponent>().Data;
                data.Color            = color;
                data.Intensity        = light.Intensity;
                data.Range            = radius;
                data.MinRadius        = kInverseSquareReference;
                data.Falloff          = ECS::LightFalloff::InverseSquare;
                data.InnerConeAngle   = light.InnerConeAngle.value_or( 0.0f );
                data.OuterConeAngle   = light.OuterConeAngle.value_or( 0.0f );
                transform.Translation = position;
                // SpotLightSystem: the cone opens along the world rotation's -Z.
                transform.Rotation = glm::eulerAngles(
                     glm::rotation( glm::vec3( 0.0f, 0.0f, -1.0f ), glm::normalize( direction ) ) );
            }
            else
            {
                auto& data            = child.AddComponent<ECS::PointLightComponent>().Data;
                data.Color            = color;
                data.Intensity        = light.Intensity;
                data.Radius           = radius;
                data.MinRadius        = kInverseSquareReference;
                data.Falloff          = ECS::LightFalloff::InverseSquare;
                transform.Translation = position;
            }
            child.SetLocalTransform( transform );
            scene.Attach( root, child );
            placed.push_back( child );
        }
        return placed;
    }
} // namespace Desert::Editor
