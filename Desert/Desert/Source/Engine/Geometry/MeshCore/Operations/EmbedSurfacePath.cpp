// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Operations/EmbedSurfacePath.cpp:13-125,591-634,
// 682-947, adapted: UE Core as std/glm, namespace Desert::Geometry, TStaticArray is std::array. Left out with
// the header's reasons: the FEmbedSimplePathSettings branches (694-805, 846-887) and the bUpdatePath tail
// (941-944). Where UE carries on after an ensure or ignores an MeshResult, this returns false instead: a split or
// poke that did not happen leaves no new vertex to put on the path, and an end point whose edge was split away can
// no longer be placed.
#include "Engine/Geometry/MeshCore/Operations/EmbedSurfacePath.hpp"

#include "Engine/Geometry/MeshCore/Distance/DistPoint3Triangle3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/SegmentTypes.hpp"
#include "Engine/Geometry/MeshCore/TriangleTypes.hpp"

#include <array>
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
            const int      TriID             = SurfacePt.ElementID;
            const Index3i  TriVertIDs        = Mesh->GetTriangle( TriID );
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
                SurfacePt.PointType = SurfacePointType::Vertex;
                return;
            }

            // failed to snap to vertex, try snapping to edge
            const Index3i  TriEdgeIDs    = Mesh->GetTriEdges( TriID );
            double         BestEdgeParam = 0;
            for ( int EdgeSubIdx = 0; EdgeSubIdx < 3; EdgeSubIdx++ )
            {
                const int EdgeID = TriEdgeIDs[EdgeSubIdx];
                glm::dvec3 EPosA{};
                glm::dvec3 EPosB{};
                Mesh->GetEdgeV( EdgeID, EPosA, EPosB );
                const Segment3d  EdgeSeg( EPosA, EPosB );
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
            double    BestTriDistSq = 0;
            glm::dvec3 BestBaryCoords{};
            int       BestTriID = -1;
            for ( const int TriID : TriIDs )
            {
                assert( Mesh->IsTriangle( TriID ) );
                const Index3i    TriVertIDs = Mesh->GetTriangle( TriID );
                const Triangle3d Tri( Mesh->GetVertex( TriVertIDs.A ), Mesh->GetVertex( TriVertIDs.B ),
                                      Mesh->GetVertex( TriVertIDs.C ) );
                // heavy duty way to get barycentric coordinates and check if on triangle; should be robust to
                // degenerate triangles unlike VectorUtil's barycentric coordinate function
                DistPoint3Triangle3d  TriDist( Pos, Tri );
                const double          DistSq = TriDist.GetSquared();
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

    bool MeshSurfacePath::EmbedSimplePath( std::vector<int>& PathVertices, bool bDoNotDuplicateFirstVertexID,
                                           double SnapElementThresholdSq )
    {
        // used to track where the new vertices for *this* path start; used for bDoNotDuplicateFirstVertexID
        const auto InitialPathIdx = static_cast<int32_t>( PathVertices.size() );

        if ( m_Path.empty() )
        {
            return true;
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
