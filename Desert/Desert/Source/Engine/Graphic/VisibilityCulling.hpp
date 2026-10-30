#pragma once

#include <Common/Core/Math/AABB.hpp>

#include <Engine/Core/Frustum.hpp>
#include <Engine/Geometry/LODSelection.hpp>
#include <Engine/Geometry/MeshBounds.hpp>

#include <glm/glm.hpp>

#include <cmath>

namespace Desert::Graphic
{
    // WHETHER A PLACED MESH IS WORTH SUBMITTING. One rule, in a header, so the DECISION is reachable by a
    // test: 85 % of this engine's translation units are compiled by no suite, and a rule left inline in
    // MeshRenderer.cpp would have been one of them (see scripts/CI/UnreachedSources.sh).
    //
    // THE DEFAULT IS TO DRAW. A mesh with no extent — no submeshes, or bounds that were never filled in —
    // answers "I do not know where this is", and that must never be answered with "so it is not there".
    // Every failure of this function has to land on the side that costs a draw call, because the other
    // side costs a hole in the picture that nothing reports and only some camera angles show.
    [[nodiscard]] inline bool IsVisibleInView( const Core::Frustum& frustum, const glm::mat4& transform,
                                               const Common::Math::AABB& localBounds )
    {
        if ( Geometry::IsEmpty( localBounds ) )
        {
            return true;
        }
        return frustum.Intersects( Geometry::TransformBounds( transform, localBounds ) );
    }
    // The same rule for a box already in world space (a shadow caster's gathered box,
    // MeshRenderer::GatherShadowCasters): empty = no known extent = drawn.
    [[nodiscard]] inline bool IsVisibleInView( const Core::Frustum&      frustum,
                                               const Common::Math::AABB& worldBounds )
    {
        return Geometry::IsEmpty( worldBounds ) || frustum.Intersects( worldBounds );
    }

    // HOW FAR A VIEW'S LENS STRETCHES EVERY DRAW AND CULL DISTANCE. Distances are authored for a 90-degree
    // HORIZONTAL lens; through a lens of horizontal focal scale k an object at distance d is as wide on screen as
    // it would be at d / k through that one, so it stays drawn to k times the authored distance. A zoomed-in
    // camera sees farther, a wide one nearer.
    // HORIZONTAL, |Projection[0][0]| alone, and not LOD's max of both focal terms: that max is the larger screen
    // axis, so it grows with the viewport's aspect — a 90-degree editor view (FOV is horizontal there, UE's
    // Maintain X FOV) 1.67 times wider than tall read 1.67 and drew a 5000 cm limit out to 8370 cm. A draw
    // distance is a property of the lens, not of the window's shape; UE's DesiredFOV is horizontal too.
    // UE scales MaxDrawDistance and the foliage CullDistance by ViewDistanceScale x
    // CalculateFieldOfViewDistanceScale(View.DesiredFOV) (SceneVisibility.cpp; Engine/Public/UnrealEngine.h), a
    // per-project lerp over FOV in cvars; here the lens itself is the scale, with no curve to tune.
    // 1 for an orthographic projection (|Projection[2][3]| = 0): its "distance" is not a size.
    [[nodiscard]] inline float DrawDistanceScale( const Geometry::LODView& view )
    {
        if ( std::abs( view.Projection[2][3] ) == 0.0f )
            return 1.0f;
        const float focal = std::abs( view.Projection[0][0] );
        return focal > 0.0f ? focal : 1.0f;
    }

    // WHETHER A PLACED MESH IS WITHIN ITS MAX DRAW DISTANCE (UE's LDMaxDrawDistance) from @p view: the distance
    // to its bounds' centre, against the authored limit stretched by the lens. maxDrawDistance <= 0: no limit.
    // A mesh with no extent is measured at its origin — it still has a place, unlike in the frustum test.
    [[nodiscard]] inline bool IsWithinDrawDistance( const Geometry::LODView& view, const glm::mat4& transform,
                                                    const Common::Math::AABB& localBounds, float maxDrawDistance )
    {
        if ( maxDrawDistance <= 0.0f )
            return true;
        const glm::vec3 localCentre =
             Geometry::IsEmpty( localBounds ) ? glm::vec3( 0.0f ) : 0.5f * ( localBounds.Min + localBounds.Max );
        const glm::vec3 centre = glm::vec3( transform * glm::vec4( localCentre, 1.0f ) );
        const glm::vec3 offset = centre - view.Origin;
        const float     limit  = maxDrawDistance * DrawDistanceScale( view );
        return glm::dot( offset, offset ) <= limit * limit;
    }
} // namespace Desert::Graphic
