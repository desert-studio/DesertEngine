// Ported from UE 5.8 Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/
// GroupEdgeInserter.cpp:96-568,920-1188,1299-1648 (PlaneCut mode), adapted: namespace Desert::Geometry, no
// FProgressCancel, plane distances in double (UE: float), a failed SplitEdge returns false (UE ignores the
// result). CreateNewGroups sorts its seed triangles: UE's TSet iterates in insertion order, ours (unordered_set)
// does not, and the seed order decides which component keeps the original group ID. Left out with the header's
// reasons: Retriangulate (587-915, 1193-1283) and bSimplifyAlongPath (1044-1050).
#include "Engine/Geometry/UECore/DynamicMesh/Operations/GroupEdgeInserter.hpp"

#include "Engine/Geometry/UECore/IndexUtil.hpp"
#include "Engine/Geometry/UECore/Operations/EmbedSurfacePath.hpp"
#include "Engine/Geometry/UECore/Selections/MeshConnectedComponents.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <Common/Core/Core.hpp>

namespace Desert::Geometry
{
    namespace GroupEdgeInserterLocals
    {
        using SplitPoint = GroupEdgeInserter::GroupEdgeSplitPoint;
        using OutParams  = GroupEdgeInserter::OptionalOutputParams;

        constexpr int32_t InvalidID          = IndexConstants::InvalidID;
        constexpr double KINDA_SMALL_NUMBER = 1e-4;

        double PointPlaneDist( const glm::dvec3& Point, const glm::dvec3& Origin, const glm::dvec3& Normal )
        {
            return glm::dot( ( Point - Origin ), Normal );
        }

        bool GetEdgeLoopOpposingEdgeAndCorner( const GroupTopology& Topology, int32_t GroupID,
                                               int32_t GroupEdgeIDIn, int32_t CornerIDIn, int32_t& GroupEdgeIDOut,
                                               int32_t& CornerIDOut, int32_t& BoundaryIndexOut,
                                               OutParams& OptionalOut )
        {
            GroupEdgeIDOut   = InvalidID;
            CornerIDOut      = InvalidID;
            BoundaryIndexOut = InvalidID;
            if ( GroupEdgeIDIn == InvalidID || GroupID == InvalidID )
            {
                return false;
            }

            const GroupTopology::Group* Group = Topology.FindGroupByID( GroupID );
            assert( Group );

            for ( int32_t i = 0; i < static_cast<int32_t>( Group->Boundaries.size() ); ++i )
            {
                const GroupTopology::GroupBoundary&   Boundary       = Group->Boundaries[i];
                const int32_t                         GroupEdgeIndex = static_cast<int32_t>(
                     std::find( Boundary.GroupEdges.begin(), Boundary.GroupEdges.end(), GroupEdgeIDIn ) -
                     Boundary.GroupEdges.begin() );
                if ( GroupEdgeIndex != static_cast<int32_t>( Boundary.GroupEdges.size() ) )
                {
                    if ( static_cast<int32_t>( Boundary.GroupEdges.size() ) != 4 )
                    {
                        if ( OptionalOut.ProblemGroupEdgeIDsOut != nullptr )
                        {
                            OptionalOut.ProblemGroupEdgeIDsOut->insert( Boundary.GroupEdges.begin(),
                                                                        Boundary.GroupEdges.end() );
                        }
                        return false;
                    }

                    GroupEdgeIDOut   = Boundary.GroupEdges[( GroupEdgeIndex + 2 ) % 4];
                    BoundaryIndexOut = i;

                    // Get the corner attached to the one we were given
                    if ( CornerIDIn != InvalidID )
                    {
                        const GroupTopology::GroupEdge& SideEdge1 =
                             Topology.m_Edges[Boundary.GroupEdges[( GroupEdgeIndex + 1 ) % 4]];
                        const GroupTopology::GroupEdge& SideEdge2 =
                             Topology.m_Edges[Boundary.GroupEdges[( GroupEdgeIndex + 3 ) % 4]];
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

        void ConvertProportionsToArcLengths( const GroupTopology& Topology, int32_t GroupEdgeID,
                                             const std::vector<double>& ProportionsIn,
                                             std::vector<double>&       ArcLengthsOut,
                                             std::vector<double>*       PerVertexLengthsOut )
        {
            ArcLengthsOut.clear();
            const double TotalLength = Topology.GetEdgeArcLength( GroupEdgeID, PerVertexLengthsOut );
            for ( const double Proportion : ProportionsIn )
            {
                ArcLengthsOut.push_back( Proportion * TotalLength );
            }
        }

        /**
         * Inserts vertices along an existing group edge that will be used as endpoints for new group edges.
         * Due to tolerance, multiple inputs can map to the same vertex. Clears EndPointsOut before use.
         */
        bool InsertNewVertexEndpoints( const GroupEdgeInserter::EdgeLoopInsertionParams& Params,
                                       int32_t GroupEdgeID, int32_t StartCornerID,
                                       std::vector<SplitPoint>& EndPointsOut, OutParams& OptionalOut )
        {
            EndPointsOut.clear();
            if ( Params.SortedInputLengths->empty() )
            {
                return false;
            }

            const GroupTopology::GroupEdge& GroupEdge = Params.Topology->m_Edges[GroupEdgeID];

            // Our own copies, because we may need to iterate backwards relative to the order in the topology.
            const bool bGoBackward =
                 ( GroupEdge.Span.Vertices.back() == Params.Topology->GetCornerVertexID( StartCornerID ) );
            std::vector<int32_t> SpanVids;
            if ( !bGoBackward )
            {
                SpanVids = GroupEdge.Span.Vertices;
            }
            else
            {
                for ( int i = static_cast<int32_t>( GroupEdge.Span.Vertices.size() ) - 1; i >= 0; --i )
                {
                    SpanVids.push_back( GroupEdge.Span.Vertices[i] );
                }
            }

            std::vector<double> PerVertexLengths;
            std::vector<double> ArcLengths;
            if ( Params.bInputsAreProportions )
            {
                ConvertProportionsToArcLengths( *Params.Topology, GroupEdgeID, *Params.SortedInputLengths,
                                                ArcLengths, &PerVertexLengths );
            }
            else
            {
                ArcLengths = *Params.SortedInputLengths;
                Params.Topology->GetEdgeArcLength( GroupEdgeID, &PerVertexLengths );
            }

            const double TotalLength = PerVertexLengths.back();
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
            int32_t CurrentVid       = SpanVids[0];
            double CurrentArcLength = 0;
            int32_t NextIndex        = 1;

            for ( double TargetLength : ArcLengths )
            {
                // If the next target is beyond the last vertex, clamp it to the last vertex
                if ( TargetLength > TotalLength + Params.VertexTolerance )
                {
                    TargetLength = TotalLength;
                }

                // Advance until the next vertex would overshoot the target length.
                while ( NextIndex < static_cast<int32_t>( PerVertexLengths.size() ) &&
                        PerVertexLengths[NextIndex] <= TargetLength )
                {
                    CurrentVid       = SpanVids[NextIndex];
                    CurrentArcLength = PerVertexLengths[NextIndex];
                    ++NextIndex;
                }

                SplitPoint SplitPoint;

                auto SetSplitPointToVertex = [&SplitPoint, &SpanVids, &Params, NextIndex]( int32_t Vid )
                {
                    SplitPoint.ElementID = Vid;
                    SplitPoint.bIsVertex = true;

                    // Inserted verts must be on an edge and have the forward edge as their tangent.
                    const bool bVertexIsOriginal = ( Vid == SpanVids[NextIndex - 1] );
                    if ( !bVertexIsOriginal )
                    {
                        SplitPoint.Tangent = Normalized( Params.Mesh->GetVertex( SpanVids[NextIndex] ) -
                                                         Params.Mesh->GetVertex( Vid ) );
                    }
                    else
                    {
                        const glm::dvec3 VertexPosition = Params.Mesh->GetVertex( Vid );
                        SplitPoint.Tangent              = glm::dvec3( 0 );
                        if ( NextIndex > 1 )
                        {
                            SplitPoint.Tangent +=
                                 Normalized( VertexPosition - Params.Mesh->GetVertex( SpanVids[NextIndex - 2] ) );
                        }
                        if ( NextIndex < static_cast<int32_t>( SpanVids.size() ) )
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
                else if ( NextIndex < static_cast<int32_t>( PerVertexLengths.size() ) &&
                          PerVertexLengths[NextIndex] - CurrentArcLength <= Params.VertexTolerance )
                {
                    SetSplitPointToVertex( SpanVids[NextIndex] );
                }
                else
                {
                    // Target must be on the edge that goes to the next vertex
                    const int32_t CurrentEid = Params.Mesh->FindEdge( CurrentVid, SpanVids[NextIndex] );
                    if ( !Common::EnsureOrWarn( CurrentEid >= 0, "CurrentEid >= 0" ) )
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

                    DynamicMesh3::EdgeSplitInfo EdgeSplitInfo;
                    if ( Params.Mesh->SplitEdge( CurrentEid, EdgeSplitInfo, SplitT ) != MeshResult::Ok )
                    {
                        return false;
                    }

                    if ( OptionalOut.ChangedTidsOut != nullptr )
                    {
                        OptionalOut.ChangedTidsOut->insert( EdgeSplitInfo.OriginalTriangles.A );
                        if ( EdgeSplitInfo.OriginalTriangles.B != InvalidID )
                        {
                            OptionalOut.ChangedTidsOut->insert( EdgeSplitInfo.OriginalTriangles.B );
                        }
                    }

                    CurrentVid       = EdgeSplitInfo.NewVertex;
                    CurrentArcLength = TargetLength;

                    SplitPoint.ElementID = CurrentVid;
                    SplitPoint.bIsVertex = true;
                    SplitPoint.Tangent   = Normalized( Params.Mesh->GetVertex( SpanVids[NextIndex] ) -
                                                       Params.Mesh->GetVertex( CurrentVid ) );
                }

                EndPointsOut.push_back( SplitPoint );
            }
            return true;
        }

        /**
         * Creates a path of MeshSurfacePoint instances across a group, from a plane cut from start to end. Does
         * not embed it. Assumes the start and end points are on the boundary of the group.
         * @returns false if path could not be found.
         */
        bool GetPlaneCutPath( const DynamicMesh3& Mesh, int32_t GroupID, const SplitPoint& StartPoint,
                              const SplitPoint&                              EndPoint,
                              std::vector<std::pair<MeshSurfacePoint, int>>& OutputPath, double VertexCutTolerance,
                              const std::unordered_set<int32_t>& DisallowedVids )
        {
            // Guards against walking in a loop or backwards.
            std::unordered_set<int32_t> CrossedVids;
            std::unordered_set<int32_t> CrossedEids;

            const glm::dvec3 StartPosition =
                 StartPoint.bIsVertex ? Mesh.GetVertex( StartPoint.ElementID )
                                      : Mesh.GetEdgePoint( StartPoint.ElementID, StartPoint.EdgeTValue );
            const glm::dvec3 EndPosition = EndPoint.bIsVertex
                                                ? Mesh.GetVertex( EndPoint.ElementID )
                                                : Mesh.GetEdgePoint( EndPoint.ElementID, EndPoint.EdgeTValue );

            const glm::dvec3 InPlaneVector = Normalized( EndPosition - StartPosition );

            // Components of the two tangents that are orthogonal to the vector between the points.
            const glm::dvec3 NormalA =
                 Normalized( StartPoint.Tangent - glm::dot( StartPoint.Tangent, InPlaneVector ) * InPlaneVector,
                             KINDA_SMALL_NUMBER );
            glm::dvec3 NormalB =
                 Normalized( EndPoint.Tangent - glm::dot( EndPoint.Tangent, InPlaneVector ) * InPlaneVector,
                             KINDA_SMALL_NUMBER );

            if ( NormalA == glm::dvec3( 0 ) || NormalB == glm::dvec3( 0 ) )
            {
                // A tangent pointed directly toward the destination: the plane would be nonsense.
                return false;
            }

            // Same half space, so that the average represents the closer average of the corresponding lines.
            if ( glm::dot( NormalA, NormalB ) < 0 )
            {
                NormalB = -NormalB;
            }

            const glm::dvec3 CutPlaneNormal = Normalized( NormalA + NormalB );
            if ( !Common::EnsureOrWarn( CutPlaneNormal != glm::dvec3( 0 ), "CutPlaneNormal != glm::dvec3( 0 )" ) )
            {
                return false;
            }
            const glm::dvec3 CutPlaneOrigin = StartPosition;

            // Distances of the current edge's vertices from the plane.
            double CurrentEdgeVertPlaneDistances[2] = { 0, 0 };

            OutputPath.clear();
            if ( StartPoint.bIsVertex )
            {
                OutputPath.emplace_back( MeshSurfacePoint( StartPoint.ElementID ), InvalidID );
            }
            else
            {
                // The endpoints are not clamped here: clamping by plane distance depends on the plane's
                // orientation and could clamp to different endpoints as multiple paths go through one start/end
                // point.
                const Index2i EdgeVids = Mesh.GetEdgeV( StartPoint.ElementID );
                CurrentEdgeVertPlaneDistances[0] =
                     PointPlaneDist( Mesh.GetVertex( EdgeVids.A ), CutPlaneOrigin, CutPlaneNormal );
                CurrentEdgeVertPlaneDistances[1] =
                     PointPlaneDist( Mesh.GetVertex( EdgeVids.B ), CutPlaneOrigin, CutPlaneNormal );
                OutputPath.emplace_back(
                     MeshSurfacePoint::MakeEdgePoint( StartPoint.ElementID, StartPoint.EdgeTValue ), InvalidID );
            }

            bool    bCurrentPointIsVertex = ( OutputPath[0].first.PointType == SurfacePointType::Vertex );
            int32_t CurrentElementID      = OutputPath[0].first.ElementID;
            int32_t PointCount            = 1;
            // The triangle traversed to reach the current edge point, so the next step does not backtrack.
            int32_t TraversedTid = InvalidID;

            while ( CurrentElementID != EndPoint.ElementID || bCurrentPointIsVertex != EndPoint.bIsVertex )
            {
                if ( bCurrentPointIsVertex )
                {
                    if ( CrossedVids.contains( CurrentElementID ) )
                        return false;
                    CrossedVids.insert( CurrentElementID );
                }
                else
                {
                    if ( CrossedEids.contains( CurrentElementID ) )
                        return false;
                    CrossedEids.insert( CurrentElementID );
                }

                if ( !Common::EnsureOrWarn( PointCount < Mesh.EdgeCount(), "PointCount < Mesh.EdgeCount()" ) )
                {
                    return false;
                }

                if ( bCurrentPointIsVertex )
                {
                    MeshSurfacePoint  NextPoint( InvalidID );
                    const glm::dvec3  CurrentPosition = OutputPath.back().first.Pos( &Mesh );

                    // Find a surrounding triangle of our group that intersects the plane
                    int32_t CandidateTraversedTid = InvalidID;
                    for ( const int32_t Tid : Mesh.VtxTrianglesItr( CurrentElementID ) )
                    {
                        if ( Tid == TraversedTid || Mesh.GetTriangleGroup( Tid ) != GroupID )
                        {
                            continue;
                        }

                        // One of the triangle edges has the endpoint: go straight there.
                        if ( !EndPoint.bIsVertex )
                        {
                            const Index3i TriangleEids = Mesh.GetTriEdges( Tid );
                            for ( int32_t i = 0; i < 3; ++i )
                            {
                                if ( EndPoint.ElementID == TriangleEids[i] )
                                {
                                    OutputPath.emplace_back( MeshSurfacePoint::MakeEdgePoint(
                                                                  EndPoint.ElementID, EndPoint.EdgeTValue ),
                                                             InvalidID );
                                    return true;
                                }
                            }
                        }

                        const Index3i  TriangleVids = Mesh.GetTriangle( Tid );
                        const int32_t  VertA =
                             ( TriangleVids.A == CurrentElementID ) ? TriangleVids.C : TriangleVids.A;
                        const int32_t VertB =
                             ( TriangleVids.B == CurrentElementID ) ? TriangleVids.C : TriangleVids.B;
                        if ( EndPoint.bIsVertex && ( EndPoint.ElementID == VertA || EndPoint.ElementID == VertB ) )
                        {
                            OutputPath.emplace_back( MeshSurfacePoint( EndPoint.ElementID ), InvalidID );
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
                                                &NextPoint]( const MeshSurfacePoint& CandidateSurfacePoint )
                        {
                            if ( NextPoint.ElementID == CandidateSurfacePoint.ElementID &&
                                 NextPoint.PointType == CandidateSurfacePoint.PointType )
                            {
                                return false; // the same point, seen from an adjacent triangle
                            }
                            if ( NextPoint.ElementID == InvalidID ||
                                 ( glm::dot( InPlaneVector, NextPoint.Pos( &Mesh ) - CurrentPosition ) <
                                   glm::dot( InPlaneVector,
                                             CandidateSurfacePoint.Pos( &Mesh ) - CurrentPosition ) ) )
                            {
                                NextPoint = CandidateSurfacePoint;
                                return true;
                            }
                            return false;
                        };

                        bool bEdgeVertIsPreferred = false;
                        if ( bVertAIsOnPlane && !DisallowedVids.contains( VertA ) )
                        {
                            bEdgeVertIsPreferred = true;
                            UpdateNextPoint( MeshSurfacePoint( VertA ) );
                        }
                        if ( bVertBIsOnPlane && !DisallowedVids.contains( VertB ) )
                        {
                            bEdgeVertIsPreferred = true;
                            UpdateNextPoint( MeshSurfacePoint( VertB ) );
                        }
                        if ( !bEdgeVertIsPreferred && PlaneDistanceA * PlaneDistanceB < 0 )
                        {
                            // The triangle's opposite edge crosses the plane
                            const int32_t Eid        = Mesh.FindEdgeFromTri( VertA, VertB, Tid );
                            double      EdgeTValue = PlaneDistanceA / ( PlaneDistanceA - PlaneDistanceB );
                            if ( VertA != Mesh.GetEdgeV( Eid ).A )
                            {
                                EdgeTValue = 1 - EdgeTValue;
                            }

                            if ( UpdateNextPoint( MeshSurfacePoint::MakeEdgePoint( Eid, EdgeTValue ) ) )
                            {
                                CurrentEdgeVertPlaneDistances[0] = PlaneDistanceA;
                                CurrentEdgeVertPlaneDistances[1] = PlaneDistanceB;
                                if ( VertA != Mesh.GetEdgeV( Eid ).A )
                                {
                                    std::swap( CurrentEdgeVertPlaneDistances[0],
                                               CurrentEdgeVertPlaneDistances[1] );
                                }
                                CandidateTraversedTid = Tid;
                            }
                        }
                    }

                    if ( NextPoint.ElementID == InvalidID )
                    {
                        return false;
                    }
                    OutputPath.emplace_back( NextPoint, InvalidID );
                    TraversedTid =
                         ( NextPoint.PointType == SurfacePointType::Edge ) ? CandidateTraversedTid : InvalidID;
                }
                else
                {
                    const DynamicMesh3::Edge Edge = Mesh.GetEdge( CurrentElementID );

                    // We're starting from an edge. Get the triangle that we're dealing with.
                    int32_t NextTid = InvalidID;
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

                    const int32_t OppositeVert =
                         IndexUtil::FindTriOtherVtx( Edge.Vert.A, Edge.Vert.B, Mesh.GetTriangle( NextTid ) );
                    if ( EndPoint.bIsVertex && EndPoint.ElementID == OppositeVert )
                    {
                        OutputPath.emplace_back( MeshSurfacePoint( EndPoint.ElementID ), InvalidID );
                        return true;
                    }

                    if ( !EndPoint.bIsVertex )
                    {
                        const Index3i TriangleEids = Mesh.GetTriEdges( NextTid );
                        for ( int32_t i = 0; i < 3; ++i )
                        {
                            if ( EndPoint.ElementID == TriangleEids[i] )
                            {
                                OutputPath.emplace_back(
                                     MeshSurfacePoint::MakeEdgePoint( EndPoint.ElementID, EndPoint.EdgeTValue ),
                                     InvalidID );
                                return true;
                            }
                        }
                    }

                    const double OppositeVertPlaneDistance =
                         PointPlaneDist( Mesh.GetVertex( OppositeVert ), CutPlaneOrigin, CutPlaneNormal );
                    if ( std::abs( OppositeVertPlaneDistance ) <= VertexCutTolerance &&
                         !DisallowedVids.contains( OppositeVert ) )
                    {
                        OutputPath.emplace_back( MeshSurfacePoint( OppositeVert ),
                                                 InvalidID ); // cutting a vertex
                    }
                    else
                    {
                        // We are cutting through an edge. Figure out which one
                        int32_t SecondVertOfNextEdge = InvalidID;
                        double SecondPlaneDistance  = 0.0;
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

                        const int32_t Eid = Mesh.FindEdge( OppositeVert, SecondVertOfNextEdge );
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
                        OutputPath.emplace_back( MeshSurfacePoint::MakeEdgePoint( Eid, EdgeTValue ), InvalidID );
                    }
                }

                ++PointCount;
                assert( PointCount == static_cast<int32_t>( OutputPath.size() ) );
                CurrentElementID      = OutputPath.back().first.ElementID;
                bCurrentPointIsVertex = ( OutputPath.back().first.PointType == SurfacePointType::Vertex );
            }
            return true;
        }

        /**
         * Places a plane path connecting the endpoints into the mesh, but does not give the triangles new groups
         * yet. Outputs the path edge IDs so that can be done later.
         */
        bool EmbedPlaneCutPath( DynamicMesh3& Mesh, const GroupTopology& Topology, int32_t GroupID,
                                const SplitPoint& StartPoint, const SplitPoint& EndPoint, double VertexTolerance,
                                std::unordered_set<int32_t>& PathEidsOut,
                                std::unordered_set<int32_t>* ChangedTrisOut )
        {
            // No snapping to the group's boundary vertices by plane distance: it could join the boundary at a
            // different point from the one the loop continues from on the other side.
            std::unordered_set<int32_t>   DisallowedVids;
            const GroupTopology::Group*   Group = Topology.FindGroupByID( GroupID );
            if ( Common::EnsureOrWarn( Group, "Group" ) )
            {
                for ( const GroupTopology::GroupBoundary& Boundary : Group->Boundaries )
                {
                    for ( const int32_t GroupEdgeID : Boundary.GroupEdges )
                    {
                        if ( Common::EnsureOrWarn(
                                  GroupEdgeID < static_cast<int32_t>( Topology.m_Edges.size() ),
                                  "GroupEdgeID < static_cast<int32_t>( Topology.Edges.size() )" ) )
                        {
                            DisallowedVids.insert( Topology.m_Edges[GroupEdgeID].Span.Vertices.begin(),
                                                   Topology.m_Edges[GroupEdgeID].Span.Vertices.end() );
                        }
                    }
                }
            }

            std::vector<std::pair<MeshSurfacePoint, int>> CutPath;
            if ( !GetPlaneCutPath( Mesh, GroupID, StartPoint, EndPoint, CutPath, VertexTolerance,
                                   DisallowedVids ) )
            {
                return false;
            }
            assert( static_cast<int32_t>( CutPath.size() ) >= 2 );

            if ( ChangedTrisOut != nullptr )
            {
                for ( int32_t i = 0; i < static_cast<int32_t>( CutPath.size() ); ++i )
                {
                    const MeshSurfacePoint& Point = CutPath[i].first;
                    if ( Point.PointType == SurfacePointType::Edge )
                    {
                        const Index2i EdgeTris = Mesh.GetEdgeT( Point.ElementID );
                        ChangedTrisOut->insert( EdgeTris.A );
                        if ( EdgeTris.B != InvalidID )
                        {
                            ChangedTrisOut->insert( EdgeTris.B );
                        }
                    }
                }
            }

            MeshSurfacePath PathEmbedder( &Mesh );
            PathEmbedder.m_Path = CutPath;
            std::vector<int32_t> PathVertices;
            if ( !PathEmbedder.EmbedSimplePath( PathVertices, false ) )
            {
                return false;
            }
            assert( static_cast<int32_t>( PathVertices.size() ) >= 2 );

            for ( int32_t i = 1; i < static_cast<int32_t>( PathVertices.size() ); ++i )
            {
                const int32_t Eid = Mesh.FindEdge( PathVertices[i - 1], PathVertices[i] );
                if ( Common::EnsureOrWarn( Eid >= 0, "Eid >= 0" ) )
                {
                    PathEidsOut.insert( Eid );
                }
            }
            return true;
        }

        /** Uses the given path edge IDs to split a group into new groups. */
        bool CreateNewGroups( DynamicMesh3& Mesh, const std::unordered_set<int32_t>& PathEids,
                              int32_t OriginalGroup, int32_t& NumGroupsCreated, OutParams& OptionalOut )
        {
            std::unordered_set<int32_t> SeedTriangleSet;
            for ( const int32_t Eid : PathEids )
            {
                const Index2i Tris = Mesh.GetEdgeT( Eid );
                if ( Mesh.GetTriangleGroup( Tris.A ) == OriginalGroup )
                {
                    SeedTriangleSet.insert( Tris.A );
                }
                if ( Tris.B != InvalidID && Mesh.GetTriangleGroup( Tris.B ) == OriginalGroup )
                {
                    SeedTriangleSet.insert( Tris.B );
                }
            }
            std::vector<int> Seeds = std::vector( SeedTriangleSet.begin(), SeedTriangleSet.end() );
            std::sort( Seeds.begin(), Seeds.end() );

            MeshConnectedComponents ConnectedComponents( &Mesh );
            ConnectedComponents.FindTrianglesConnectedToSeeds(
                 Seeds,
                 [&]( int32_t t0, int32_t t1 )
                 {
                     // Connected only if same group and not across
                     // one of the newly inserted group edges.
                     if ( Mesh.GetTriangleGroup( t0 ) == Mesh.GetTriangleGroup( t1 ) )
                     {
                         return !PathEids.contains( Mesh.FindEdgeFromTriPair( t0, t1 ) );
                     }
                     return false;
                 } );

            // Assign a new group id for each component. The first component keeps the old group ID.
            for ( int32_t i = 1; i < ConnectedComponents.Num(); ++i )
            {
                const MeshConnectedComponents::Component& Component = ConnectedComponents.GetComponent( i );
                if ( OptionalOut.ChangedTidsOut != nullptr )
                {
                    OptionalOut.ChangedTidsOut->insert( Component.Indices.begin(), Component.Indices.end() );
                }
                const int32_t NewGroupID = Mesh.AllocateTriangleGroup();
                for ( const int Tid : Component.Indices )
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
        bool ConnectEndpoints( const GroupEdgeInserter::EdgeLoopInsertionParams& Params, int32_t GroupID,
                               const std::vector<SplitPoint>& StartPoints,
                               const std::vector<SplitPoint>& EndPoints, int32_t& NumGroupsCreated,
                               OutParams& OptionalOut )
        {
            NumGroupsCreated             = 0;
            const int32_t NumEdgesToInsert =
                 std::min( static_cast<int32_t>( StartPoints.size() ), static_cast<int32_t>( EndPoints.size() ) );
            std::unordered_set<int32_t> PathsEids;
            for ( int32_t i = 0; i < NumEdgesToInsert; ++i )
            {
                if ( !EmbedPlaneCutPath( *Params.Mesh, *Params.Topology, GroupID, StartPoints[i], EndPoints[i],
                                         Params.VertexTolerance, PathsEids, OptionalOut.ChangedTidsOut ) )
                {
                    return false;
                }
            }

            if ( OptionalOut.NewEidsOut != nullptr )
            {
                OptionalOut.NewEidsOut->insert( PathsEids.begin(), PathsEids.end() );
            }

            return CreateNewGroups( *Params.Mesh, PathsEids, GroupID, NumGroupsCreated, OptionalOut );
        }

        /** Continues the loop in one direction from a start edge. @returns false if there is an error. */
        bool InsertEdgeLoopEdgesInDirection( const GroupEdgeInserter::EdgeLoopInsertionParams& Params,
                                             const std::vector<SplitPoint>& StartEndpoints, int32_t NextGroupID,
                                             int32_t NextEdgeID, int32_t NextCornerID, int32_t NextBoundaryIndex,
                                             std::unordered_set<int32_t>& AlteredGroups, int32_t& NumInserted,
                                             OutParams& OptionalOut )
        {
            NumInserted = 0;
            if ( AlteredGroups.contains( NextGroupID ) || StartEndpoints.empty() )
            {
                return true;
            }

            std::vector<SplitPoint>  EndpointStorage1 = StartEndpoints;
            std::vector<SplitPoint>  EndpointStorage2;
            std::vector<SplitPoint>* CurrentEndpoints = &EndpointStorage1;
            std::vector<SplitPoint>* NextEndpoints    = &EndpointStorage2;

            bool bHaveNextGroup = true;
            bool bSuccess       = true;
            while ( bHaveNextGroup && !AlteredGroups.contains( NextGroupID ) )
            {
                // See if we looped around to the start
                if ( NextEdgeID == Params.GroupEdgeID )
                {
                    int32_t NumGroupsCreated = 0;
                    bSuccess = ConnectEndpoints( Params, NextGroupID, *CurrentEndpoints, StartEndpoints,
                                                 NumGroupsCreated, OptionalOut );
                    AlteredGroups.insert( NextGroupID );
                    NumInserted += ( NumGroupsCreated > 1 ? 1 : 0 );
                    break;
                }

                // Otherwise, create next endpoints
                if ( !InsertNewVertexEndpoints( Params, NextEdgeID, NextCornerID, *NextEndpoints, OptionalOut ) )
                {
                    return false;
                }
                if ( NextEndpoints->empty() )
                {
                    return true; // the next edge was not long enough for the input lengths
                }

                int32_t NumGroupsCreated = 0;
                bSuccess               = ConnectEndpoints( Params, NextGroupID, *CurrentEndpoints, *NextEndpoints,
                                                           NumGroupsCreated, OptionalOut );
                AlteredGroups.insert( NextGroupID );
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
                NextGroupID = Params.Topology->m_Edges[NextEdgeID].OtherGroupID( NextGroupID );
                bHaveNextGroup =
                     GetEdgeLoopOpposingEdgeAndCorner( *Params.Topology, NextGroupID, NextEdgeID, NextCornerID,
                                                       NextEdgeID, NextCornerID, NextBoundaryIndex, OptionalOut );
                std::swap( CurrentEndpoints, NextEndpoints );
            }
            return bSuccess;
        }
    } // namespace GroupEdgeInserterLocals

    bool GroupEdgeInserter::InsertEdgeLoops( const EdgeLoopInsertionParams& Params,
                                             OptionalOutputParams           OptionalOut )
    {
        using namespace GroupEdgeInserterLocals;

        assert( Params.Mesh );
        assert( Params.Topology );
        assert( Params.SortedInputLengths );
        assert( Params.GroupEdgeID != InvalidID );
        assert( Params.StartCornerID != InvalidID );

        const GroupTopology::GroupEdge& GroupEdge = Params.Topology->m_Edges[Params.GroupEdgeID];

        // Check for a valid path forward or backward first: no edge splits if we have neither.
        const int32_t ForwardGroupID       = GroupEdge.Groups.A;
        int32_t       ForwardEdgeID        = InvalidID;
        int32_t       ForwardCornerID      = InvalidID;
        int32_t       ForwardBoundaryIndex = InvalidID;
        const bool  bHaveForwardEdge     = GetEdgeLoopOpposingEdgeAndCorner(
             *Params.Topology, ForwardGroupID, Params.GroupEdgeID, Params.StartCornerID, ForwardEdgeID,
             ForwardCornerID, ForwardBoundaryIndex, OptionalOut );

        const int32_t BackwardGroupID       = GroupEdge.Groups.B;
        int32_t       BackwardEdgeID        = InvalidID;
        int32_t       BackwardCornerID      = InvalidID;
        int32_t       BackwardBoundaryIndex = InvalidID;
        const bool  bHaveBackwardEdge     = GetEdgeLoopOpposingEdgeAndCorner(
             *Params.Topology, BackwardGroupID, Params.GroupEdgeID, Params.StartCornerID, BackwardEdgeID,
             BackwardCornerID, BackwardBoundaryIndex, OptionalOut );

        if ( !bHaveForwardEdge && !bHaveBackwardEdge )
        {
            return false; // neither neighbour is quad-like
        }

        // The loop ends when it arrives at a group it already altered (it may try to cross itself from the side);
        // this also means the topology need not be updated as we go.
        std::unordered_set<int32_t> AlteredGroups;

        // The first endpoints are kept in case they close the loop. Splitting ahead of time keeps a split from
        // changing the eid of the next endpoint.
        std::vector<GroupEdgeSplitPoint> StartEndpoints;
        bool bSuccess = InsertNewVertexEndpoints( Params, Params.GroupEdgeID, Params.StartCornerID, StartEndpoints,
                                                  OptionalOut );
        if ( !bSuccess || StartEndpoints.empty() )
        {
            return false;
        }

        // Both directions. For a closed loop the second call does nothing: AlteredGroups has its groups.
        int32_t TotalNumInserted = 0;
        if ( bHaveForwardEdge )
        {
            bSuccess = InsertEdgeLoopEdgesInDirection( Params, StartEndpoints, ForwardGroupID, ForwardEdgeID,
                                                       ForwardCornerID, ForwardBoundaryIndex, AlteredGroups,
                                                       TotalNumInserted, OptionalOut );
        }
        if ( bSuccess && bHaveBackwardEdge )
        {
            int32_t NumInserted = 0;
            bSuccess = InsertEdgeLoopEdgesInDirection( Params, StartEndpoints, BackwardGroupID, BackwardEdgeID,
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

    bool GroupEdgeInserter::InsertGroupEdge( GroupEdgeInsertionParams& Params, OptionalOutputParams OptionalOut )
    {
        using namespace GroupEdgeInserterLocals;

        assert( Params.Mesh );
        assert( Params.Topology );
        assert( Params.GroupID != InvalidID );
        assert( Params.StartPoint.ElementID != InvalidID );
        assert( Params.EndPoint.ElementID != InvalidID );

        if ( Params.StartPoint.bIsVertex == Params.EndPoint.bIsVertex &&
             Params.StartPoint.ElementID == Params.EndPoint.ElementID )
        {
            return false; // points are on the same vertex or edge
        }

        std::unordered_set<int32_t>  TempNewEids;
        std::unordered_set<int32_t>* NewEids =
             OptionalOut.NewEidsOut != nullptr ? OptionalOut.NewEidsOut : &TempNewEids;

        if ( !EmbedPlaneCutPath( *Params.Mesh, *Params.Topology, Params.GroupID, Params.StartPoint,
                                 Params.EndPoint, Params.VertexTolerance, *NewEids, OptionalOut.ChangedTidsOut ) )
        {
            return false;
        }

        int32_t NumGroupsCreated = 0;
        if ( !CreateNewGroups( *Params.Mesh, *NewEids, Params.GroupID, NumGroupsCreated, OptionalOut ) )
        {
            return false;
        }

        Params.Topology->RebuildTopology();
        return true;
    }
} // namespace Desert::Geometry
