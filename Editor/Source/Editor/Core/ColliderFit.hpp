#pragma once

#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/System/ColliderFit.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>

#include <glm/glm.hpp>

namespace Desert::Editor::Core
{
    // Fitting a collider to the mesh an entity actually draws, for every editor place that does it: the
    // Collider section in Details (Add, "Fit to Mesh Bounds", and its "off the mesh" warning) and Modeling's Mesh
    // To Collision. All of them fit ECS::FitCollider to the points PhysicsECSSystem cooks a hull from — the same
    // source precedence and the same world scale as the body — so the warning, the button that silences it and
    // the body the simulation builds can never disagree.

    // @p shape fit to the entity's mesh. An error names why there is nothing to fit (no mesh, a skinned one, the
    // asset still loading, or a Mesh shape, which is the render triangles themselves).
    inline Common::ResultStr<ECS::ColliderData> FitEntityCollider( const ECS::Entity& entity,
                                                                   Physics::ShapeType shape )
    {
        const glm::mat3 world( entity.GetWorldTransform() );
        const glm::vec3 scale( glm::length( world[0] ), glm::length( world[1] ), glm::length( world[2] ) );
        auto            gathered =
             ECS::PhysicsECSSystem::GatherColliderMesh( *entity.GetRegistry(), entity.GetHandle(), scale );
        if ( !gathered.IsSuccess() )
            return Common::MakeError<ECS::ColliderData>( gathered.GetError() );
        const auto& mesh = gathered.GetValue();
        if ( !mesh.has_value() )
            return Common::MakeError<ECS::ColliderData>( "the entity's mesh is still loading" );
        return ECS::FitCollider( shape, mesh->Points );
    }

    // The collider's half-span along the body axes: what the Details warning compares against the fit.
    inline glm::vec3 ColliderHalfSpan( const ECS::ColliderData& col )
    {
        switch ( col.Shape )
        {
            case Physics::ShapeType::Box:
                return col.HalfExtents;
            case Physics::ShapeType::Capsule:
            {
                glm::vec3 span( col.Radius );
                span[static_cast<int>( col.Axis )] = col.HalfHeight + col.Radius;
                return span;
            }
            default:
                return glm::vec3( col.Radius );
        }
    }

    // Sizes @p col to wrap the entity's mesh, keeping its shape. Returns false (and leaves @p col alone) when
    // there is nothing to fit to.
    inline bool FitColliderToMesh( const ECS::Entity& entity, ECS::ColliderData& col )
    {
        auto fit = FitEntityCollider( entity, col.Shape );
        if ( !fit.IsSuccess() )
            return false;
        col = fit.GetValue();
        return true;
    }
} // namespace Desert::Editor::Core
