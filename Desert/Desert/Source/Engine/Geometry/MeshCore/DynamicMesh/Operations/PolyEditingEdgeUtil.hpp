// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/
// PolyEditingEdgeUtil.h:17-54, adapted: namespace Desert::Geometry. Only the inset-line functions (used by
// InsetMeshRegion and MeshBevel) are ported; ComputeNewGroupIDsAlongEdgeLoop has no caller here.
#pragma once

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/LineTypes.hpp"

namespace Desert::Geometry
{
    /**
     * For each edge in EdgeList, compute a line offset from the original edge by distance InsetDistance, towards
     * the centroid of the edge's first triangle (each edge is assumed to be a boundary edge, Tri.A "inside").
     */
    void ComputeInsetLineSegmentsFromEdges( const DynamicMesh3& Mesh, const std::vector<int32_t>& EdgeList,
                                            double InsetDistance, std::vector<Line3d>& InsetLinesOut );

    /**
     * Solve for a new inset position by intersecting a pair of inset-lines (the midpoint of their closest points),
     * or the nearest point on the first line when the lines are parallel.
     */
    glm::dvec3 SolveInsetVertexPositionFromLinePair( const glm::dvec3& Position, const Line3d& InsetEdgeLine1,
                                                     const Line3d& InsetEdgeLine2 );

    /**
     * Solve new inset vertex positions from consecutive inset-lines: vertex vi sits between line vi-1 and line vi.
     * When bIsLoop is false the two end vertices are projected onto the first/last line instead.
     */
    void SolveInsetVertexPositionsFromInsetLines( const DynamicMesh3&         Mesh,
                                                  const std::vector<Line3d>&  InsetEdgeLines,
                                                  const std::vector<int32_t>& VertexIDs,
                                                  std::vector<glm::dvec3>& VertexPositionsOut, bool bIsLoop );
} // namespace Desert::Geometry
