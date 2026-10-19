#pragma once

// A SPLIT SOURCE PLACED AS ITS NODES (IMP-NODES; UE: FbxSceneImport / "Import Into Level" with Combine Meshes off
// spawns one StaticMeshActor per mesh-bearing node, each at the node's transform). A source imported with Combine
// Meshes off has no combined mesh (NodeMeshSplit.hpp WriteStaticMeshImport): its record names the node meshes and
// where each stands (ImportRecord.hpp `Nodes`). Placing it is one root entity for the source and, under it, one
// entity per node with that node's mesh at that node's placement - the nodes stand as the file arranged them, and
// moving the root moves the whole file.

#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <vector>

namespace Desert::Editor
{
    struct PlacedNodeMesh
    {
        std::string         Name;      // the node's name (ImportRecordNode::Name), the entity's name
        Assets::AssetHandle Mesh;      // its `<stem>_<node>.stmesh`
        glm::vec3           Placement; // ImportRecordNode::Placement: the translation under the root
    };

    // One child of @p root per entry of @p nodes, in order: named by the node, a StaticMeshComponent naming its
    // mesh, translated to its placement (no rotation or scale: the node's are in its mesh). Returns the entities
    // created.
    std::vector<ECS::Entity> PlaceNodeActors( ::Desert::Core::Scene& scene, ECS::Entity root,
                                              std::span<const PlacedNodeMesh> nodes );

    // THE SOURCE'S LIGHTS PLACED WITH IT (BISTRO-LIGHTS; UE FbxSceneImport / Interchange: each FBX light becomes a
    // PointLight, SpotLight or DirectionalLight actor at its node). One child of @p root per entry of @p lights
    // (ImportRecord.hpp `Lights`, engine space), named by its node:
    //   - Point: a PointLightComponent at Position;
    //   - Spot: a SpotLightComponent at Position, rotated so its forward (-Z) is Direction, the record's cones;
    //   - Directional: a DirectionLightComponent whose Translation is Direction (the engine's rule: a directional
    //     light's Translation is the way it travels, SystemRules.hpp DirectionalLightTravel).
    // Colour and Intensity are the record's. A point or spot light falls off as UE's do, by the inverse square of
    // the distance in metres (LightFalloff::InverseSquare with a 100 cm Min Radius: full Intensity within a metre,
    // Intensity / d^2 beyond), windowed to zero at the record's Radius - unbounded (FLT_MAX, UE's value) when the
    // file states no decay. Returns the entities created.
    std::vector<ECS::Entity> PlaceSourceLights( ::Desert::Core::Scene& scene, ECS::Entity root,
                                                std::span<const Assets::Serialization::ImportRecordLight> lights );
} // namespace Desert::Editor
