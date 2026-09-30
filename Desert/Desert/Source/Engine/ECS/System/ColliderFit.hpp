#pragma once

#include <Engine/ECS/Components.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>

namespace Desert::ECS
{
    // UE's Mesh To Collision (USetCollisionGeometryTool): a simple collider that encloses a mesh. The points are
    // the ones a ConvexHull collider is cooked from (ColliderMesh: body space, entity scale applied), so every
    // shape fits what the hull would, in centimetres. Box and Sphere are fit about the points' bounds centre; the
    // capsule runs along the bounds' longest axis. A ConvexHull needs no fit — the body cooks it from the same
    // points.
    inline Common::ResultStr<ColliderData> FitCollider( Physics::ShapeType         shape,
                                                        std::span<const glm::vec3> points )
    {
        if ( points.empty() )
            return Common::MakeError<ColliderData>( "the mesh has no vertices to fit a collider to" );

        glm::vec3 lo = points.front();
        glm::vec3 hi = points.front();
        for ( const glm::vec3& p : points )
        {
            lo = glm::min( lo, p );
            hi = glm::max( hi, p );
        }

        ColliderData out;
        out.Shape  = shape;
        out.Center = ( lo + hi ) * 0.5f;
        switch ( shape )
        {
            case Physics::ShapeType::Box:
                out.HalfExtents = ( hi - lo ) * 0.5f;
                return Common::MakeSuccess( out );
            case Physics::ShapeType::Sphere:
            {
                float r2 = 0.0f;
                for ( const glm::vec3& p : points )
                {
                    const glm::vec3 d = p - out.Center;
                    r2                = std::max( r2, glm::dot( d, d ) );
                }
                out.Radius = std::sqrt( r2 );
                return Common::MakeSuccess( out );
            }
            case Physics::ShapeType::Capsule:
            {
                const glm::vec3 size = hi - lo;
                int             axis = 0;
                if ( size.y > size[axis] )
                    axis = 1;
                if ( size.z > size[axis] )
                    axis = 2;
                constexpr std::array<Physics::CapsuleAxis, 3> kAxes{
                     Physics::CapsuleAxis::X, Physics::CapsuleAxis::Y, Physics::CapsuleAxis::Z };
                out.Axis = kAxes.at( static_cast<size_t>( axis ) );

                // Radius: the farthest point from the axis line. Half height: the shortest cylinder whose caps
                // still cover every point — a point at distance d from the axis needs |t| <= h + sqrt(r^2 - d^2).
                float r2 = 0.0f;
                for ( const glm::vec3& p : points )
                {
                    glm::vec3 d = p - out.Center;
                    d[axis]     = 0.0f;
                    r2          = std::max( r2, glm::dot( d, d ) );
                }
                float h = 0.0f;
                for ( const glm::vec3& p : points )
                {
                    glm::vec3   d     = p - out.Center;
                    const float along = std::abs( d[axis] );
                    d[axis]           = 0.0f;
                    h = std::max( h, along - std::sqrt( std::max( r2 - glm::dot( d, d ), 0.0f ) ) );
                }
                out.Radius     = std::sqrt( r2 );
                out.HalfHeight = h;
                return Common::MakeSuccess( out );
            }
            case Physics::ShapeType::ConvexHull:
                out.Center = glm::vec3( 0.0f ); // the hull's points already sit where they are
                return Common::MakeSuccess( out );
            case Physics::ShapeType::Mesh:
                break;
        }
        return Common::MakeError<ColliderData>( std::format( "Mesh To Collision makes simple collision; shape {} "
                                                             "is the render mesh itself, set it on the collider",
                                                             static_cast<int>( shape ) ) );
    }
} // namespace Desert::ECS
