// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/
// GroupEdgeInserter.cpp:96-568,920-1188,1299-1648 (PlaneCut mode), adapted: namespace Desert::Geometry, no
// FProgressCancel, plane distances in double (UE: float), a failed SplitEdge returns false (UE ignores the result).
// CreateNewGroups sorts its seed triangles: UE's TSet iterates in insertion order, ours (unordered_set) does not,
// and the seed order decides which component keeps the original group ID. Left out with the header's reasons:
// Retriangulate (587-915, 1193-1283) and bSimplifyAlongPath (1044-1050).
#include "Engine/Geometry/UECore/DynamicMesh/Operations/GroupEdgeInserter.hpp"

#include "Engine/Geometry/UECore/IndexUtil.hpp"
#include "Engine/Geometry/UECore/Operations/EmbedSurfacePath.hpp"
#include "Engine/Geometry/UECore/Selections/MeshConnectedComponents.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Desert::Geometry
{
    namespace GroupEdgeInserterLocals
    {
        using FSplitPoint = FGroupEdgeInserter::FGroupEdgeSplitPoint;
        using FOutParams  = FGroupEdgeInserter::FOptionalOutputParams;

        constexpr int32  InvalidID         = IndexConstants::InvalidID;
        constexpr double KINDA_SMALL_NUMBER = 1e-4;

        double PointPlaneDist( const FVector3d& Point, const FVector3d& Origin, const FVector3d& Normal )
        {
            return ( Point - Origin ).Dot( Normal );
        }

        bool GetEdgeLoopOpposingEdgeAndCorner( const FGroupTopology& Topology, int32 GroupID, int32 GroupEdgeIDIn,
                                               int32 CornerIDIn, int32& GroupEdgeIDOut, int32& CornerIDOut,
                                               int32& BoundaryIndexOut, FOutParams& OptionalOut )
        {
            GroupEdgeIDOut   = InvalidID;
            CornerIDOut      = InvalidID;
            BoundaryIndexOut = InvalidID;
            if ( GroupEdgeIDIn == InvalidID || GroupID == InvalidID )
            {
                return false;
            }

            const FGroupTopology::FGroup* Group = Topology.FindGroupByID( GroupID );
            UE_CHECK( Group );

            for ( int32 i = 0; i < Group->Boundaries.Num(); ++i )
            {
                const FGroupTopology::FGroupBoundary& Boundary       = Group->Boundaries[i];
                const int32                           GroupEdgeIndex = Boundary.GroupEdges.Find( GroupEdgeIDIn );
                if ( GroupEdgeIndex != INDEX_NONE )
                {
                    if ( Boundary.GroupEdges.Num() != 4 )
                    {
                        if ( OptionalOut.ProblemGroupEdgeIDsOut )
                        {
                            OptionalOut.ProblemGroupEdgeIDsOut->Append( Boundary.GroupEdges );
                        }
                        return false;
                    }

                    GroupEdgeIDOut   = Boundary.GroupEdges[( GroupEdgeIndex + 2 ) % 4];
                    BoundaryIndexOut = i;

                    // Get the corner attached to the one we were given
                    if ( CornerIDIn != InvalidID )
                    {
                        const FGroupTopology::FGroupEdge& SideEdge1 =
                            Topology.Edges[Boundary.GroupEdges[( GroupEdgeIndex + 1 ) % 4]];
                        const FGroupTopology::FGroupEdge& SideEdge2 =
                            Topology.Edges[Boundary.GroupEdges[( GroupEdgeIndex + 3 ) % 4]];
                        if ( SideEdge1.EndpointCorners.A == CornerIDIn )
                            CornerIDOut = SideEdge1.EndpointCorners.B;
                        else if ( SideEdge1.EndpointCorners.B == CornerIDIn )
                            CornerIDOut = SideEdge1.EndpointCorners.A;
                        else if ( SideEdge2.EndpointCorners.A == CornerIDIn )
                            CornerIDOut = SideEdge2.EndpointCorners.B;
                        else if ( SideEdge2.EndpointCorners.B == CornerIDIn )
                            CornerIDOut = SideEdge2.EndpointCorners.A;
                    }
                    return true;
                }
            }
            return false;
        }

        void ConvertProportionsToArcLengths( const FGroupTopology& Topology, int32 GroupEdgeID,
                                             const TArray<double>& ProportionsIn, TArray<double>& ArcLengthsOut,
                                             TArray<double>* PerVertexLengthsOut )
        {
            ArcLengthsOut.Reset();
            const double TotalLength = Topology.GetEdgeArcLength( GroupEdgeID, PerVertexLengthsOut );
            for ( double Proportion : ProportionsIn )
            {
                ArcLengthsOut.Add( Proportion * TotalLength );
            }
        }

        /**
         * Inserts vertices along an existing group edge that will be used as endpoints for new group edges.
         * Due to tolerance, multiple inputs can map to the same vertex. Clears EndPointsOut before use.
         */
        bool InsertNewVertexEndpoints( const FGroupEdgeInserter::FEdgeLoopInsertionParams& Params, int32 GroupEdgeID,
                                       int32 StartCornerID, TArray<FSplitPoint>& EndPointsOut, FOutParams& OptionalOut )
        {
            EndPointsOut.Reset();
            if ( Params.SortedInputLengths->Num() == 0 )
            {
                return false;
            }

            const FGroupTopology::FGroupEdge& GroupEdge = Params.Topology->Edges[GroupEdgeID];

            // Our own copies, because we may need to iterate backwards relative to the order in the topology.
            const bool bGoBackward =
                ( GroupEdge.Span.Vertices.Last() == Params.Topology->GetCornerVertexID( StartCornerID ) );
            TArray<int32> SpanVids;
            if ( !bGoBackward )
            {
                SpanVids = GroupEdge.Span.Vertices;
            }
            else
            {
                for ( int i = GroupEdge.Span.Vertices.Num() - 1; i >= 0; --i )
                {
                    SpanVids.Add( GroupEdge.Span.Vertices[i] );
                }
            }

            TArray<double> PerVertexLengths;
            TArray<double> ArcLengths;
            if ( Params.bInputsAreProportions )
            {
                ConvertProportionsToArcLengths( *Params.Topology, GroupEdgeID, *Params.SortedInputLengths, ArcLengths,
                                                &PerVertexLengths );
            }
            else
            {
                ArcLengths = *Params.SortedInputLengths;
                Params.Topology->GetEdgeArcLength( GroupEdgeID, &PerVertexLengths );
            }

            const double TotalLength = PerVertexLengths.Last();
            if ( bGoBackward )
            {
                std::reverse( PerVertexLengths.begin(), PerVertexLengths.end() );
                for ( double& Length : PerVertexLengths )
                {
                    Length = TotalLength - Length;
                }
            }

            // Walk forward selecting existing vertices or adding new ones. NextIndex is always an index into
            // SpanVids/PerVertexLengths of the next vertex in front of the current one.
            int32  CurrentVid       = SpanVids[0];
            double CurrentArcLength = 0;
            int32  NextIndex        = 1;

            for ( double TargetLength : ArcLengths )
            {
                // If the next target is beyond the last vertex, clamp it to the last vertex
                if ( TargetLength > TotalLength + Params.VertexTolerance )
                {
                    TargetLength = TotalLength;
                }

                // Advance until the next vertex would overshoot the target length.
                while ( NextIndex < PerVertexLengths.Num() && PerVertexLengths[NextIndex] <= TargetLength )
                {
                    CurrentVid       = SpanVids[NextIndex];
                    CurrentArcLength = PerVertexLengths[NextIndex];
                    ++NextIndex;
                }

                FSplitPoint SplitPoint;

                auto SetSplitPointToVertex = [&SplitPoint, &SpanVids, &Params, NextIndex]( int32 Vid )
                {
                    SplitPoint.ElementID = Vid;
                    SplitPoint.bIsVertex = true;

                    // Inserted verts must be on an edge and have the forward edge as their tangent.
                    const bool bVertexIsOriginal = ( Vid == SpanVids[NextIndex - 1] );
                    if ( !bVertexIsOriginal )
                    {
                        SplitPoint.Tangent =
                            Normalized( Params.Mesh->GetVertex( SpanVids[NextIndex] ) - Params.Mesh->GetVertex( Vid ) );
                    }
                    else
                    {
                        const FVector3d VertexPosition = Params.Mesh->GetVertex( Vid );
                        SplitPoint.Tangent             = FVector3d::Zero();
                        if ( NextIndex > 1 )
                        {
                            SplitPoint.Tangent +=
                                Normalized( VertexPosition - Params.Mesh->GetVertex( SpanVids[NextIndex - 2] ) );
                        }
                        if ( NextIndex < SpanVids.Num() )
                        {
                            SplitPoint.Tangent +=
                                Normalized( Params.Mesh->GetVertex( SpanVids[NextIndex] ) - VertexPosition );
                        }
                        Normalize( SplitPoint.Tangent );
                    }
                };

                if ( TargetLength - CurrentArcLength <= Params.VertexTolerance )
                {
                    SetSplitPointToVertex( CurrentVid );
                }
                else if ( NextIndex < PerVertexLengths.Num() &&
                          PerVertexLengths[NextIndex] - CurrentArcLength <= Params.VertexTolerance )
                {
                    SetSplitPointToVertex( SpanVids[NextIndex] );
                }
                else
                {
                    // Target must be on the edge that goes to the next vertex
                    const int32 CurrentEid = Params.Mesh->FindEdge( CurrentVid, SpanVids[NextIndex] );
                    if ( !UE_ENSURE( CurrentEid >= 0 ) )
                    {
                        return false;
                    }

                    double SplitT =
                        ( TargetLength - CurrentArcLength ) / ( PerVertexLengths[NextIndex] - CurrentArcLength );

                    // See if the edge is stored backwards relative to the direction we're traveling
                    if ( Params.Mesh->GetEdge( CurrentEid ).Vert.B != SpanVids[NextIndex] )
                    {
                        SplitT = 1 - SplitT;
                    }

                    FDynamicMesh3::FEdgeSplitInfo EdgeSplitInfo;
                    if ( Params.Mesh->SplitEdge( CurrentEid, EdgeSplitInfo, SplitT ) != EMeshResult::Ok )
                    {
                        return false;
                    }

                    if ( OptionalOut.ChangedTidsOut )
                    {
                        OptionalOut.ChangedTidsOut->Add( EdgeSplitInfo.OriginalTriangles.A );
                        if ( EdgeSplitInfo.OriginalTriangles.B != InvalidID )
                        {
                            OptionalOut.ChangedTidsOut->Add( EdgeSplitInfo.OriginalTriangles.B );
                        }
                    }

                    CurrentVid       = EdgeSplitInfo.NewVertex;
                    CurrentArcLength = TargetLength;

                    SplitPoint.ElementID = CurrentVid;
                    SplitPoint.bIsVertex = true;
                    SplitPoint.Tangent   = Normalized( Params.Mesh->GetVertex( SpanVids[NextIndex] ) -
                                                     Params.Mesh->GetVertex( CurrentVid ) );
                }

                EndPointsOut.Add( SplitPoint );
            }
            return true;
        }

        /**
         * Creates a path of FMeshSurfacePoint instances across a group, from a plane cut from start to end. Does
         * not embed it. Assumes the start and end points are on the boundary of the group.
         * @returns false if path could not be found.
         */
        bool GetPlaneCutPath( const FDynamicMesh3& Mesh, int32 GroupID, const FSplitPoint& StartPoint,
                              const FSplitPoint& EndPoint, TArray<TPair<FMeshSurfacePoint, int>>& OutputPath,
                              double VertexCutTolerance, const TSet<int32>& DisallowedVids )
        {
            // Guards against walking in a loop or backwards.
            TSet<int32> CrossedVids;
            TSet<int32> CrossedEids;

            const FVector3d StartPosition = StartPoint.bIsVertex
                                                ? Mesh.GetVertex( StartPoint.ElementID )
                                                : Mesh.GetEdgePoint( StartPoint.ElementID, StartPoint.EdgeTValue );
            const FVector3d EndPosition = EndPoint.bIsVertex ? Mesh.GetVertex( EndPoint.ElementID )
                                                             : Mesh.GetEdgePoint( EndPoint.ElementID, EndPoint.EdgeTValue );

            const FVector3d InPlaneVector = Normalized( EndPosition - StartPosition );

            // Components of the two tangents that are orthogonal to the vector between the points.
            FVector3d NormalA = Normalized( StartPoint.Tangent - StartPoint.Tangent.Dot( InPlaneVector ) * InPlaneVector,
                                            KINDA_SMALL_NUMBER );
            FVector3d NormalB = Normalized( EndPoint.Tangent - EndPoint.Tangent.Dot( InPlaneVector ) * InPlaneVector,
                                            KINDA_SMALL_NUMBER );

            if ( NormalA == FVector3d::Zero() || NormalB == FVector3d::Zero() )
            {
                // A tangent pointed directly toward the destination: the plane would be nonsense.
                return false;
            }

            // Same half space, so that the average represents the closer average of the corresponding lines.
            if ( NormalA.Dot( NormalB ) < 0 )
            {
                NormalB = -NormalB;
            }

            const FVector3d CutPlaneNormal = Normalized( NormalA + NormalB );
            if ( !UE_ENSURE( CutPlaneNormal != FVector3d::Zero() ) )
            {
                return false;
            }
            const FVector3d CutPlaneOrigin = StartPosition;

            // Distances of the current edge's vertices from the plane.
            double CurrentEdgeVertPlaneDistances[2] = { 0, 0 };

            OutputPath.Empty();
            if ( StartPoint.bIsVertex )
            {
                OutputPath.Emplace( FMeshSurfacePoint( StartPoint.ElementID ), InvalidID );
            }
            else
            {
                // The endpoints are not clamped here: clamping by plane distance depends on the plane's orientation
                // and could clamp to different endpoints as multiple paths go through one start/end point.
                const FIndex2i EdgeVids = Mesh.GetEdgeV( StartPoint.ElementID );
                CurrentEdgeVertPlaneDistances[0] =
                    PointPlaneDist( Mesh.GetVertex( EdgeVids.A ), CutPlaneOrigin, CutPlaneNormal );
                CurrentEdgeVertPlaneDistances[1] =
                    PointPlaneDist( Mesh.GetVertex( EdgeVids.B ), CutPlaneOrigin, CutPlaneNormal );
                OutputPath.Emplace( FMeshSurfacePoint::MakeEdgePoint( StartPoint.ElementID, StartPoint.EdgeTValue ),
                                    InvalidID );
            }

            bool  bCurrentPointIsVertex = ( OutputPath[0].Key.PointType == ESurfacePointType::Vertex );
            int32 CurrentElementID      = OutputPath[0].Key.ElementID;
            int32 PointCount            = 1;
            // The triangle traversed to reach the current edge point, so the next step does not backtrack.
            int32 TraversedTid = InvalidID;

            while ( !( CurrentElementID == EndPoint.ElementID && bCurrentPointIsVertex == EndPoint.bIsVertex ) )
            {
                if ( bCurrentPointIsVertex )
                {
                    if ( CrossedVids.Contains( CurrentElementID ) )
                        return false;
                    CrossedVids.Add( CurrentElementID );
                }
                else
                {
                    if ( CrossedEids.Contains( CurrentElementID ) )
                        return false;
                    CrossedEids.Add( CurrentElementID );
                }

                if ( !UE_ENSURE( PointCount < Mesh.EdgeCount() ) )
                {
                    return false;
                }

                if ( bCurrentPointIsVertex )
                {
                    FMeshSurfacePoint NextPoint( InvalidID );
                    const FVector3d   CurrentPosition = OutputPath.Last().Key.Pos( &Mesh );

                    // Find a surrounding triangle of our group that intersects the plane
                    int32 CandidateTraversedTid = InvalidID;
                    for ( int32 Tid : Mesh.VtxTrianglesItr( CurrentElementID ) )
                    {
                        if ( Tid == TraversedTid || Mesh.GetTriangleGroup( Tid ) != GroupID )
                        {
                            continue;
                        }

                        // One of the triangle edges has the endpoint: go straight there.
                        if ( !EndPoint.bIsVertex )
                        {
                            const FIndex3i TriangleEids = Mesh.GetTriEdges( Tid );
                            for ( int32 i = 0; i < 3; ++i )
                            {
                                if ( EndPoint.ElementID == TriangleEids[i] )
                                {
                                    OutputPath.Emplace(
                                        FMeshSurfacePoint::MakeEdgePoint( EndPoint.ElementID, EndPoint.EdgeTValue ),
                                        InvalidID );
                                    return true;
                                }
                            }
                        }

                        const FIndex3i TriangleVids = Mesh.GetTriangle( Tid );
                        const int32    VertA = ( TriangleVids.A == CurrentElementID ) ? TriangleVids.C : TriangleVids.A;
                        const int32    VertB = ( TriangleVids.B == CurrentElementID ) ? TriangleVids.C : TriangleVids.B;
                        if ( EndPoint.bIsVertex && ( EndPoint.ElementID == VertA || EndPoint.ElementID == VertB ) )
                        {
                            OutputPath.Emplace( FMeshSurfacePoint( EndPoint.ElementID ), InvalidID );
                            return true;
                        }

                        // See if one of the other vertices is on the plane (and is therefore the next destination)
                        const double PlaneDistanceA =
                            PointPlaneDist( Mesh.GetVertex( VertA ), CutPlaneOrigin, CutPlaneNormal );
                        const double PlaneDistanceB =
                            PointPlaneDist( Mesh.GetVertex( VertB ), CutPlaneOrigin, CutPlaneNormal );
                        const bool bVertAIsOnPlane = std::abs( PlaneDistanceA ) <= VertexCutTolerance;
                        const bool bVertBIsOnPlane = std::abs( PlaneDistanceB ) <= VertexCutTolerance;

                        // Takes the candidate if it moves more directly toward the destination.
                        auto UpdateNextPoint = [&InPlaneVector, &Mesh, &CurrentPosition,
                                                &NextPoint]( const FMeshSurfacePoint& CandidateSurfacePoint )
                        {
                            if ( NextPoint.ElementID == CandidateSurfacePoint.ElementID &&
                                 NextPoint.PointType == CandidateSurfacePoint.PointType )
                            {
                                return false; // the same point, seen from an adjacent triangle
                            }
                            if ( NextPoint.ElementID == InvalidID ||
                                 ( InPlaneVector.Dot( NextPoint.Pos( &Mesh ) - CurrentPosition ) <
                                   InPlaneVector.Dot( CandidateSurfacePoint.Pos( &Mesh ) - CurrentPosition ) ) )
                            {
                                NextPoint = CandidateSurfacePoint;
                                return true;
                            }
                            return false;
                        };

                        bool bEdgeVertIsPreferred = false;
                        if ( bVertAIsOnPlane && !DisallowedVids.Contains( VertA ) )
                        {
                            bEdgeVertIsPreferred = true;
                            UpdateNextPoint( FMeshSurfacePoint( VertA ) );
                        }
                        if ( bVertBIsOnPlane && !DisallowedVids.Contains( VertB ) )
                        {
                            bEdgeVertIsPreferred = true;
                            UpdateNextPoint( FMeshSurfacePoint( VertB ) );
                        }
                        if ( !bEdgeVertIsPreferred && PlaneDistanceA * PlaneDistanceB < 0 )
                        {
                            // The triangle's opposite edge crosses the plane
                            const int32 Eid        = Mesh.FindEdgeFromTri( VertA, VertB, Tid );
                            double      EdgeTValue = PlaneDistanceA / ( PlaneDistanceA - PlaneDistanceB );
                            if ( VertA != Mesh.GetEdgeV( Eid ).A )
                            {
                                EdgeTValue = 1 - EdgeTValue;
                            }

                            if ( UpdateNextPoint( FMeshSurfacePoint::MakeEdgePoint( Eid, EdgeTValue ) ) )
                            {
                                CurrentEdgeVertPlaneDistances[0] = PlaneDistanceA;
                                CurrentEdgeVertPlaneDistances[1] = PlaneDistanceB;
                                if ( VertA != Mesh.GetEdgeV( Eid ).A )
                                {
                                    std::swap( CurrentEdgeVertPlaneDistances[0], CurrentEdgeVertPlaneDistances[1] );
                                }
                                CandidateTraversedTid = Tid;
                            }
                        }
                    }

                    if ( NextPoint.ElementID == InvalidID )
                    {
                        return false;
                    }
                    OutputPath.Emplace( NextPoint, InvalidID );
                    TraversedTid =
                        ( NextPoint.PointType == ESurfacePointType::Edge ) ? CandidateTraversedTid : InvalidID;
                }
                else
                {
                    const FDynamicMesh3::FEdge Edge = Mesh.GetEdge( CurrentElementID );

                    // We're starting from an edge. Get the triangle that we're dealing with.
                    int32 NextTid;
                    if ( Edge.Tri.A == TraversedTid )
                        NextTid = Edge.Tri.B;
                    else if ( Edge.Tri.B == TraversedTid )
                        NextTid = Edge.Tri.A;
                    else
                        NextTid = Mesh.GetTriangleGroup( Edge.Tri.A ) == GroupID ? Edge.Tri.A : Edge.Tri.B;

                    if ( NextTid == InvalidID || Mesh.GetTriangleGroup( NextTid ) != GroupID )
                    {
                        return false; // a dead end before the end
                    }
                    TraversedTid = NextTid;

                    const int32 OppositeVert =
                        IndexUtil::FindTriOtherVtx( Edge.Vert.A, Edge.Vert.B, Mesh.GetTriangle( NextTid ) );
                    if ( EndPoint.bIsVertex && EndPoint.ElementID == OppositeVert )
                    {
                        OutputPath.Emplace( FMeshSurfacePoint( EndPoint.ElementID ), InvalidID );
                        return true;
                    }

                    if ( !EndPoint.bIsVertex )
                    {
                        const FIndex3i TriangleEids = Mesh.GetTriEdges( NextTid );
                        for ( int32 i = 0; i < 3; ++i )
                        {
                            if ( EndPoint.ElementID == TriangleEids[i] )
                            {
                                OutputPath.Emplace(
                                    FMeshSurfacePoint::MakeEdgePoint( EndPoint.ElementID, EndPoint.EdgeTValue ),
                                    InvalidID );
                                return true;
                            }
                        }
                    }

                    const double OppositeVertPlaneDistance =
                        PointPlaneDist( Mesh.GetVertex( OppositeVert ), CutPlaneOrigin, CutPlaneNormal );
                    if ( std::abs( OppositeVertPlaneDistance ) <= VertexCutTolerance &&
                         !DisallowedVids.Contains( OppositeVert ) )
                    {
                        OutputPath.Emplace( FMeshSurfacePoint( OppositeVert ), InvalidID ); // cutting a vertex
                    }
                    else
                    {
                        // We are cutting through an edge. Figure out which one
                        int32  SecondVertOfNextEdge;
                        double SecondPlaneDistance;
                        if ( CurrentEdgeVertPlaneDistances[0] * OppositeVertPlaneDistance < 0 )
                        {
                            SecondVertOfNextEdge = Edge.Vert.A;
                            SecondPlaneDistance  = CurrentEdgeVertPlaneDistances[0];
                        }
                        else if ( CurrentEdgeVertPlaneDistances[1] * OppositeVertPlaneDistance < 0 )
                        {
                            SecondVertOfNextEdge = Edge.Vert.B;
                            SecondPlaneDistance  = CurrentEdgeVertPlaneDistances[1];
                        }
                        else
                        {
                            // The edge lies in the plane: a bad cutting plane from an edge start.
                            return false;
                        }

                        const int32 Eid = Mesh.FindEdge( OppositeVert, SecondVertOfNextEdge );
                        // Safe: SecondPlaneDistance has the opposite sign to OppositeVertPlaneDistance.
                        double EdgeTValue =
                            OppositeVertPlaneDistance / ( OppositeVertPlaneDistance - SecondPlaneDistance );

                        CurrentEdgeVertPlaneDistances[0] = OppositeVertPlaneDistance;
                        CurrentEdgeVertPlaneDistances[1] = SecondPlaneDistance;
                        if ( OppositeVert != Mesh.GetEdgeV( Eid ).A )
                        {
                            EdgeTValue = 1 - EdgeTValue;
                            std::swap( CurrentEdgeVertPlaneDistances[0], CurrentEdgeVertPlaneDistances[1] );
                        }
                        OutputPath.Emplace( FMeshSurfacePoint::MakeEdgePoint( Eid, EdgeTValue ), InvalidID );
                    }
                }

                ++PointCount;
                UE_CHECK( PointCount == OutputPath.Num() );
                CurrentElementID      = OutputPath.Last().Key.ElementID;
                bCurrentPointIsVertex = ( OutputPath.Last().Key.PointType == ESurfacePointType::Vertex );
            }
            return true;
        }

        /**
         * Places a plane path connecting the endpoints into the mesh, but does not give the triangles new groups
         * yet. Outputs the path edge IDs so that can be done later.
         */
        bool EmbedPlaneCutPath( FDynamicMesh3& Mesh, const FGroupTopology& Topology, int32 GroupID,
                                const FSplitPoint& StartPoint, const FSplitPoint& EndPoint, double VertexTolerance,
                                TSet<int32>& PathEidsOut, TSet<int32>* ChangedTrisOut )
        {
            // No snapping to the group's boundary vertices by plane distance: it could join the boundary at a
            // different point from the one the loop continues from on the other side.
            TSet<int32>                   DisallowedVids;
            const FGroupTopology::FGroup* Group = Topology.FindGroupByID( GroupID );
            if ( UE_ENSURE( Group ) )
            {
                for ( const FGroupTopology::FGroupBoundary& Boundary : Group->Boundaries )
                {
                    for ( int32 GroupEdgeID : Boundary.GroupEdges )
                    {
                        if ( UE_ENSURE( GroupEdgeID < Topology.Edges.Num() ) )
                        {
                            DisallowedVids.Append( Topology.Edges[GroupEdgeID].Span.Vertices );
                        }
                    }
                }
            }

            TArray<TPair<FMeshSurfacePoint, int>> CutPath;
            if ( !GetPlaneCutPath( Mesh, GroupID, StartPoint, EndPoint, CutPath, VertexTolerance, DisallowedVids ) )
            {
                return false;
            }
            UE_CHECK( CutPath.Num() >= 2 );

            if ( ChangedTrisOut )
            {
                for ( int32 i = 0; i < CutPath.Num(); ++i )
                {
                    const FMeshSurfacePoint& Point = CutPath[i].Key;
                    if ( Point.PointType == ESurfacePointType::Edge )
                    {
                        const FIndex2i EdgeTris = Mesh.GetEdgeT( Point.ElementID );
                        ChangedTrisOut->Add( EdgeTris.A );
                        if ( EdgeTris.B != InvalidID )
                        {
                            ChangedTrisOut->Add( EdgeTris.B );
                        }
                    }
                }
            }

            FMeshSurfacePath PathEmbedder( &Mesh );
            PathEmbedder.Path = CutPath;
            TArray<int32> PathVertices;
            if ( !PathEmbedder.EmbedSimplePath( PathVertices, false ) )
            {
                return false;
            }
            UE_CHECK( PathVertices.Num() >= 2 );

            for ( int32 i = 1; i < PathVertices.Num(); ++i )
            {
                const int32 Eid = Mesh.FindEdge( PathVertices[i - 1], PathVertices[i] );
                if ( UE_ENSURE( Eid >= 0 ) )
                {
                    PathEidsOut.Add( Eid );
                }
            }
            return true;
        }

        /** Uses the given path edge IDs to split a group into new groups. */
        bool CreateNewGroups( FDynamicMesh3& Mesh, const TSet<int32>& PathEids, int32 OriginalGroup,
                              int32& NumGroupsCreated, FOutParams& OptionalOut )
        {
            TSet<int32> SeedTriangleSet;
            for ( int32 Eid : PathEids )
            {
                const FIndex2i Tris = Mesh.GetEdgeT( Eid );
                if ( Mesh.GetTriangleGroup( Tris.A ) == OriginalGroup )
                {
                    SeedTriangleSet.Add( Tris.A );
                }
                if ( Tris.B != InvalidID && Mesh.GetTriangleGroup( Tris.B ) == OriginalGroup )
                {
                    SeedTriangleSet.Add( Tris.B );
                }
            }
            TArray<int> Seeds = SeedTriangleSet.Array();
            std::sort( Seeds.begin(), Seeds.end() );

            FMeshConnectedComponents ConnectedComponents( &Mesh );
            ConnectedComponents.FindTrianglesConnectedToSeeds( Seeds,
                                                               [&]( int32 t0, int32 t1 )
                                                               {
                                                                   // Connected only if same group and not across
                                                                   // one of the newly inserted group edges.
                                                                   if ( Mesh.GetTriangleGroup( t0 ) ==
                                                                        Mesh.GetTriangleGroup( t1 ) )
                                                                   {
                                                                       return !PathEids.Contains(
                                                                           Mesh.FindEdgeFromTriPair( t0, t1 ) );
                                                                   }
                                                                   return false;
                                                               } );

            // Assign a new group id for each component. The first component keeps the old group ID.
            for ( int32 i = 1; i < ConnectedComponents.Num(); ++i )
            {
                const FMeshConnectedComponents::FComponent& Component = ConnectedComponents.GetComponent( i );
                if ( OptionalOut.ChangedTidsOut )
                {
                    OptionalOut.ChangedTidsOut->Append( Component.Indices );
                }
                const int32 NewGroupID = Mesh.AllocateTriangleGroup();
                for ( int Tid : Component.Indices )
                {
                    Mesh.SetTriangleGroup( Tid, NewGroupID );
                }
            }

            NumGroupsCreated = ConnectedComponents.Num();
            return true;
        }

        /**
         * Connects multiple vertex endpoints across the same group; StartPoints and EndPoints are 1:1 and ordered
         * sequentially away from the first start point and first end point.
         */
        bool ConnectEndpoints( const FGroupEdgeInserter::FEdgeLoopInsertionParams& Params, int32 GroupID,
                               const TArray<FSplitPoint>& StartPoints, const TArray<FSplitPoint>& EndPoints,
                               int32& NumGroupsCreated, FOutParams& OptionalOut )
        {
            NumGroupsCreated              = 0;
            const int32 NumEdgesToInsert = std::min( StartPoints.Num(), EndPoints.Num() );
            TSet<int32> PathsEids;
            for ( int32 i = 0; i < NumEdgesToInsert; ++i )
            {
                if ( !EmbedPlaneCutPath( *Params.Mesh, *Params.Topology, GroupID, StartPoints[i], EndPoints[i],
                                         Params.VertexTolerance, PathsEids, OptionalOut.ChangedTidsOut ) )
                {
                    return false;
                }
            }

            if ( OptionalOut.NewEidsOut )
            {
                OptionalOut.NewEidsOut->Append( PathsEids );
            }

            return CreateNewGroups( *Params.Mesh, PathsEids, GroupID, NumGroupsCreated, OptionalOut );
        }

        /** Continues the loop in one direction from a start edge. @returns false if there is an error. */
        bool InsertEdgeLoopEdgesInDirection( const FGroupEdgeInserter::FEdgeLoopInsertionParams& Params,
                                             const TArray<FSplitPoint>& StartEndpoints, int32 NextGroupID,
                                             int32 NextEdgeID, int32 NextCornerID, int32 NextBoundaryIndex,
                                             TSet<int32>& AlteredGroups, int32& NumInserted, FOutParams& OptionalOut )
        {
            NumInserted = 0;
            if ( AlteredGroups.Contains( NextGroupID ) || StartEndpoints.Num() == 0 )
            {
                return true;
            }

            TArray<FSplitPoint>  EndpointStorage1 = StartEndpoints;
            TArray<FSplitPoint>  EndpointStorage2;
            TArray<FSplitPoint>* CurrentEndpoints = &EndpointStorage1;
            TArray<FSplitPoint>* NextEndpoints    = &EndpointStorage2;

            bool bHaveNextGroup = true;
            bool bSuccess       = true;
            while ( bHaveNextGroup && !AlteredGroups.Contains( NextGroupID ) )
            {
                // See if we looped around to the start
                if ( NextEdgeID == Params.GroupEdgeID )
                {
                    int32 NumGroupsCreated = 0;
                    bSuccess               = ConnectEndpoints( Params, NextGroupID, *CurrentEndpoints, StartEndpoints,
                                                               NumGroupsCreated, OptionalOut );
                    AlteredGroups.Add( NextGroupID );
                    NumInserted += ( NumGroupsCreated > 1 ? 1 : 0 );
                    break;
                }

                // Otherwise, create next endpoints
                if ( !InsertNewVertexEndpoints( Params, NextEdgeID, NextCornerID, *NextEndpoints, OptionalOut ) )
                {
                    return false;
                }
                if ( NextEndpoints->Num() == 0 )
                {
                    return true; // the next edge was not long enough for the input lengths
                }

                int32 NumGroupsCreated = 0;
                bSuccess = ConnectEndpoints( Params, NextGroupID, *CurrentEndpoints, *NextEndpoints, NumGroupsCreated,
                                             OptionalOut );
                AlteredGroups.Add( NextGroupID );
                NumInserted += ( NumGroupsCreated > 1 ? 1 : 0 );
                if ( !bSuccess )
                {
                    return false;
                }

                // Get the next group edge target
                if ( Params.Topology->IsBoundaryEdge( NextEdgeID ) )
                {
                    break;
                }
                NextGroupID    = Params.Topology->Edges[NextEdgeID].OtherGroupID( NextGroupID );
                bHaveNextGroup = GetEdgeLoopOpposingEdgeAndCorner( *Params.Topology, NextGroupID, NextEdgeID,
                                                                   NextCornerID, NextEdgeID, NextCornerID,
                                                                   NextBoundaryIndex, OptionalOut );
                std::swap( CurrentEndpoints, NextEndpoints );
            }
            return bSuccess;
        }
    } // namespace GroupEdgeInserterLocals

    bool FGroupEdgeInserter::InsertEdgeLoops( const FEdgeLoopInsertionParams& Params, FOptionalOutputParams OptionalOut )
    {
        using namespace GroupEdgeInserterLocals;

        UE_CHECK( Params.Mesh );
        UE_CHECK( Params.Topology );
        UE_CHECK( Params.SortedInputLengths );
        UE_CHECK( Params.GroupEdgeID != InvalidID );
        UE_CHECK( Params.StartCornerID != InvalidID );

        const FGroupTopology::FGroupEdge& GroupEdge = Params.Topology->Edges[Params.GroupEdgeID];

        // Check for a valid path forward or backward first: no edge splits if we have neither.
        const int32 ForwardGroupID = GroupEdge.Groups.A;
        int32       ForwardEdgeID, ForwardCornerID, ForwardBoundaryIndex;
        const bool  bHaveForwardEdge =
            GetEdgeLoopOpposingEdgeAndCorner( *Params.Topology, ForwardGroupID, Params.GroupEdgeID,
                                              Params.StartCornerID, ForwardEdgeID, ForwardCornerID,
                                              ForwardBoundaryIndex, OptionalOut );

        const int32 BackwardGroupID = GroupEdge.Groups.B;
        int32       BackwardEdgeID, BackwardCornerID, BackwardBoundaryIndex;
        const bool  bHaveBackwardEdge =
            GetEdgeLoopOpposingEdgeAndCorner( *Params.Topology, BackwardGroupID, Params.GroupEdgeID,
                                              Params.StartCornerID, BackwardEdgeID, BackwardCornerID,
                                              BackwardBoundaryIndex, OptionalOut );

        if ( !bHaveForwardEdge && !bHaveBackwardEdge )
        {
            return false; // neither neighbour is quad-like
        }

        // The loop ends when it arrives at a group it already altered (it may try to cross itself from the side);
        // this also means the topology need not be updated as we go.
        TSet<int32> AlteredGroups;

        // The first endpoints are kept in case they close the loop. Splitting ahead of time keeps a split from
        // changing the eid of the next endpoint.
        TArray<FGroupEdgeSplitPoint> StartEndpoints;
        bool bSuccess = InsertNewVertexEndpoints( Params, Params.GroupEdgeID, Params.StartCornerID, StartEndpoints,
                                                  OptionalOut );
        if ( !bSuccess || StartEndpoints.Num() == 0 )
        {
            return false;
        }

        // Both directions. For a closed loop the second call does nothing: AlteredGroups has its groups.
        int32 TotalNumInserted = 0;
        if ( bHaveForwardEdge )
        {
            bSuccess = InsertEdgeLoopEdgesInDirection( Params, StartEndpoints, ForwardGroupID, ForwardEdgeID,
                                                       ForwardCornerID, ForwardBoundaryIndex, AlteredGroups,
                                                       TotalNumInserted, OptionalOut );
        }
        if ( bSuccess && bHaveBackwardEdge )
        {
            int32 NumInserted = 0;
            bSuccess          = InsertEdgeLoopEdgesInDirection( Params, StartEndpoints, BackwardGroupID, BackwardEdgeID,
                                                                BackwardCornerID, BackwardBoundaryIndex, AlteredGroups,
                                                                NumInserted, OptionalOut ) &&
                       bSuccess;
            TotalNumInserted += NumInserted;
        }

        if ( TotalNumInserted == 0 )
        {
            return false;
        }
        return Params.Topology->RebuildTopology() && bSuccess;
    }

    bool FGroupEdgeInserter::InsertGroupEdge( FGroupEdgeInsertionParams& Params, FOptionalOutputParams OptionalOut )
    {
        using namespace GroupEdgeInserterLocals;

        UE_CHECK( Params.Mesh );
        UE_CHECK( Params.Topology );
        UE_CHECK( Params.GroupID != InvalidID );
        UE_CHECK( Params.StartPoint.ElementID != InvalidID );
        UE_CHECK( Params.EndPoint.ElementID != InvalidID );

        if ( Params.StartPoint.bIsVertex == Params.EndPoint.bIsVertex &&
             Params.StartPoint.ElementID == Params.EndPoint.ElementID )
        {
            return false; // points are on the same vertex or edge
        }

        TSet<int32>  TempNewEids;
        TSet<int32>* NewEids = OptionalOut.NewEidsOut ? OptionalOut.NewEidsOut : &TempNewEids;

        if ( !EmbedPlaneCutPath( *Params.Mesh, *Params.Topology, Params.GroupID, Params.StartPoint, Params.EndPoint,
                                 Params.VertexTolerance, *NewEids, OptionalOut.ChangedTidsOut ) )
        {
            return false;
        }

        int32 NumGroupsCreated = 0;
        if ( !CreateNewGroups( *Params.Mesh, *NewEids, Params.GroupID, NumGroupsCreated, OptionalOut ) )
        {
            return false;
        }

        Params.Topology->RebuildTopology();
        return true;
    }
} // namespace Desert::Geometry
