// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/
// PolyEditingEdgeUtil.cpp:11-94, adapted: namespace Desert::Geometry, behaviour unchanged.
#include "Engine/Geometry/UECore/DynamicMesh/Operations/PolyEditingEdgeUtil.hpp"

#include "Engine/Geometry/UECore/Distance/DistLine3Line3.hpp"

#include <cmath>

namespace Desert::Geometry
{
    void ComputeInsetLineSegmentsFromEdges( const FDynamicMesh3& Mesh, const TArray<int32>& EdgeList,
                                            double InsetDistance, TArray<FLine3d>& InsetLinesOut )
    {
        const int32 NumEdges = EdgeList.Num();
        InsetLinesOut.SetNum( NumEdges );
        for ( int32 k = 0; k < NumEdges; ++k )
        {
            if ( Mesh.IsEdge( EdgeList[k] ) )
            {
                const FDynamicMesh3::FEdge EdgeVT   = Mesh.GetEdge( EdgeList[k] );
                const FVector3d            A        = Mesh.GetVertex( EdgeVT.Vert.A );
                const FVector3d            B        = Mesh.GetVertex( EdgeVT.Vert.B );
                const FVector3d            EdgeDir  = Normalized( A - B );
                const FVector3d            Midpoint = ( A + B ) * 0.5;
                FVector3d                  Normal, Centroid;
                double                     Area;
                Mesh.GetTriInfo( EdgeVT.Tri.A, Normal, Area, Centroid );

                FVector3d InsetDir = Normal.Cross( EdgeDir );
                if ( ( Centroid - Midpoint ).Dot( InsetDir ) < 0 )
                {
                    InsetDir = InsetDir * -1.0;
                }
                InsetLinesOut[k] = FLine3d( Midpoint + InsetDistance * InsetDir, EdgeDir );
            }
            else
            {
                // UE keeps a default line here too: the caller passed an edge that no longer exists.
                InsetLinesOut[k] = FLine3d();
            }
        }
    }

    FVector3d SolveInsetVertexPositionFromLinePair( const FVector3d& Position, const FLine3d& InsetEdgeLine1,
                                                    const FLine3d& InsetEdgeLine2 )
    {
        if ( std::abs( InsetEdgeLine1.Direction.Dot( InsetEdgeLine2.Direction ) ) > 0.999 )
        {
            // The lines are parallel, so their intersection is not useful: use the nearest point on either line.
            return InsetEdgeLine1.NearestPoint( Position );
        }
        // Inset the point to the intersection point of the two lines.
        FDistLine3Line3d Distance( InsetEdgeLine1, InsetEdgeLine2 );
        Distance.GetSquared();
        return 0.5 * ( Distance.Line1ClosestPoint + Distance.Line2ClosestPoint );
    }

    void SolveInsetVertexPositionsFromInsetLines( const FDynamicMesh3& Mesh, const TArray<FLine3d>& InsetEdgeLines,
                                                  const TArray<int32>& VertexIDs,
                                                  TArray<FVector3d>& VertexPositionsOut, bool bIsLoop )
    {
        const int32 NumVertices = VertexIDs.Num();
        VertexPositionsOut.SetNum( NumVertices );

        int32 StartIndex = 0, EndIndex = NumVertices;

        // An open vertex span has no two lines to intersect at its start/end, so those use the nearest points.
        if ( bIsLoop == false )
        {
            StartIndex = 1;
            EndIndex   = NumVertices - 1;

            VertexPositionsOut[0]     = InsetEdgeLines[0].NearestPoint( Mesh.GetVertex( VertexIDs[0] ) );
            VertexPositionsOut.Last() = InsetEdgeLines.Last().NearestPoint( Mesh.GetVertex( VertexIDs.Last() ) );
        }

        for ( int32 vi = StartIndex; vi < EndIndex; ++vi )
        {
            const FLine3d& PrevLine = ( vi == 0 ) ? InsetEdgeLines.Last() : InsetEdgeLines[vi - 1];
            const FLine3d& NextLine = InsetEdgeLines[vi];
            VertexPositionsOut[vi] =
                 SolveInsetVertexPositionFromLinePair( Mesh.GetVertex( VertexIDs[vi] ), PrevLine, NextLine );
        }
    }
} // namespace Desert::Geometry
