#pragma once

#include <Common/Core/Math/AABB.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <optional>

namespace Desert::Editor::ActorDrop
{
    // WHERE A DROPPED ASSET LANDS, as UE's FActorPositioning answers it.
    //
    // A drop is a ray (the cursor's, or the view centre's for a drop without a mouse). When the ray meets a
    // surface, the asset RESTS on it: its bounds are pushed out along the surface normal until their lowest
    // point along that normal touches the hit point, so a character stands on the ground instead of being
    // buried to the waist (its origin at the hips) or floating (its origin below its feet). When the ray
    // meets nothing, the asset is placed a fixed distance along the ray in front of the camera
    // (ULevelEditorViewportSettings::BackgroundDropDistance), never at the world origin, which may be
    // anywhere relative to what the user is looking at. Pure: no scene, no camera, no mesh.

    // UE's BackgroundDropDistance default (768 units), in this project's centimetres.
    inline constexpr float kBackgroundDropDistance = 768.0f;

    // The point a drop names, and the normal of the surface it rests on (nullopt: the ray met nothing,
    // there is nothing to rest on and no bounds offset applies).
    struct Target
    {
        glm::vec3                Point{ 0.0f };
        std::optional<glm::vec3> SurfaceNormal;
    };

    // The drop's target from what the ray met: the hit point and normal, or the background point.
    [[nodiscard]] inline Target TargetFor( const std::optional<glm::vec3>& hitPoint, const glm::vec3& hitNormal,
                                           const glm::vec3& rayOrigin, const glm::vec3& rayDirection )
    {
        if ( hitPoint )
            return Target{ *hitPoint, glm::normalize( hitNormal ) };
        return Target{ rayOrigin + glm::normalize( rayDirection ) * kBackgroundDropDistance, std::nullopt };
    }

    // THE OFFSET FROM THE SURFACE POINT TO THE ENTITY ORIGIN so the mesh's bounds rest on the surface:
    // -n * min over the eight corners of dot(corner, n), the box scaled by the entity's scale (a new entity
    // has no rotation). A box with no extent (Min above Max, a mesh with no submeshes) rests nowhere: zero.
    [[nodiscard]] inline glm::vec3 RestOffset( const ::Common::Math::AABB& localBounds, const glm::vec3& scale,
                                               const glm::vec3& surfaceNormal )
    {
        if ( localBounds.Min.x > localBounds.Max.x || localBounds.Min.y > localBounds.Max.y ||
             localBounds.Min.z > localBounds.Max.z )
            return glm::vec3( 0.0f );
        const glm::vec3 n      = glm::normalize( surfaceNormal );
        float           lowest = 0.0f;
        bool            first  = true;
        for ( int corner = 0; corner < 8; ++corner )
        {
            const glm::vec3 p( ( corner & 1 ) != 0 ? localBounds.Max.x : localBounds.Min.x,
                               ( corner & 2 ) != 0 ? localBounds.Max.y : localBounds.Min.y,
                               ( corner & 4 ) != 0 ? localBounds.Max.z : localBounds.Min.z );
            const float d = glm::dot( p * scale, n );
            lowest        = first ? d : std::min( lowest, d );
            first         = false;
        }
        return -lowest * n;
    }

    // Where the entity's origin goes for a drop at @p target of a mesh with @p localBounds.
    [[nodiscard]] inline glm::vec3 PlacedOrigin( const Target& target, const ::Common::Math::AABB& localBounds,
                                                 const glm::vec3& scale )
    {
        if ( !target.SurfaceNormal )
            return target.Point;
        return target.Point + RestOffset( localBounds, scale, *target.SurfaceNormal );
    }

} // namespace Desert::Editor::ActorDrop
