// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Operations/EmbedSurfacePath.cpp:13-652,682-947,
// adapted: UE Core as std/glm, namespace Desert::Geometry, TStaticArray is std::array, TSet is std::unordered_set
// (membership only, so iteration order never matters), TMap is std::unordered_map, the TArray heap is a
// std::priority_queue ordered smallest-first like UE's HeapPop. Left out: the bUpdatePath tail (941-944), which UE
// never implemented. Where UE carries on after an ensure or ignores an MeshResult, this returns false instead: a
// split or poke that did not happen leaves no new vertex to put on the path, and an end point whose edge was split
// away can no longer be placed.
#include "Engine/Geometry/MeshCore/Operations/EmbedSurfacePath.hpp"

#include "Engine/Geometry/MeshCore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/SegmentTypes.hpp"
#include "Engine/Geometry/MeshCore/TriangleTypes.hpp"
#include "Engine/Geometry/MeshCore/VectorUtil.hpp"

#include <array>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <Common/Core/Core.hpp>

namespace Desert::Geometry
{
    glm::dvec3 MeshSurfacePoint::Pos( const DynamicMesh3* Mesh ) const
    {
        if ( PointType == SurfacePointType::Vertex )
        {
            return Mesh->GetVertex( ElementID );
        }
        if ( PointType == SurfacePointType::Edge )
        {
            glm::dvec3 EA{};
            glm::dvec3 EB{};
            Mesh->GetEdgeV( ElementID, EA, EB );
            // Note this is equivalent to Lerp(EA, EB, BaryCoord[1])
            return BaryCoord[0] * EA + BaryCoord[1] * EB;
        }
        // PointType == SurfacePointType::Triangle
        glm::dvec3 TA{};
        glm::dvec3 TB{};
        glm::dvec3 TC{};
        Mesh->GetTriVertices( ElementID, TA, TB, TC );
        return BaryCoord[0] * TA + BaryCoord[1] * TB + BaryCoord[2] * TC;
    }

    namespace
    {
        /**
         * Helper function to snap a triangle surface point to the triangle vertices or edges if it's close enough.
         * Input SurfacePt must be a triangle.
         */
        void RefineSurfacePtFromTriangleToSubElement( const DynamicMesh3* Mesh, const glm::dvec3& Pos,
                                                      MeshSurfacePoint& SurfacePt, double SnapElementThresholdSq )
        {
            // expect this to only be called on SurfacePoints with PointType == Triangle; otherwise indicative of
            // incorrect usage
            if ( !Common::EnsureOrWarn( SurfacePt.PointType == SurfacePointType::Triangle,
                                        "SurfacePt.PointType == SurfacePointType::Triangle" ) )
            {
                return;
            }
            const int     TriID             = SurfacePt.ElementID;
            const Index3i TriVertIDs        = Mesh->GetTriangle( TriID );
            int           BestSubIdx        = -1;
            double        BestElementDistSq = 0;
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
                SurfacePt.PointType = SurfacePointType::Vertex;
                return;
            }

            // failed to snap to vertex, try snapping to edge
            const Index3i TriEdgeIDs    = Mesh->GetTriEdges( TriID );
            double        BestEdgeParam = 0;
            for ( int EdgeSubIdx = 0; EdgeSubIdx < 3; EdgeSubIdx++ )
            {
                const int  EdgeID = TriEdgeIDs[EdgeSubIdx];
                glm::dvec3 EPosA{};
                glm::dvec3 EPosB{};
                Mesh->GetEdgeV( EdgeID, EPosA, EPosB );
                const Segment3d EdgeSeg( EPosA, EPosB );
                const double    DistSq = EdgeSeg.DistanceSquared( Pos );
                if ( DistSq <= SnapElementThresholdSq && ( BestSubIdx == -1 || DistSq < BestElementDistSq ) )
                {
                    BestSubIdx        = EdgeSubIdx;
                    BestElementDistSq = DistSq;
                    BestEdgeParam     = EdgeSeg.ProjectUnitRange( Pos );
                }
            }
            if ( BestSubIdx > -1 )
            {
                SurfacePt = MeshSurfacePoint::MakeEdgePoint( TriEdgeIDs[BestSubIdx], BestEdgeParam );
                return;
            }

            // no snapping to be done, leave surfacept on the triangle
        }

        // For when a triangle is replaced by multiple triangles, create a new surface point for the point's new
        // location among the smaller triangles.
        template <typename IterableTrisType>
        MeshSurfacePoint RelocateTrianglePointAfterRefinement( const DynamicMesh3* Mesh, const glm::dvec3& Pos,
                                                               const IterableTrisType& TriIDs,
                                                               double                  SnapElementThresholdSq )
        {
            double     BestTriDistSq = 0;
            glm::dvec3 BestBaryCoords{};
            int        BestTriID = -1;
            for ( const int TriID : TriIDs )
            {
                assert( Mesh->IsTriangle( TriID ) );
                const Index3i    TriVertIDs = Mesh->GetTriangle( TriID );
                const Triangle3d Tri( Mesh->GetVertex( TriVertIDs.A ), Mesh->GetVertex( TriVertIDs.B ),
                                      Mesh->GetVertex( TriVertIDs.C ) );
                // heavy duty way to get barycentric coordinates and check if on triangle; should be robust to
                // degenerate triangles unlike VectorUtil's barycentric coordinate function
                DistPoint3Triangle3d TriDist( Pos, Tri );
                const double         DistSq = TriDist.GetSquared();
                if ( BestTriID == -1 || DistSq < BestTriDistSq )
                {
                    BestTriID      = TriID;
                    BestTriDistSq  = DistSq;
                    BestBaryCoords = TriDist.m_TriangleBaryCoords;
                }
            }
            DESERT_VERIFY_WARN( Mesh->IsTriangle( BestTriID ) );
            MeshSurfacePoint SurfacePt( BestTriID, BestBaryCoords );
            RefineSurfacePtFromTriangleToSubElement( Mesh, Pos, SurfacePt, SnapElementThresholdSq );
            return SurfacePt;
        }

        // Helper to snap a point to the nearest point in a fixed-size array
        template <size_t NumPoints>
        bool SelectSnapPoint( const std::array<glm::dvec3, NumPoints>& PointArray, const glm::dvec3& Point,
                              int32_t& OutIdx, double SnapThresholdSq )
        {
            double BestDistSq = SnapThresholdSq;
            OutIdx            = -1;
            for ( int32_t Idx = 0; Idx < static_cast<int32_t>( NumPoints ); ++Idx )
            {
                const double DSq = DistanceSquared( Point, PointArray[Idx] );
                if ( DSq < BestDistSq )
                {
                    OutIdx     = Idx;
                    BestDistSq = DSq;
                }
            }
            return OutIdx != -1;
        }

        struct IndexDistance
        {
            int    Index         = -1;
            double PathLength    = 0;
            double DistanceToEnd = 0;
        };

        // std::priority_queue pops its LARGEST element under the comparator; ordering by "greater" puts the lowest
        // potential path length on top, which is what UE's HeapPop over FIndexDistance::operator< yields.
        struct IndexDistanceGreater
        {
            bool operator()( const IndexDistance& A, const IndexDistance& B ) const
            {
                return A.PathLength + A.DistanceToEnd > B.PathLength + B.DistanceToEnd;
            }
        };

        using VertexToPosnFunc = std::function<glm::dvec3( const DynamicMesh3*, int )>;

        // track where you came from and where you are going
        struct WalkIndices
        {
            // Position in the coordinate space used for the walk (note: may not be the same space as the mesh
            // vertices, e.g. if we walk on UV positions)
            glm::dvec3 Position{};
            int        WalkedFromPt = -1; // index into ComputedPointsAndSources (or -1)
            int        WalkingOnTri = -1; // ID of triangle in mesh (or -1)
        };

        bool WalkMeshPlanar( const DynamicMesh3* Mesh, int StartTri, int StartVID, const glm::dvec3& StartPt,
                             int EndTri, int EndVertID, const glm::dvec3& EndPt, const glm::dvec3& WalkPlaneNormal,
                             const VertexToPosnFunc& VertexToPosnFn, bool bAllowBackwardsSearch,
                             double AcceptEndPtOutsideDist, double PtOnPlaneThresholdSq,
                             std::vector<std::pair<MeshSurfacePoint, int>>& WalkedPath, double BackwardsTolerance )
        {
            // Even when bAllowBackwardsSearch is false, there may be multiple paths from start point to end point.
            // The approach we take is a breadth-first-like one where we keep track of multiple paths and always
            // extend the one that has the potential to be shortest, i.e. whose current path length + distance to
            // destination is smallest. (we can't go by distance to destination alone because a sub optimal path
            // can curve around the destination in the plane in such a way that it always seems closer than the
            // next step in a more direct path that happens to pass through a more tesselated region).

            auto SetTriVertPositions = [&VertexToPosnFn, Mesh]( const Index3i& TriVertIDs, Triangle3d& Tri )
            {
                Tri.V[0] = VertexToPosnFn( Mesh, TriVertIDs.A );
                Tri.V[1] = VertexToPosnFn( Mesh, TriVertIDs.B );
                Tri.V[2] = VertexToPosnFn( Mesh, TriVertIDs.C );
            };

            std::vector<std::pair<MeshSurfacePoint, WalkIndices>> ComputedPointsAndSources;

            // Lowest potential path length (path length so far + distance to end point) on top, so that we can pop
            // off of it to grow the best candidate path. The payloads are indices into ComputedPointsAndSources.
            std::priority_queue<IndexDistance, std::vector<IndexDistance>, IndexDistanceGreater> UnexploredEnds;

            std::unordered_set<int> ExploredTriangles;
            std::unordered_set<int> CrossedVertices;
            std::unordered_set<int> CrossedEdges;

            // When we've found a path, the index into ComputedPointsAndSources of the best path end.
            int BestKnownEnd = -1;

            Triangle3d    CurrentTri;
            const Index3i StartTriVertIDs = Mesh->GetTriangle( StartTri );
            SetTriVertPositions( StartTriVertIDs, CurrentTri );
            // heavy duty way to get barycentric coordinates and check if on triangle; should be robust to
            // degenerate triangles unlike VectorUtil's barycentric coordinate function
            DistPoint3Triangle3d CurrentTriDist( StartPt, CurrentTri );
            int                  StartVIDIndex = -1;
            if ( StartVID != -1 )
            {
                StartVIDIndex = StartTriVertIDs.IndexOf( StartVID );
            }
            if ( StartVIDIndex == -1 )
            {
                CurrentTriDist.ComputeResult();
            }
            else
            {
                // if a valid StartVID was given, assume that's our closest point
                CurrentTriDist.m_TriangleBaryCoords                = glm::dvec3( 0 );
                CurrentTriDist.m_TriangleBaryCoords[StartVIDIndex] = 1.0;
                CurrentTriDist.m_ClosestTrianglePoint              = StartPt;
            }
            ComputedPointsAndSources.emplace_back(
                 MeshSurfacePoint( StartTri, CurrentTriDist.m_TriangleBaryCoords ),
                 WalkIndices{ StartPt, -1, StartTri } );

            const glm::dvec3 ForwardsDirection = EndPt - StartPt;
            // not allowed to go in a direction that gets us further from the destination than our initial point if
            // backwards search not allowed
            auto IsForward = [&]( const glm::dvec3& P )
            { return glm::dot( ForwardsDirection, P - StartPt ) >= -BackwardsTolerance; };

            // Our start point is our first unexplored end
            UnexploredEnds.push( { 0, 0.0, glm::length( ForwardsDirection ) } );

            int       IterCountSafety = 0;
            const int NumTriangles    = Mesh->TriangleCount();
            while ( true )
            {
                // safety check to protect against infinite loop
                if ( !Common::EnsureOrWarn( IterCountSafety++ < NumTriangles * 2,
                                            "WalkMeshPlanar: more iterations than twice the triangle count" ) )
                {
                    return false;
                }

                // Grab the best potential path, if there is still a viable one.
                if ( UnexploredEnds.empty() )
                {
                    return false; // failed to find path
                }
                const IndexDistance TopEndWithDistance = UnexploredEnds.top();
                UnexploredEnds.pop();
                const int    CurrentEnd        = TopEndWithDistance.Index;
                const double CurrentPathLength = TopEndWithDistance.PathLength;

                const MeshSurfacePoint FromPt = ComputedPointsAndSources[CurrentEnd].first;
                const int              TriID  = ComputedPointsAndSources[CurrentEnd].second.WalkingOnTri;
                if ( !Common::EnsureOrWarn( Mesh->IsTriangle( TriID ),
                                            "WalkMeshPlanar: walking on a non-triangle" ) )
                {
                    return false;
                }
                const Index3i TriVertIDs = Mesh->GetTriangle( TriID );
                SetTriVertPositions( TriVertIDs, CurrentTri );

                // Note about ending the search: the final step of our path is a direct line to the destination, so
                // the final path length will be CurrentPathLength + DistanceToDestination at that point. Since
                // we've been grabbing the minimal path (for extending) by the same criteria, we know that we don't
                // need to try extending any other paths at that point (we have the shortest). This breaks down if
                // AcceptEndPtOutsideDist is large enough to have multiple candidate end points.

                // if we're on a triangle that is connected to the known final vertex, end the search!
                if ( EndVertID >= 0 && TriVertIDs.Contains( EndVertID ) )
                {
                    ComputedPointsAndSources.emplace_back( MeshSurfacePoint( EndVertID ),
                                                           WalkIndices{ EndPt, CurrentEnd, TriID } );
                    BestKnownEnd = static_cast<int>( ComputedPointsAndSources.size() ) - 1;
                    break;
                }

                bool OnEndTri           = EndTri == TriID;
                bool ComputedEndPtOnTri = false;
                if ( EndVertID < 0 && EndTri == -1 ) // if we need to check if this is the end tri, and it could be
                {
                    CurrentTriDist.m_Triangle = CurrentTri;
                    CurrentTriDist.m_Point    = EndPt;
                    ComputedEndPtOnTri        = true;
                    if ( CurrentTriDist.GetSquared() < AcceptEndPtOutsideDist )
                    {
                        OnEndTri = true;
                    }
                }

                // if we're on the final triangle, end the search!
                if ( OnEndTri )
                {
                    if ( !ComputedEndPtOnTri )
                    {
                        CurrentTriDist.m_Triangle = CurrentTri;
                        CurrentTriDist.m_Point    = EndPt;
                        CurrentTriDist.GetSquared();
                    }
                    ComputedPointsAndSources.emplace_back(
                         MeshSurfacePoint( TriID, CurrentTriDist.m_TriangleBaryCoords ),
                         WalkIndices{ EndPt, CurrentEnd, TriID } );
                    BestKnownEnd = static_cast<int>( ComputedPointsAndSources.size() ) - 1;
                    break;
                }

                // explored triangles are only in the search to handle going 'the long way' around and back to the
                // start triangle, which the `if ( OnEndTri )` above already handled, so this branch can die here
                if ( ExploredTriangles.contains( TriID ) )
                {
                    continue;
                }
                ExploredTriangles.insert( TriID );

                // not on a terminal triangle, cross the triangle and continue the search
                std::array<double, 3> SignDist{};
                std::array<int, 3>    Side{};
                const auto            InitialComputedPointsNum = ComputedPointsAndSources.size();
                for ( int TriSubIdx = 0; TriSubIdx < 3; TriSubIdx++ )
                {
                    const double SD     = glm::dot( CurrentTri.V[TriSubIdx] - StartPt, WalkPlaneNormal );
                    SignDist[TriSubIdx] = SD;
                    if ( std::abs( SD ) > PtOnPlaneThresholdSq )
                    {
                        Side[TriSubIdx] = SD > 0 ? 1 : -1;
                        continue;
                    }
                    // Vertex crossing
                    Side[TriSubIdx]           = 0;
                    const int CandidateVertID = TriVertIDs[TriSubIdx];
                    if ( FromPt.PointType == SurfacePointType::Vertex && CandidateVertID == FromPt.ElementID )
                    {
                        continue;
                    }
                    if ( ( !bAllowBackwardsSearch && !IsForward( CurrentTri.V[TriSubIdx] ) ) ||
                         CrossedVertices.contains( CandidateVertID ) )
                    {
                        continue;
                    }
                    // consider going over this vertex
                    CrossedVertices.insert( CandidateVertID );

                    // walking over a vertex means searching the whole one ring for candidate next triangles, and
                    // there might be multiple of them
                    for ( const int NbrTriID : Mesh->VtxTrianglesItr( CandidateVertID ) )
                    {
                        if ( NbrTriID == TriID )
                        {
                            continue;
                        }
                        const Index3i NbrTriVertIDs = Mesh->GetTriangle( NbrTriID );
                        Triangle3d    NbrTri;
                        SetTriVertPositions( NbrTriVertIDs, NbrTri );
                        int SignsMultiplied = 1;
                        for ( int NbrTriSubIdx = 0; NbrTriSubIdx < 3; NbrTriSubIdx++ )
                        {
                            if ( NbrTriVertIDs[NbrTriSubIdx] == CandidateVertID )
                            {
                                continue;
                            }
                            const double NbrSD = glm::dot( NbrTri.V[NbrTriSubIdx] - StartPt, WalkPlaneNormal );
                            const int    NbrSign =
                                 std::abs( NbrSD ) <= PtOnPlaneThresholdSq ? 0 : ( NbrSD > 0 ? 1 : -1 );
                            SignsMultiplied *= NbrSign;
                        }
                        if ( SignsMultiplied < 1 ) // plane will cross this triangle, so try walking it
                        {
                            ComputedPointsAndSources.emplace_back(
                                 MeshSurfacePoint( CandidateVertID ),
                                 WalkIndices{ CurrentTri.V[TriSubIdx], CurrentEnd, NbrTriID } );
                        }
                    }
                }
                const Index3i TriEdgeIDs = Mesh->GetTriEdges( TriID );
                for ( int TriSubIdx = 0; TriSubIdx < 3; TriSubIdx++ )
                {
                    const int NextSubIdx = ( TriSubIdx + 1 ) % 3;
                    if ( Side[TriSubIdx] * Side[NextSubIdx] >= 0 )
                    {
                        continue;
                    }
                    // edge crossing
                    const int CandidateEdgeID = TriEdgeIDs[TriSubIdx];
                    if ( FromPt.PointType == SurfacePointType::Edge && CandidateEdgeID == FromPt.ElementID )
                    {
                        continue;
                    }
                    double CrossingT = SignDist[TriSubIdx] / ( SignDist[TriSubIdx] - SignDist[NextSubIdx] );
                    const glm::dvec3 CrossingP =
                         ( 1 - CrossingT ) * CurrentTri.V[TriSubIdx] + CrossingT * CurrentTri.V[NextSubIdx];
                    const DynamicMesh3::Edge Edge = Mesh->GetEdge( CandidateEdgeID );
                    // edge verts are stored backwards from the order in the local triangle, reverse the crossing
                    if ( Edge.Vert.A != TriVertIDs[TriSubIdx] )
                    {
                        CrossingT = 1 - CrossingT;
                    }
                    const int CrossToTriID = Edge.Tri.A == TriID ? Edge.Tri.B : Edge.Tri.A;
                    if ( CrossToTriID == -1 )
                    {
                        // We've walked off the border of the mesh
                        continue;
                    }
                    if ( ( bAllowBackwardsSearch || IsForward( CrossingP ) ) &&
                         !CrossedEdges.contains( CandidateEdgeID ) )
                    {
                        CrossedEdges.insert( CandidateEdgeID );
                        ComputedPointsAndSources.emplace_back(
                             MeshSurfacePoint::MakeEdgePoint( CandidateEdgeID, CrossingT ),
                             WalkIndices{ CrossingP, CurrentEnd, CrossToTriID } );
                    }
                }

                const glm::dvec3 PreviousPathPoint = ComputedPointsAndSources[CurrentEnd].second.Position;
                for ( auto NewComputedPtIdx = InitialComputedPointsNum;
                      NewComputedPtIdx < ComputedPointsAndSources.size(); NewComputedPtIdx++ )
                {
                    const glm::dvec3& CurrentPathPoint =
                         ComputedPointsAndSources[NewComputedPtIdx].second.Position;
                    const double PathLength = CurrentPathLength + Distance( PreviousPathPoint, CurrentPathPoint );
                    const double DistanceToEnd = Distance( EndPt, CurrentPathPoint );

                    // the "forward" rule was applied while grabbing points; checked again here just in case
                    if ( Common::EnsureOrWarn( bAllowBackwardsSearch || IsForward( CurrentPathPoint ),
                                               "WalkMeshPlanar: a backwards point reached the search" ) )
                    {
                        UnexploredEnds.push( { static_cast<int>( NewComputedPtIdx ), PathLength, DistanceToEnd } );
                    }
                }
            }

            int              TrackedPtIdx       = BestKnownEnd;
            int              SafetyIdxBacktrack = 0;
            std::vector<int> AcceptedIndices;
            while ( TrackedPtIdx > -1 )
            {
                // infinite loop guard
                if ( !Common::EnsureOrWarn( SafetyIdxBacktrack++ <
                                                 2 * static_cast<int>( ComputedPointsAndSources.size() ),
                                            "WalkMeshPlanar: backtrack longer than the computed points" ) )
                {
                    return false;
                }
                AcceptedIndices.push_back( TrackedPtIdx );
                TrackedPtIdx = ComputedPointsAndSources[TrackedPtIdx].second.WalkedFromPt;
            }
            WalkedPath.clear();
            for ( auto It = AcceptedIndices.rbegin(); It != AcceptedIndices.rend(); ++It )
            {
                WalkedPath.emplace_back( ComputedPointsAndSources[*It].first,
                                         ComputedPointsAndSources[*It].second.WalkingOnTri );
            }

            // try refining start and end points if they were on triangles, and remove them if they turn out to be
            // duplicates after refinement (refining up front would complicate the traversal logic). The edge case
            // does not compare barycoords: the path can only cross the edge at one point, and two points on the
            // same edge would break the simple embedding code.
            if ( !WalkedPath.empty() && WalkedPath[0].first.PointType == SurfacePointType::Triangle )
            {
                MeshSurfacePoint& SurfacePt = WalkedPath[0].first;

                // if we started on an exact vertex ID, make sure refinement gets the same vertex ID
                if ( StartVIDIndex > -1 && SurfacePt.BaryCoord[StartVIDIndex] == 1.0 )
                {
                    SurfacePt.ElementID = Mesh->GetTriangle( SurfacePt.ElementID )[StartVIDIndex];
                    SurfacePt.PointType = SurfacePointType::Vertex;
                }
                else
                {
                    RefineSurfacePtFromTriangleToSubElement( Mesh, SurfacePt.Pos( Mesh ), SurfacePt,
                                                             PtOnPlaneThresholdSq );
                }
                if ( WalkedPath.size() > 1 && SurfacePt.PointType != SurfacePointType::Triangle &&
                     SurfacePt.PointType == WalkedPath[1].first.PointType &&
                     SurfacePt.ElementID == WalkedPath[1].first.ElementID )
                {
                    if ( SurfacePt.PointType == SurfacePointType::Edge ) // copy closer barycoord
                    {
                        WalkedPath[1].first.BaryCoord = SurfacePt.BaryCoord;
                    }
                    WalkedPath.erase( WalkedPath.begin() );
                }
            }
            if ( !WalkedPath.empty() && WalkedPath.back().first.PointType == SurfacePointType::Triangle )
            {
                MeshSurfacePoint& SurfacePt = WalkedPath.back().first;
                RefineSurfacePtFromTriangleToSubElement( Mesh, SurfacePt.Pos( Mesh ), SurfacePt,
                                                         PtOnPlaneThresholdSq );
                if ( WalkedPath.size() > 1 && SurfacePt.PointType != SurfacePointType::Triangle &&
                     SurfacePt.PointType == WalkedPath[WalkedPath.size() - 2].first.PointType &&
                     SurfacePt.ElementID == WalkedPath[WalkedPath.size() - 2].first.ElementID )
                {
                    if ( SurfacePt.PointType == SurfacePointType::Edge ) // copy closer barycoord
                    {
                        WalkedPath[WalkedPath.size() - 2].first.BaryCoord = SurfacePt.BaryCoord;
                    }
                    WalkedPath.pop_back();
                }
            }

            return true;
        }
    } // namespace

    bool MeshSurfacePath::IsConnected() const
    {
        for ( int Idx = 1, LastIdx = 0; Idx < static_cast<int32_t>( m_Path.size() ); LastIdx = Idx++ )
        {
            const int WalkingOnTri = m_Path[LastIdx].second;
            if ( !m_Mesh->IsTriangle( WalkingOnTri ) )
            {
                return false;
            }
            const int Inds[2] = { LastIdx, Idx };
            for ( const int Ind : Inds )
            {
                const MeshSurfacePoint& P = m_Path[Ind].first;
                switch ( P.PointType )
                {
                    case SurfacePointType::Triangle:
                        if ( P.ElementID != WalkingOnTri )
                        {
                            return false;
                        }
                        break;
                    case SurfacePointType::Edge:
                        if ( !m_Mesh->GetEdgeT( P.ElementID ).Contains( WalkingOnTri ) )
                        {
                            return false;
                        }
                        break;
                    case SurfacePointType::Vertex:
                        if ( !m_Mesh->GetTriangle( WalkingOnTri ).Contains( P.ElementID ) )
                        {
                            return false;
                        }
                        break;
                }
            }
        }
        return true;
    }

    bool MeshSurfacePath::AddViaPlanarWalk( int StartTri, int StartVID, glm::dvec3 StartPt, int EndTri,
                                            int EndVertID, glm::dvec3 EndPt, glm::dvec3 WalkPlaneNormal,
                                            std::function<glm::dvec3( const DynamicMesh3*, int )> VertexToPosnFn,
                                            bool bAllowBackwardsSearch, double AcceptEndPtOutsideDist,
                                            double PtOnPlaneThresholdSq, double BackwardsTolerance )
    {
        if ( !VertexToPosnFn )
        {
            VertexToPosnFn = []( const DynamicMesh3* MeshArg, int VertexID )
            { return MeshArg->GetVertex( VertexID ); };
        }
        return WalkMeshPlanar( m_Mesh, StartTri, StartVID, StartPt, EndTri, EndVertID, EndPt, WalkPlaneNormal,
                               VertexToPosnFn, bAllowBackwardsSearch, AcceptEndPtOutsideDist, PtOnPlaneThresholdSq,
                               m_Path, BackwardsTolerance );
    }

    bool MeshSurfacePath::EmbedSimplePath( std::vector<int>& PathVertices, bool bDoNotDuplicateFirstVertexID,
                                           double SnapElementThresholdSq, const EmbedSimplePathSettings& Settings )
    {
        // used to track where the new vertices for *this* path start; used for bDoNotDuplicateFirstVertexID
        const auto InitialPathIdx = static_cast<int32_t>( PathVertices.size() );

        if ( m_Path.empty() )
        {
            return true;
        }

        if ( Settings.bSimplifyPathBySnapping )
        {
            // Helper to snap tri and edge surface points to vertices, if they're within the threshold distance
            auto SnapToVertex = [this, SnapElementThresholdSq]( MeshSurfacePoint& Pt ) -> bool
            {
                if ( Pt.PointType == SurfacePointType::Triangle )
                {
                    std::array<glm::dvec3, 3> TriV{};
                    m_Mesh->GetTriVertices( Pt.ElementID, TriV[0], TriV[1], TriV[2] );
                    const glm::dvec3 NewPt =
                         Pt.BaryCoord[0] * TriV[0] + Pt.BaryCoord[1] * TriV[1] + Pt.BaryCoord[2] * TriV[2];
                    int32_t ClosestIdx = -1;
                    if ( SelectSnapPoint( TriV, NewPt, ClosestIdx, SnapElementThresholdSq ) )
                    {
                        Pt = MeshSurfacePoint( m_Mesh->GetTriangle( Pt.ElementID )[ClosestIdx] );
                        return true;
                    }
                }
                else if ( Pt.PointType == SurfacePointType::Edge )
                {
                    const Index2i             EdgeV = m_Mesh->GetEdgeV( Pt.ElementID );
                    std::array<glm::dvec3, 2> EdgePos{};
                    m_Mesh->GetEdgeV( Pt.ElementID, EdgePos[0], EdgePos[1] );
                    const glm::dvec3 NewPt      = Pt.BaryCoord[0] * EdgePos[0] + Pt.BaryCoord[1] * EdgePos[1];
                    int32_t          ClosestIdx = -1;
                    if ( SelectSnapPoint( EdgePos, NewPt, ClosestIdx, SnapElementThresholdSq ) )
                    {
                        Pt = MeshSurfacePoint( EdgeV[ClosestIdx] );
                        return true;
                    }
                }
                return false;
            };

            int32_t NumRemoved = 0;
            for ( int32_t PathIdx = 0; PathIdx < static_cast<int32_t>( m_Path.size() ); ++PathIdx )
            {
                const int32_t OffsetPathIdx = PathIdx - NumRemoved;
                if ( NumRemoved > 0 )
                {
                    m_Path[OffsetPathIdx] = m_Path[PathIdx];
                }

                if ( SnapToVertex( m_Path[OffsetPathIdx].first ) )
                {
                    const int32_t LastValidIdx = PathIdx - 1 - NumRemoved;
                    // if snapped point is a duplicate, can skip it
                    if ( LastValidIdx >= 0 && m_Path[LastValidIdx].first.PointType == SurfacePointType::Vertex &&
                         m_Path[LastValidIdx].first.ElementID == m_Path[OffsetPathIdx].first.ElementID )
                    {
                        NumRemoved++;
                    }
                }
            }
            m_Path.resize( m_Path.size() - NumRemoved );
        }

        // Snapping on near-degenerate paths can cause the path to revisit a vertex. For paths that were intended
        // to be straight line connections, we can skip all points between the duplicated vertex(/vertices)
        if ( Settings.bRemovePathLoops )
        {
            int32_t                          NumRemoved = 0;
            std::unordered_map<int, int32_t> SeenVertices;
            SeenVertices.reserve( m_Path.size() );

            for ( int32_t PathIdx = 0; PathIdx < static_cast<int32_t>( m_Path.size() ); ++PathIdx )
            {
                const int32_t OffsetPathIdx = PathIdx - NumRemoved;
                if ( NumRemoved > 0 )
                {
                    m_Path[OffsetPathIdx] = m_Path[PathIdx];
                }

                if ( m_Path[OffsetPathIdx].first.PointType != SurfacePointType::Vertex )
                {
                    continue;
                }
                const auto SeenBefore = SeenVertices.find( m_Path[OffsetPathIdx].first.ElementID );
                if ( SeenBefore == SeenVertices.end() )
                {
                    SeenVertices.emplace( m_Path[OffsetPathIdx].first.ElementID, OffsetPathIdx );
                    continue;
                }
                const int32_t SeenBeforeVal = SeenBefore->second;
                DESERT_VERIFY_WARN( OffsetPathIdx > SeenBeforeVal );
                NumRemoved += OffsetPathIdx - SeenBeforeVal;
                for ( int32_t WalkBack = OffsetPathIdx - 1; WalkBack > SeenBeforeVal; --WalkBack )
                {
                    if ( m_Path[WalkBack].first.PointType == SurfacePointType::Vertex )
                    {
                        SeenVertices.erase( m_Path[WalkBack].first.ElementID );
                    }
                }
            }
            m_Path.resize( m_Path.size() - NumRemoved );
        }

        const auto              PathNum   = static_cast<int32_t>( m_Path.size() );
        const MeshSurfacePoint& OrigEndPt = m_Path[PathNum - 1].first;
        // If FinalTri is split or poked, we will need to re-locate the last point in the path
        int  StartProcessIdx         = 0;
        int  EndSimpleProcessIdx     = PathNum - 1;
        bool bEndPointSpecialProcess = false;
        if ( PathNum > 1 && OrigEndPt.PointType == SurfacePointType::Triangle )
        {
            EndSimpleProcessIdx     = PathNum - 2;
            bEndPointSpecialProcess = true;
        }
        MeshSurfacePoint EndPtUpdated = m_Path.back().first;
        const glm::dvec3 EndPtPos     = OrigEndPt.Pos( m_Mesh );

        if ( m_Path[0].first.PointType == SurfacePointType::Triangle )
        {
            // poke triangle, and place initial vertex
            DynamicMesh3::PokeTriangleInfo PokeInfo;
            if ( !Common::EnsureOrWarn( m_Mesh->PokeTriangle( m_Path[0].first.ElementID, m_Path[0].first.BaryCoord,
                                                              PokeInfo ) == MeshResult::Ok,
                                        "Mesh->PokeTriangle( Path[0].first.ElementID, Path[0].first.BaryCoord, "
                                        "PokeInfo ) == MeshResult::Ok" ) )
            {
                return false;
            }
            if ( EndPtUpdated.PointType == SurfacePointType::Triangle &&
                 m_Path[0].first.ElementID == EndPtUpdated.ElementID )
            {
                const std::array<int, 3> EndCandidateTris{ PokeInfo.NewTriangles.A, PokeInfo.NewTriangles.B,
                                                           PokeInfo.OriginalTriangle };
                EndPtUpdated = RelocateTrianglePointAfterRefinement( m_Mesh, EndPtPos, EndCandidateTris,
                                                                     SnapElementThresholdSq );
            }
            PathVertices.push_back( PokeInfo.NewVertex );
            StartProcessIdx = 1;
        }

        for ( int32_t PathIdx = StartProcessIdx; PathIdx <= EndSimpleProcessIdx; PathIdx++ )
        {
            if ( !Common::EnsureOrWarn( m_Path[PathIdx].first.PointType != SurfacePointType::Triangle,
                                        "Path[PathIdx].first.PointType != SurfacePointType::Triangle" ) )
            {
                // Input assumptions violated -- Simple path can only have Triangle points at the very first and/or
                // last points!  Would need a more powerful embed function to handle this case.
                return false;
            }
            const MeshSurfacePoint& Pt = m_Path[PathIdx].first;
            if ( Pt.PointType == SurfacePointType::Edge )
            {
                if ( Settings.bAllowEdgeFlipToCollapseTinyEdges && !PathVertices.empty() &&
                     !m_Mesh->IsBoundaryEdge( Pt.ElementID ) )
                {
                    // If the edge we insert would be degenerate, try to flip the edge rather than splitting it.
                    // Note this is equivalent to splitting the edge then immediately collapsing the split
                    std::array<glm::dvec3, 2> EdgeVerts{};
                    m_Mesh->GetEdgeV( Pt.ElementID, EdgeVerts[0], EdgeVerts[1] );
                    const glm::dvec3 EdgePt  = EdgeVerts[0] * Pt.BaryCoord[0] + EdgeVerts[1] * Pt.BaryCoord[1];
                    const glm::dvec3 LastPos = m_Mesh->GetVertex( PathVertices.back() );
                    if ( DistanceSquared( LastPos, EdgePt ) < SnapElementThresholdSq )
                    {
                        const Index2i                   OppVIDs = m_Mesh->GetEdgeOpposingV( Pt.ElementID );
                        const std::array<glm::dvec3, 2> OpposingVerts{ m_Mesh->GetVertex( OppVIDs.A ),
                                                                       m_Mesh->GetVertex( OppVIDs.B ) };
                        // These normals may face opposite to the triangles' own (the edge's vertex order need not
                        // match the first triangle's winding), but only their relative orientation is tested
                        const std::array<glm::dvec3, 2> Normals{
                             VectorUtil::Normal( EdgeVerts[0], EdgeVerts[1], OpposingVerts[0] ),
                             VectorUtil::Normal( EdgeVerts[1], EdgeVerts[0], OpposingVerts[1] ) };
                        const std::array<glm::dvec3, 2> FlipNormals{
                             VectorUtil::Normal( OpposingVerts[0], OpposingVerts[1], EdgeVerts[1] ),
                             VectorUtil::Normal( OpposingVerts[1], OpposingVerts[0], EdgeVerts[0] ) };
                        // Only flip if (1) the original triangles face the same way and (2) flipping won't create
                        // a 'fold over'. Degenerate triangles have no clear normal, so the flip is allowed there
                        // to avoid introducing even more degenerate geometry
                        if ( Normals[1] == glm::dvec3( 0 ) || ( glm::dot( Normals[0], Normals[1] ) >= 0 &&
                                                                glm::dot( Normals[0], FlipNormals[0] ) >= 0. &&
                                                                glm::dot( Normals[0], FlipNormals[1] ) >= 0 ) )
                        {
                            // EmbedSimplePath crosses each triangle at most once (except the start/end tri), so no
                            // later path point references the edge we flip
                            DynamicMesh3::EdgeFlipInfo FlipInfo;
                            if ( m_Mesh->FlipEdge( Pt.ElementID, FlipInfo ) == MeshResult::Ok )
                            {
                                if ( EndPtUpdated.PointType == SurfacePointType::Triangle &&
                                     FlipInfo.Triangles.Contains( EndPtUpdated.ElementID ) )
                                {
                                    const std::array<int, 2> TriInds{ FlipInfo.Triangles.A, FlipInfo.Triangles.B };
                                    EndPtUpdated = RelocateTrianglePointAfterRefinement( m_Mesh, EndPtPos, TriInds,
                                                                                         SnapElementThresholdSq );
                                }
                                else if ( PathIdx != PathNum - 1 &&
                                          EndPtUpdated.PointType == SurfacePointType::Edge &&
                                          Pt.ElementID == EndPtUpdated.ElementID )
                                {
                                    // The end point's edge now joins other vertices; UE has no relocation for it.
                                    DESERT_VERIFY_WARN( false );
                                    return false;
                                }
                                // no vertex joins the path: whatever the split vertex would have reached is
                                // reachable from the previous path vertex after the flip
                                continue;
                            }
                        }
                    }
                }

                DynamicMesh3::EdgeSplitInfo SplitInfo;
                if ( !Common::EnsureOrWarn( m_Mesh->SplitEdge( Pt.ElementID, SplitInfo, Pt.GetEdgeSplitParam() ) ==
                                                 MeshResult::Ok,
                                            "Mesh->SplitEdge( Pt.ElementID, SplitInfo, Pt.GetEdgeSplitParam() ) "
                                            "== MeshResult::Ok" ) )
                {
                    return false;
                }
                PathVertices.push_back( SplitInfo.NewVertex );
                if ( EndPtUpdated.PointType == SurfacePointType::Triangle &&
                     SplitInfo.OriginalTriangles.Contains( EndPtUpdated.ElementID ) )
                {
                    const std::array<int, 2> TriInds{ EndPtUpdated.ElementID,
                                                      SplitInfo.OriginalTriangles.A == EndPtUpdated.ElementID
                                                           ? SplitInfo.NewTriangles.A
                                                           : SplitInfo.NewTriangles.B };
                    EndPtUpdated =
                         RelocateTrianglePointAfterRefinement( m_Mesh, EndPtPos, TriInds, SnapElementThresholdSq );
                }
                else if ( PathIdx != PathNum - 1 && EndPtUpdated.PointType == SurfacePointType::Edge &&
                          Pt.ElementID == EndPtUpdated.ElementID )
                {
                    // The end point's edge is gone and UE has no relocation for this case.
                    DESERT_VERIFY_WARN( false );
                    return false;
                }
            }
            else
            {
                DESERT_VERIFY_WARN( Pt.PointType == SurfacePointType::Vertex );
                DESERT_VERIFY_WARN( m_Mesh->IsVertex( Pt.ElementID ) );
                // make sure we don't add a duplicate vertex for the very first vertex (occurs when appending paths
                // sequentially)
                if ( !bDoNotDuplicateFirstVertexID ||
                     static_cast<int32_t>( PathVertices.size() ) != InitialPathIdx ||
                     0 == static_cast<int32_t>( PathVertices.size() ) || PathVertices.back() != Pt.ElementID )
                {
                    PathVertices.push_back( Pt.ElementID );
                }
            }
        }

        if ( bEndPointSpecialProcess )
        {
            if ( EndPtUpdated.PointType == SurfacePointType::Triangle )
            {
                DynamicMesh3::PokeTriangleInfo PokeInfo;
                if ( !Common::EnsureOrWarn( m_Mesh->PokeTriangle( EndPtUpdated.ElementID, EndPtUpdated.BaryCoord,
                                                                  PokeInfo ) == MeshResult::Ok,
                                            "Mesh->PokeTriangle( EndPtUpdated.ElementID, EndPtUpdated.BaryCoord, "
                                            "PokeInfo ) == MeshResult::Ok" ) )
                {
                    return false;
                }
                PathVertices.push_back( PokeInfo.NewVertex );
            }
            else if ( EndPtUpdated.PointType == SurfacePointType::Edge )
            {
                DynamicMesh3::EdgeSplitInfo SplitInfo;
                if ( !Common::EnsureOrWarn( m_Mesh->SplitEdge( EndPtUpdated.ElementID, SplitInfo,
                                                               EndPtUpdated.GetEdgeSplitParam() ) ==
                                                 MeshResult::Ok,
                                            "Mesh->SplitEdge( EndPtUpdated.ElementID, SplitInfo, "
                                            "EndPtUpdated.GetEdgeSplitParam() ) == MeshResult::Ok" ) )
                {
                    return false;
                }
                PathVertices.push_back( SplitInfo.NewVertex );
            }
            else
            {
                if ( PathVertices.empty() || PathVertices.back() != EndPtUpdated.ElementID )
                {
                    PathVertices.push_back( EndPtUpdated.ElementID );
                }
            }
        }

        return true;
    }
} // namespace Desert::Geometry
