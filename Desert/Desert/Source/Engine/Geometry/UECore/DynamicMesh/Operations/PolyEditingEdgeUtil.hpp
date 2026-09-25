// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Public/Operations/
// PolyEditingEdgeUtil.h:17-54, adapted: namespace Desert::Geometry. Only the inset-line functions (used by
// FInsetMeshRegion and FMeshBevel) are ported; ComputeNewGroupIDsAlongEdgeLoop has no caller here.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/LineTypes.hpp"

namespace Desert::Geometry
{
    /**
     * For each edge in EdgeList, compute a line offset from the original edge by distance InsetDistance, towards
     * the centroid of the edge's first triangle (each edge is assumed to be a boundary edge, Tri.A "inside").
     */
    void ComputeInsetLineSegmentsFromEdges( const FDynamicMesh3& Mesh, const TArray<int32>& EdgeList,
                                            double InsetDistance, TArray<FLine3d>& InsetLinesOut );

    /**
     * Solve for a new inset position by intersecting a pair of inset-lines (the midpoint of their closest points),
     * or the nearest point on the first line when the lines are parallel.
     */
    FVector3d SolveInsetVertexPositionFromLinePair( const FVector3d& Position, const FLine3d& InsetEdgeLine1,
                                                    const FLine3d& InsetEdgeLine2 );

    /**
     * Solve new inset vertex positions from consecutive inset-lines: vertex vi sits between line vi-1 and line vi.
     * When bIsLoop is false the two end vertices are projected onto the first/last line instead.
     */
    void SolveInsetVertexPositionsFromInsetLines( const FDynamicMesh3& Mesh, const TArray<FLine3d>& InsetEdgeLines,
                                                  const TArray<int32>& VertexIDs,
                                                  TArray<FVector3d>& VertexPositionsOut, bool bIsLoop );
} // namespace Desert::Geometry
