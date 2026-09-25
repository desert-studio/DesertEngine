// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Selections/QuadGridPatch.cpp:
// 86-174,269-296, adapted: see QuadGridPatch.hpp; UE's ensure(false) on a bad input is the false return alone.
#include "Engine/Geometry/UECore/Selections/QuadGridPatch.hpp"

#include <utility>

namespace Desert::Geometry
{
    bool FQuadGridPatch::InitializeFromQuadPatch( const FDynamicMesh3&            Mesh,
                                                  const TArray<TArray<FIndex2i>>& QuadRowsIn,
                                                  const TArray<TArray<int32>>&    VertexSpansIn )
    {
        if ( VertexSpansIn.Num() < 2 || QuadRowsIn.Num() != VertexSpansIn.Num() - 1 )
            return false;
        const int32 NumV = VertexSpansIn[0].Num();
        const int32 NumQ = QuadRowsIn[0].Num();
        if ( NumQ != NumV - 1 )
            return false;
        for ( int32 j = 1; j < VertexSpansIn.Num(); ++j )
        {
            if ( VertexSpansIn[j].Num() != NumV )
                return false;
        }
        const int32 NumQuadRows = QuadRowsIn.Num();
        for ( int32 j = 1; j < NumQuadRows; ++j )
        {
            if ( QuadRowsIn[j].Num() != NumQ )
                return false;
        }
        NumVertexColsU = NumV;
        NumVertexRowsV = VertexSpansIn.Num();
        VertexSpans    = VertexSpansIn;
        QuadTriangles  = QuadRowsIn;

        bool bAllOK = true;
        for ( int32 j = 0; j < NumQuadRows && bAllOK; ++j )
        {
            for ( int32 k = 0; k < NumQ && bAllOK; ++k )
            {
                // these should be the four vertices of the quad
                const int32 VertexA  = VertexSpans[j][k];
                const int32 VertexB  = VertexSpans[j][k + 1];
                const int32 VertexC  = VertexSpans[j + 1][k + 1];
                const int32 VertexD  = VertexSpans[j + 1][k];
                FIndex2i&   QuadTris = QuadTriangles[j][k];
                if ( !Mesh.IsTriangle( QuadTris.A ) || !Mesh.IsTriangle( QuadTris.B ) )
                {
                    bAllOK = false;
                    break;
                }
                const FIndex3i TriA = Mesh.GetTriangle( QuadTris.A );
                const FIndex3i TriB = Mesh.GetTriangle( QuadTris.B );
                TArray<int32>  TriVerts;
                TriVerts.Add( TriA.A );
                TriVerts.Add( TriA.B );
                TriVerts.Add( TriA.C );
                TriVerts.AddUnique( TriB.A );
                TriVerts.AddUnique( TriB.B );
                TriVerts.AddUnique( TriB.C );
                if ( TriVerts.Num() != 4 || !TriVerts.Contains( VertexA ) || !TriVerts.Contains( VertexB ) ||
                     !TriVerts.Contains( VertexC ) || !TriVerts.Contains( VertexD ) )
                {
                    bAllOK = false;
                    break;
                }
                // we want TriA to be the one containing the edge (Vertex0,Vertex1), so if it's TriB, swap the quad
                if ( TriB.Contains( VertexA ) && TriB.Contains( VertexB ) )
                    std::swap( QuadTris.A, QuadTris.B );
            }
        }
        if ( !bAllOK )
        {
            VertexSpans.Reset();
            QuadTriangles.Reset();
            NumVertexColsU = 0;
            NumVertexRowsV = 0;
            return false;
        }
        return true;
    }

    bool FQuadGridPatch::GetVertexColumn( int32 ColumnIndex, TArray<int32>& VerticesOut ) const
    {
        if ( VertexSpans.IsEmpty() || ColumnIndex < 0 || ColumnIndex >= VertexSpans[0].Num() )
            return false;
        VerticesOut.Reset();
        for ( int32 k = 0; k < VertexSpans.Num(); ++k )
            VerticesOut.Add( VertexSpans[k][ColumnIndex] );
        return true;
    }

    int32 FQuadGridPatch::FindColumnIndex( int32 VertexID ) const
    {
        for ( const TArray<int32>& Span : VertexSpans )
        {
            for ( int32 Index = 0; Index < Span.Num(); ++Index )
            {
                if ( Span[Index] == VertexID )
                    return Index;
            }
        }
        return IndexConstants::InvalidID;
    }
} // namespace Desert::Geometry
