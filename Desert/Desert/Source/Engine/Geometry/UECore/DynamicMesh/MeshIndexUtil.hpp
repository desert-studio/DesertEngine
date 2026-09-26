// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Public/DynamicMesh/MeshIndexUtil.h:44-73,122-156, adapted:
// UE Core via UECore.hpp, namespace Desert::Geometry. Only what FMeshBevel (MeshBevel.cpp:880-1321) calls is
// ported: FindVertexEdgesInTriangle, FindSharedEdgeInTriangles, the two one-ring splitters and the walk they
// share. TriangleToVertexIDs / VertexToTriangleOneRing / the overlay-element helpers have no caller here.
#pragma once

#include "Engine/Geometry/UECore/UECore.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/IndexTypes.hpp"

namespace Desert::Geometry
{
    /** The two edges of TriangleID that touch VertexID, as (edge ending at it, edge starting at it) in triangle
     *  order; (InvalidID, InvalidID) when VertexID is not a corner of the triangle. */
    FIndex2i FindVertexEdgesInTriangle( const FDynamicMesh3& Mesh, int32_t TriangleID, int32_t VertexID );

    /** The edge shared by two triangles, or InvalidID when they are not adjacent. */
    int32_t FindSharedEdgeInTriangles( const FDynamicMesh3& Mesh, int32_t Triangle0, int32_t Triangle1 );

    /** Split the triangle fan of boundary vertex VertexID into the two sides of interior edge SplitEdgeID. */
    bool SplitBoundaryVertexTrianglesIntoSubsets( const FDynamicMesh3* Mesh, int32_t VertexID, int32_t SplitEdgeID,
                                                  std::vector<int32_t>& TriangleSet0,
                                                  std::vector<int32_t>& TriangleSet1 );

    /** Split the closed one-ring of interior vertex VertexID into the two sides of the path SplitEdgeID0 ->
     *  VertexID -> SplitEdgeID1. Which side lands in Set0 is arbitrary (UE: callers reconcile it). */
    bool SplitInteriorVertexTrianglesIntoSubsets( const FDynamicMesh3* Mesh, int32_t VertexID,
                                                  int32_t SplitEdgeID0, int32_t SplitEdgeID1,
                                                  std::vector<int32_t>& TriangleSet0,
                                                  std::vector<int32_t>& TriangleSet1 );

    /** Next triangle around VertexID from FromTriangleID, not going back to PrevTriangleID, allowed by
     *  TrisConnectedTest(Tri0, Tri1, SharedEdge). Returns (TriangleID, SharedEdgeID, edge index in From). */
    template <typename TrisConnectedPredicate>
    FIndex3i FindNextAdjacentTriangleAroundVtx( const FDynamicMesh3* Mesh, int32_t VertexID,
                                                int32_t FromTriangleID, int32_t PrevTriangleID,
                                                TrisConnectedPredicate TrisConnectedTest )
    {
        const FIndex3i TriEdges = Mesh->GetTriEdges( FromTriangleID );
        FIndex3i       TriNbrTris;
        for ( int32_t j = 0; j < 3; ++j )
        {
            const FIndex2i EdgeT = Mesh->GetEdgeT( TriEdges[j] );
            TriNbrTris[j]        = ( EdgeT.A == FromTriangleID ) ? EdgeT.B : EdgeT.A;
        }
        for ( int32_t j = 0; j < 3; ++j )
        {
            if ( TriNbrTris[j] != PrevTriangleID && Mesh->IsTriangle( TriNbrTris[j] ) )
            {
                const FIndex3i TriVerts = Mesh->GetTriangle( TriNbrTris[j] );
                if ( TriVerts.A == VertexID || TriVerts.B == VertexID || TriVerts.C == VertexID )
                {
                    // UE stops at the first ring neighbour: a refused connection ends the walk.
                    if ( !TrisConnectedTest( FromTriangleID, TriNbrTris[j], TriEdges[j] ) )
                        break;
                    return { TriNbrTris[j], TriEdges[j], j };
                }
            }
        }
        return { IndexConstants::InvalidID, IndexConstants::InvalidID, IndexConstants::InvalidID };
    }
} // namespace Desert::Geometry
