// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/MeshIndexUtil.cpp:71-215, adapted:
// namespace Desert::Geometry; the two splitters share one walk (UE repeats it verbatim), behaviour unchanged.
#include "Engine/Geometry/UECore/DynamicMesh/MeshIndexUtil.hpp"

namespace Desert::Geometry
{
    FIndex2i FindVertexEdgesInTriangle( const FDynamicMesh3& Mesh, int32_t TriangleID, int32_t VertexID )
    {
        if ( Mesh.IsTriangle( TriangleID ) )
        {
            const FIndex3i TriV     = Mesh.GetTriangle( TriangleID );
            const FIndex3i TriEdges = Mesh.GetTriEdges( TriangleID );
            for ( int32_t j = 0; j < 3; ++j )
            {
                if ( TriV[j] == VertexID )
                    return { TriEdges[( j == 0 ) ? 2 : j - 1], TriEdges[j] };
            }
        }
        return { IndexConstants::InvalidID, IndexConstants::InvalidID };
    }

    int32_t FindSharedEdgeInTriangles( const FDynamicMesh3& Mesh, int32_t Triangle0, int32_t Triangle1 )
    {
        if ( Mesh.IsTriangle( Triangle0 ) && Mesh.IsTriangle( Triangle1 ) )
        {
            const FIndex3i Edges0 = Mesh.GetTriEdges( Triangle0 );
            const FIndex3i Edges1 = Mesh.GetTriEdges( Triangle1 );
            for ( int32_t j = 0; j < 3; ++j )
            {
                if ( Edges1.Contains( Edges0[j] ) )
                    return Edges0[j];
            }
        }
        return IndexConstants::InvalidID;
    }

    namespace
    {
        // Walk both ways from the two triangles of StartEdgeID, stopping at edges IsSplitEdge accepts.
        template <typename SplitEdgeTest>
        bool WalkOneRingSides( const FDynamicMesh3* Mesh, int32_t VertexID, int32_t StartEdgeID,
                               SplitEdgeTest IsSplitEdge, TArray<int32_t>& TriangleSet0,
                               TArray<int32_t>& TriangleSet1 )
        {
            const FIndex2i StartTris = Mesh->GetEdgeT( StartEdgeID );
            if ( StartTris.B < 0 )
                return false;
            for ( int32_t si = 0; si < 2; ++si )
            {
                TArray<int32_t>& CurrentTriangleSet = ( si == 0 ) ? TriangleSet0 : TriangleSet1;
                const int32_t    StartTri           = StartTris[si];
                CurrentTriangleSet.Add( StartTri );
                const int32_t EdgeOtherTri = StartTris[si == 0 ? 1 : 0];
                int32_t       CurTri       = StartTri;
                int32_t       PrevTri      = EdgeOtherTri;
                while ( true )
                {
                    const FIndex3i NextTri = FindNextAdjacentTriangleAroundVtx(
                         Mesh, VertexID, CurTri, PrevTri,
                         [&]( int32_t, int32_t, int32_t Edge ) { return !IsSplitEdge( Edge ); } );
                    if ( NextTri.A == IndexConstants::InvalidID )
                        break;
                    // Looping back to the start means the split edges did not cut the ring: bad arguments.
                    if ( NextTri.A == EdgeOtherTri )
                        return false;
                    CurrentTriangleSet.Add( NextTri.A );
                    PrevTri = CurTri;
                    CurTri  = NextTri.A;
                }
            }
            return TriangleSet0.Num() > 0 && TriangleSet1.Num() > 0;
        }
    } // namespace

    bool SplitBoundaryVertexTrianglesIntoSubsets( const FDynamicMesh3* Mesh, int32_t VertexID, int32_t SplitEdgeID,
                                                  TArray<int32_t>& TriangleSet0, TArray<int32_t>& TriangleSet1 )
    {
        if ( !Mesh->IsVertex( VertexID ) || !Mesh->IsEdge( SplitEdgeID ) )
            return false;
        return WalkOneRingSides(
             Mesh, VertexID, SplitEdgeID, [&]( int32_t Edge ) { return Edge == SplitEdgeID; }, TriangleSet0,
             TriangleSet1 );
    }

    bool SplitInteriorVertexTrianglesIntoSubsets( const FDynamicMesh3* Mesh, int32_t VertexID,
                                                  int32_t SplitEdgeID0, int32_t SplitEdgeID1,
                                                  TArray<int32_t>& TriangleSet0, TArray<int32_t>& TriangleSet1 )
    {
        if ( !Mesh->IsVertex( VertexID ) || !Mesh->IsEdge( SplitEdgeID0 ) || !Mesh->IsEdge( SplitEdgeID1 ) )
            return false;
        return WalkOneRingSides(
             Mesh, VertexID, SplitEdgeID1, [&]( int32_t Edge )
             { return Edge == SplitEdgeID0 || Edge == SplitEdgeID1; }, TriangleSet0, TriangleSet1 );
    }
} // namespace Desert::Geometry
