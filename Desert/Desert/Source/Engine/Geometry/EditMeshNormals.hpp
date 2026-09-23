#pragma once

#include "EditMesh.hpp"

#include <glm/vec3.hpp>

namespace Desert::Geometry
{
    // Unit normal of t's plane (by winding); the zero vector for a zero-area triangle, which has no plane.
    [[nodiscard]] glm::vec3 TriangleNormal( const EditMesh& mesh, int t );

    // Rebuilds the normal layer (enabling it if needed) as HARD EDGES AT POLYGROUP BOUNDARIES, UE's
    // "recompute normals per polygroup": each vertex gets one element per polygroup among its triangles,
    // the angle-weighted average of those triangles' face normals. Angle weighting, not area weighting, so
    // a corner's normal does not swing towards whichever neighbour was tessellated into bigger triangles.
    // Every previous normal element is dropped.
    void ComputeNormalsByPolyGroup( EditMesh& mesh );
} // namespace Desert::Geometry
