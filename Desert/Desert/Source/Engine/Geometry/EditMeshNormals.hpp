#pragma once

#include "EditMesh.hpp"

#include <Common/Core/ResultStr.hpp>

#include <glm/vec3.hpp>

#include <vector>

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

    // Interior angle of t at corner j (radians); zero for a degenerate corner so it weighs nothing.
    [[nodiscard]] float CornerAngle( const EditMesh& mesh, int t, int corner );
    // The corner (0..2) of t that is vertex v, -1 when v is not a corner of t.
    [[nodiscard]] int TriangleCornerOf( const EditMesh& mesh, int t, int v );

    // The LOCAL form of ComputeNormalsByPolyGroup, for an operation that changed a few vertices: every
    // triangle touching `vertices` gets, at those corners, the angle-weighted normal of the triangles in ITS
    // polygroup there; every other corner keeps its element. A triangle unset in the layer must have all
    // three corners in the set. No normal layer: nothing to do.
    [[nodiscard]] Common::BoolResultStr RebuildNormalsByPolyGroupAt( EditMesh&               mesh,
                                                                     const std::vector<int>& vertices );

    // Tangents for `triangles` from UV layer 0 and the (already rebuilt) normals, one element per corner;
    // without UVs the frame follows the triangle's first edge. No tangent layer: nothing to do.
    [[nodiscard]] Common::BoolResultStr ComputeTangentsAt( EditMesh& mesh, const std::vector<int>& triangles );
} // namespace Desert::Geometry
