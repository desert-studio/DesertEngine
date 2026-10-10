#pragma once

#include <Common/Core/ResultStr.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/System/ColliderMesh.hpp>

namespace Desert::Editor::Tools
{
    // The collision Cube Grid's Accept gives a piece (UE's Cube Grid bakes collision with the mesh): UE's
    // "complex as simple", the triangles on screen, on a static body. A blockout is concave by design - a
    // room, an arch, a corridor - and its bounding box would make it solid inside, so it is never a box.
    // The collider carries no copy of the triangles: Play cooks them from whatever the entity's StaticMesh
    // draws (PhysicsECSSystem::GatherColliderMesh), so the live mesh and an Output: Static Mesh asset both
    // collide as they look, and a later edit of the piece moves its collision with it.
    // Refused, by reason, when the entity has nothing drawn to build the collision from.
    [[nodiscard]] inline Common::BoolResultStr GiveBlockoutCollision( const ECS::Entity& piece )
    {
        if ( !piece.HasComponent<ECS::StaticMeshComponent>() )
            return Common::MakeError<bool>( "the piece has no StaticMesh to build its collision from" );
        if ( ECS::PickColliderMeshSource( piece.GetComponent<ECS::StaticMeshComponent>() ) ==
             ECS::ColliderMeshSource::None )
            return Common::MakeError<bool>( "the piece's StaticMesh has no mesh to build its collision from" );

        auto& collider      = piece.HasComponent<ECS::ColliderComponent>()
                                   ? piece.GetComponent<ECS::ColliderComponent>()
                                   : piece.AddComponent<ECS::ColliderComponent>();
        collider.Data.Shape = Physics::ShapeType::Mesh;

        // A collider alone is inert: the static body is what puts it in the simulation. Static, because Jolt's
        // MeshShape has no volume and so no mass (PhysicsWorld refuses a Mesh on a dynamic body).
        auto& body = piece.HasComponent<ECS::RigidBodyComponent>() ? piece.GetComponent<ECS::RigidBodyComponent>()
                                                                   : piece.AddComponent<ECS::RigidBodyComponent>();
        body.Data.Type = Physics::BodyType::Static;
        return Common::MakeSuccess( true );
    }
} // namespace Desert::Editor::Tools
