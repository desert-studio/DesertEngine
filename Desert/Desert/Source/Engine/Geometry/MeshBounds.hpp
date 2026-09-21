#pragma once

#include <Common/Core/Math/AABB.hpp>

#include <Engine/Geometry/MeshTypes.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Desert::Geometry
{
    // WHERE A PLACED MESH IS, as a box. Two callers, and they must agree: the LOD policy already asked
    // this question inline (Geometry::SelectLOD) and the culler now asks it as well. A second inline
    // copy of "union of the submesh boxes" is the middle-link defect this project keeps finding — both
    // ends look right and one of them quietly disagrees about what a mesh's extent is.

    // An AABB no point satisfies: Min above Max on every axis. A mesh with NO submeshes has no extent,
    // and reporting a zero-sized box at the origin would be a lie that culls or keeps the object
    // depending on where the camera happens to be looking rather than on the mesh.
    inline constexpr float kNoBoundsSentinel = 1.0e30f;

    [[nodiscard]] inline bool IsEmpty( const Common::Math::AABB& box )
    {
        return box.Min.x > box.Max.x || box.Min.y > box.Max.y || box.Min.z > box.Max.z;
    }

    /// Union of the submesh boxes, in MESH-LOCAL space.
    [[nodiscard]] inline Common::Math::AABB LocalBounds( const std::vector<Submesh>& submeshes )
    {
        Common::Math::AABB box{ glm::vec3( kNoBoundsSentinel ), glm::vec3( -kNoBoundsSentinel ) };
        for ( const auto& sm : submeshes )
        {
            box.Min = glm::min( box.Min, sm.BoundingBox.Min );
            box.Max = glm::max( box.Max, sm.BoundingBox.Max );
        }
        return box;
    }

    /// The local box carried into WORLD space: the AABB of the eight transformed corners.
    ///
    /// EIGHT CORNERS, not `transform * Min` and `transform * Max`. The two-corner shortcut is correct
    /// only for a transform with no rotation: a 45-degree yaw maps the two opposite corners onto a box
    /// that no longer contains the object, and the object then vanishes near the screen edge. Every
    /// building in the world-scale acceptance scene is rotated, so the shortcut would have been wrong
    /// on the very scene this exists to make runnable.
    [[nodiscard]] inline Common::Math::AABB TransformBounds( const glm::mat4&          transform,
                                                             const Common::Math::AABB& local )
    {
        if ( IsEmpty( local ) )
        {
            return local;
        }

        Common::Math::AABB out{ glm::vec3( kNoBoundsSentinel ), glm::vec3( -kNoBoundsSentinel ) };
        for ( int corner = 0; corner < 8; ++corner )
        {
            const glm::vec3 localCorner( ( corner & 1 ) != 0 ? local.Max.x : local.Min.x,
                                         ( corner & 2 ) != 0 ? local.Max.y : local.Min.y,
                                         ( corner & 4 ) != 0 ? local.Max.z : local.Min.z );
            const glm::vec3 worldCorner = glm::vec3( transform * glm::vec4( localCorner, 1.0f ) );
            out.Min                     = glm::min( out.Min, worldCorner );
            out.Max                     = glm::max( out.Max, worldCorner );
        }
        return out;
    }
} // namespace Desert::Geometry
