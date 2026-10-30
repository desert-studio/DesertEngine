#pragma once

// The engine-side adapter over the ported QEM simplifier (MeshCore QemSimplification) for runtime builders.

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"

namespace Desert::Geometry
{
    // The proxy simplifier of a MeshSimplify HLOD layer (UE UHLODBuilderMeshSimplify): QEM collapses @p mesh, in
    // place, down to @p targetTriangles with the simplifier's default constraints, then recomputes the primary
    // normal overlay from the new surface (the collapses moved vertices, so the old corner normals describe a
    // surface that no longer exists). Unlike SimplifyMesh it refuses nothing: a target the mesh already meets
    // leaves it as it is, because an HLOD layer at 100 % is a merge, not an error. An adapter of its
    // own, not MeshRegionOperation: that object also drives SubdividePoly, and linking it into the player
    // (Runtime) pulled OpenSubdiv in. One QEM: QemSimplification, which the editor's SimplifyMesh drives too.
    void SimplifyToTriangleCount( DynamicMesh3& mesh, int targetTriangles );
} // namespace Desert::Geometry
