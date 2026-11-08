#pragma once

#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/Math/Simd.hpp>

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

        // THE EIGHT CORNERS IN FOUR-LANE REGISTERS (Common/Core/Math/Simd.hpp; UE FBox::TransformBy's
        // VectorRegister form), bit for bit the plain loop it replaced: corner k (bit 0 = x, bit 1 = y, bit 2 = z
        // at Max) through glm's mat4 * vec4, ((c0 * x + c1 * y) + (c2 * z + c3 * 1)) per lane - the product c3 * 1
        // is c3 exactly - and folded into the box in corner order with glm::min / glm::max's operand order
        // (Simd::Min( corner, acc ) == glm::min( acc, corner )), so NaN and signed-zero ties land as before. The
        // per-axis products and the x + y sums are shared by the corners that use them instead of recomputed: the
        // same operations on the same values. Lane 3 (w) is computed and ignored. Not Arvo's min/max of the
        // products: that sums in another order and starts from no sentinel, so it is not this function's box.
        namespace S          = Common::Math::Simd;
        const float*    m    = &transform[0][0];
        const S::Float4 c0   = S::Load4( m );
        const S::Float4 c1   = S::Load4( m + 4 );
        const S::Float4 c2   = S::Load4( m + 8 );
        const S::Float4 c3   = S::Load4( m + 12 );
        const S::Float4 x[2] = { S::Mul( c0, S::Splat( local.Min.x ) ), S::Mul( c0, S::Splat( local.Max.x ) ) };
        const S::Float4 y[2] = { S::Mul( c1, S::Splat( local.Min.y ) ), S::Mul( c1, S::Splat( local.Max.y ) ) };
        const S::Float4 z[2] = { S::Add( S::Mul( c2, S::Splat( local.Min.z ) ), c3 ),
                                 S::Add( S::Mul( c2, S::Splat( local.Max.z ) ), c3 ) };
        const S::Float4 xy[4] = { S::Add( x[0], y[0] ), S::Add( x[1], y[0] ), S::Add( x[0], y[1] ),
                                  S::Add( x[1], y[1] ) };
        S::Float4       lo    = S::Splat( kNoBoundsSentinel );
        S::Float4       hi    = S::Splat( -kNoBoundsSentinel );
        for ( int corner = 0; corner < 8; ++corner )
        {
            const S::Float4 world = S::Add( xy[corner & 3], z[corner >> 2] );
            lo                    = S::Min( world, lo );
            hi                    = S::Max( world, hi );
        }
        float minLanes[4];
        float maxLanes[4];
        S::Store4( minLanes, lo );
        S::Store4( maxLanes, hi );
        return Common::Math::AABB{ glm::vec3( minLanes[0], minLanes[1], minLanes[2] ),
                                   glm::vec3( maxLanes[0], maxLanes[1], maxLanes[2] ) };
    }
} // namespace Desert::Geometry
