// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Operations/EmbedSurfacePath.cpp:13-125,591-634,
// 682-947, adapted: UE Core via UECore.hpp, namespace Desert::Geometry, TStaticArray is std::array. Left out with
// the header's reasons: the FEmbedSimplePathSettings branches (694-805, 846-887) and the bUpdatePath tail
// (941-944). Where UE carries on after an ensure or ignores an EMeshResult, this returns false instead: a split or
// poke that did not happen leaves no new vertex to put on the path, and an end point whose edge was split away can
// no longer be placed.
#include "Engine/Geometry/UECore/Operations/EmbedSurfacePath.hpp"

#include "Engine/Geometry/UECore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/UECore/SegmentTypes.hpp"
#include "Engine/Geometry/UECore/TriangleTypes.hpp"

#include <array>

namespace Desert::Geometry
{
    FVector3d FMeshSurfacePoint::Pos( const FDynamicMesh3* Mesh ) const
    {
        if ( PointType == ESurfacePointType::Vertex )
        {
            return Mesh->GetVertex( ElementID );
        }
        if ( PointType == ESurfacePointType::Edge )
        {
            FVector3d EA;
            FVector3d EB;
            Mesh->GetEdgeV( ElementID, EA, EB );
            // Note this is equivalent to Lerp(EA, EB, BaryCoord[1])
            return BaryCoord[0] * EA + BaryCoord[1] * EB;
        }
        // PointType == ESurfacePointType::Triangle
        FVector3d TA;
        FVector3d TB;
        FVector3d TC;
        Mesh->GetTriVertices( ElementID, TA, TB, TC );
        return BaryCoord[0] * TA + BaryCoord[1] * TB + BaryCoord[2] * TC;
    }

    namespace
    {
        /**
         * Helper function to snap a triangle surface point to the triangle vertices or edges if it's close enough.
         * Input SurfacePt must be a triangle.
         */
        void RefineSurfacePtFromTriangleToSubElement( const FDynamicMesh3* Mesh, const FVector3d& Pos,
                                                      FMeshSurfacePoint& SurfacePt, double SnapElementThresholdSq )
        {
            // expect this to only be called on SurfacePoints with PointType == Triangle; otherwise indicative of
            // incorrect usage
            if ( !UE_ENSURE( SurfacePt.PointType == ESurfacePointType::Triangle ) )
            {
                return;
            }
            const int      TriID             = SurfacePt.ElementID;
            const FIndex3i TriVertIDs        = Mesh->GetTriangle( TriID );
            int            BestSubIdx        = -1;
            double         BestElementDistSq = 0;
            for ( int VertSubIdx = 0; VertSubIdx < 3; VertSubIdx++ )
            {
                const double DistSq = DistanceSquared( Pos, Mesh->GetVertex( TriVertIDs[VertSubIdx] ) );
                if ( DistSq <= SnapElementThresholdSq && ( BestSubIdx == -1 || DistSq < BestElementDistSq ) )
                {
                    BestSubIdx        = VertSubIdx;
                    BestElementDistSq = DistSq;
                }
            }
            if ( BestSubIdx > -1 )
            {
                SurfacePt.ElementID = TriVertIDs[BestSubIdx];
                SurfacePt.PointType = ESurfacePointType::Vertex;
                return;
            }

            // failed to snap to vertex, try snapping to edge
            const FIndex3i TriEdgeIDs    = Mesh->GetTriEdges( TriID );
            double         BestEdgeParam = 0;
            for ( int EdgeSubIdx = 0; EdgeSubIdx < 3; EdgeSubIdx++ )
            {
                const int EdgeID = TriEdgeIDs[EdgeSubIdx];
                FVector3d EPosA;
                FVector3d EPosB;
                Mesh->GetEdgeV( EdgeID, EPosA, EPosB );
                const FSegment3d EdgeSeg( EPosA, EPosB );
                const double     DistSq = EdgeSeg.DistanceSquared( Pos );
                if ( DistSq <= SnapElementThresholdSq && ( BestSubIdx == -1 || DistSq < BestElementDistSq ) )
                {
                    BestSubIdx        = EdgeSubIdx;
                    BestElementDistSq = DistSq;
                    BestEdgeParam     = EdgeSeg.ProjectUnitRange( Pos );
                }
            }
            if ( BestSubIdx > -1 )
            {
                SurfacePt = FMeshSurfacePoint::MakeEdgePoint( TriEdgeIDs[BestSubIdx], BestEdgeParam );
                return;
            }

            // no snapping to be done, leave surfacept on the triangle
        }

        // For when a triangle is replaced by multiple triangles, create a new surface point for the point's new
        // location among the smaller triangles.
        template <typename TIterableTrisType>
        FMeshSurfacePoint RelocateTrianglePointAfterRefinement( const FDynamicMesh3* Mesh, const FVector3d& Pos,
                                                                const TIterableTrisType& TriIDs,
                                                                double                   SnapElementThresholdSq )
        {
            double    BestTriDistSq = 0;
            FVector3d BestBaryCoords;
            int       BestTriID = -1;
            for ( const int TriID : TriIDs )
            {
                UE_CHECK( Mesh->IsTriangle( TriID ) );
                const FIndex3i    TriVertIDs = Mesh->GetTriangle( TriID );
                const FTriangle3d Tri( Mesh->GetVertex( TriVertIDs.A ), Mesh->GetVertex( TriVertIDs.B ),
                                       Mesh->GetVertex( TriVertIDs.C ) );
                // heavy duty way to get barycentric coordinates and check if on triangle; should be robust to
                // degenerate triangles unlike VectorUtil's barycentric coordinate function
                FDistPoint3Triangle3d TriDist( Pos, Tri );
                const double          DistSq = TriDist.GetSquared();
                if ( BestTriID == -1 || DistSq < BestTriDistSq )
                {
                    BestTriID      = TriID;
                    BestTriDistSq  = DistSq;
                    BestBaryCoords = TriDist.TriangleBaryCoords;
                }
            }
            UE_ENSURE( Mesh->IsTriangle( BestTriID ) );
            FMeshSurfacePoint SurfacePt( BestTriID, BestBaryCoords );
            RefineSurfacePtFromTriangleToSubElement( Mesh, Pos, SurfacePt, SnapElementThresholdSq );
            return SurfacePt;
        }
    } // namespace

    bool FMeshSurfacePath::IsConnected() const
    {
        for ( int Idx = 1, LastIdx = 0; Idx < Path.Num(); LastIdx = Idx++ )
        {
            const int WalkingOnTri = Path[LastIdx].Value;
            if ( !Mesh->IsTriangle( WalkingOnTri ) )
            {
                return false;
            }
            const int Inds[2] = { LastIdx, Idx };
            for ( const int Ind : Inds )
            {
                const FMeshSurfacePoint& P = Path[Ind].Key;
                switch ( P.PointType )
                {
                    case ESurfacePointType::Triangle:
                        if ( P.ElementID != WalkingOnTri )
                        {
                            return false;
                        }
                        break;
                    case ESurfacePointType::Edge:
                        if ( !Mesh->GetEdgeT( P.ElementID ).Contains( WalkingOnTri ) )
                        {
                            return false;
                        }
                        break;
                    case ESurfacePointType::Vertex:
                        if ( !Mesh->GetTriangle( WalkingOnTri ).Contains( P.ElementID ) )
                        {
                            return false;
                        }
                        break;
                }
            }
        }
        return true;
    }

    bool FMeshSurfacePath::EmbedSimplePath( TArray<int>& PathVertices, bool bDoNotDuplicateFirstVertexID,
                                            double SnapElementThresholdSq )
    {
        // used to track where the new vertices for *this* path start; used for bDoNotDuplicateFirstVertexID
        const int32_t InitialPathIdx = PathVertices.Num();

        if ( Path.Num() == 0 )
        {
            return true;
        }

        const int32_t            PathNum   = Path.Num();
        const FMeshSurfacePoint& OrigEndPt = Path[PathNum - 1].Key;
        // If FinalTri is split or poked, we will need to re-locate the last point in the path
        int  StartProcessIdx         = 0;
        int  EndSimpleProcessIdx     = PathNum - 1;
        bool bEndPointSpecialProcess = false;
        if ( PathNum > 1 && OrigEndPt.PointType == ESurfacePointType::Triangle )
        {
            EndSimpleProcessIdx     = PathNum - 2;
            bEndPointSpecialProcess = true;
        }
        FMeshSurfacePoint EndPtUpdated = Path.Last().Key;
        const FVector3d   EndPtPos     = OrigEndPt.Pos( Mesh );

        if ( Path[0].Key.PointType == ESurfacePointType::Triangle )
        {
            // poke triangle, and place initial vertex
            FDynamicMesh3::FPokeTriangleInfo PokeInfo;
            if ( !UE_ENSURE( Mesh->PokeTriangle( Path[0].Key.ElementID, Path[0].Key.BaryCoord, PokeInfo ) ==
                             EMeshResult::Ok ) )
            {
                return false;
            }
            if ( EndPtUpdated.PointType == ESurfacePointType::Triangle &&
                 Path[0].Key.ElementID == EndPtUpdated.ElementID )
            {
                const std::array<int, 3> EndCandidateTris{ PokeInfo.NewTriangles.A, PokeInfo.NewTriangles.B,
                                                           PokeInfo.OriginalTriangle };
                EndPtUpdated = RelocateTrianglePointAfterRefinement( Mesh, EndPtPos, EndCandidateTris,
                                                                     SnapElementThresholdSq );
            }
            PathVertices.Add( PokeInfo.NewVertex );
            StartProcessIdx = 1;
        }

        for ( int32_t PathIdx = StartProcessIdx; PathIdx <= EndSimpleProcessIdx; PathIdx++ )
        {
            if ( !UE_ENSURE( Path[PathIdx].Key.PointType != ESurfacePointType::Triangle ) )
            {
                // Input assumptions violated -- Simple path can only have Triangle points at the very first and/or
                // last points!  Would need a more powerful embed function to handle this case.
                return false;
            }
            const FMeshSurfacePoint& Pt = Path[PathIdx].Key;
            if ( Pt.PointType == ESurfacePointType::Edge )
            {
                FDynamicMesh3::FEdgeSplitInfo SplitInfo;
                if ( !UE_ENSURE( Mesh->SplitEdge( Pt.ElementID, SplitInfo, Pt.GetEdgeSplitParam() ) ==
                                 EMeshResult::Ok ) )
                {
                    return false;
                }
                PathVertices.Add( SplitInfo.NewVertex );
                if ( EndPtUpdated.PointType == ESurfacePointType::Triangle &&
                     SplitInfo.OriginalTriangles.Contains( EndPtUpdated.ElementID ) )
                {
                    const std::array<int, 2> TriInds{ EndPtUpdated.ElementID,
                                                      SplitInfo.OriginalTriangles.A == EndPtUpdated.ElementID
                                                           ? SplitInfo.NewTriangles.A
                                                           : SplitInfo.NewTriangles.B };
                    EndPtUpdated =
                         RelocateTrianglePointAfterRefinement( Mesh, EndPtPos, TriInds, SnapElementThresholdSq );
                }
                else if ( PathIdx != PathNum - 1 && EndPtUpdated.PointType == ESurfacePointType::Edge &&
                          Pt.ElementID == EndPtUpdated.ElementID )
                {
                    // The end point's edge is gone and UE has no relocation for this case.
                    UE_ENSURE( false );
                    return false;
                }
            }
            else
            {
                UE_ENSURE( Pt.PointType == ESurfacePointType::Vertex );
                UE_ENSURE( Mesh->IsVertex( Pt.ElementID ) );
                // make sure we don't add a duplicate vertex for the very first vertex (occurs when appending paths
                // sequentially)
                if ( !bDoNotDuplicateFirstVertexID || PathVertices.Num() != InitialPathIdx ||
                     0 == PathVertices.Num() || PathVertices.Last() != Pt.ElementID )
                {
                    PathVertices.Add( Pt.ElementID );
                }
            }
        }

        if ( bEndPointSpecialProcess )
        {
            if ( EndPtUpdated.PointType == ESurfacePointType::Triangle )
            {
                FDynamicMesh3::FPokeTriangleInfo PokeInfo;
                if ( !UE_ENSURE( Mesh->PokeTriangle( EndPtUpdated.ElementID, EndPtUpdated.BaryCoord, PokeInfo ) ==
                                 EMeshResult::Ok ) )
                {
                    return false;
                }
                PathVertices.Add( PokeInfo.NewVertex );
            }
            else if ( EndPtUpdated.PointType == ESurfacePointType::Edge )
            {
                FDynamicMesh3::FEdgeSplitInfo SplitInfo;
                if ( !UE_ENSURE( Mesh->SplitEdge( EndPtUpdated.ElementID, SplitInfo,
                                                  EndPtUpdated.GetEdgeSplitParam() ) == EMeshResult::Ok ) )
                {
                    return false;
                }
                PathVertices.Add( SplitInfo.NewVertex );
            }
            else
            {
                if ( PathVertices.Num() == 0 || PathVertices.Last() != EndPtUpdated.ElementID )
                {
                    PathVertices.Add( EndPtUpdated.ElementID );
                }
            }
        }

        return true;
    }
} // namespace Desert::Geometry
