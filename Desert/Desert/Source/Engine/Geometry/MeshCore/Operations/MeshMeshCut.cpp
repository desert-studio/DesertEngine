// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Operations/MeshMeshCut.cpp:10-571
// (FCutWorkingInfo), 589-614 (FMeshMeshCut::Cut), adapted: std/glm, namespace Desert::Geometry; checkf/ensure are
// asserts, and an ensure that UE recovers from is recovered from the same way here. TMultiMap -> std::multimap:
// the next pending element is the LOWEST triangle/edge ID and points on one element come in insertion order, where
// UE takes whatever its hashed TMultiMap iterates first -- a fixed order either way, so two runs cut identically,
// but not necessarily the same order as UE. Left out: FMeshSelfCut::Cut (see the header) and SegmentToChain.
#include "Engine/Geometry/MeshCore/Operations/MeshMeshCut.hpp"

#include "Engine/Geometry/MeshCore/Operations/EmbedSurfacePath.hpp"
#include "Engine/Geometry/MeshCore/SegmentTypes.hpp"
#include "Engine/Geometry/MeshCore/VectorUtil.hpp"

#include <cassert>
#include <limits>
#include <map>

namespace Desert::Geometry
{
    namespace
    {
        enum class VertexType
        {
            Unknown = -1,
            Vertex  = 0,
            Edge    = 1,
            Face    = 2
        };

        // An intersection point + where it maps to on the surface
        struct PtOnMesh
        {
            glm::dvec3 Pos{};
            VertexType Type   = VertexType::Unknown;
            int        ElemID = IndexConstants::InvalidID;
        };

        // Mapping from intersection segments to source triangle and intersection points
        struct SegmentToElements
        {
            int BaseTID = IndexConstants::InvalidID; // triangle the segment was on *before* the mesh was cut
            int PtOnMeshIdx[2]{ -1, -1 };            // indices into IntersectionVerts for the two endpoints
        };

        using Tri3 = std::array<glm::dvec3, 3>;

        // Per mesh info about an in-progress cut. Only stored while the cut is performed.
        struct CutWorkingInfo
        {
            CutWorkingInfo( DynamicMesh3* WorkingMesh, double SnapTolerance )
                 : Mesh( WorkingMesh ), SnapToleranceSq( SnapTolerance * SnapTolerance )
            {
                BaseFaceNormals.resize( WorkingMesh->MaxTriangleID() );
                for ( int TID : WorkingMesh->TriangleIndicesItr() )
                {
                    BaseFaceNormals[TID] = WorkingMesh->GetTriNormal( TID );
                }
            }

            // the direction of the first non-degenerate edge (or a default vector if all edges were degenerate)
            static glm::dvec3 GetDegenTriangleEdgeDirection( const DynamicMesh3* Mesh, int TID,
                                                             glm::dvec3 DefaultDir = glm::dvec3( 0, 0, 1 ) )
            {
                Tri3 V;
                Mesh->GetTriVertices( TID, V[0], V[1], V[2] );
                for ( int Prev = 2, Idx = 0; Idx < 3; Prev = Idx++ )
                {
                    glm::dvec3 E = V[Idx] - V[Prev];
                    if ( Normalize( E ) > 0 )
                    {
                        return E;
                    }
                }
                return DefaultDir;
            }

            DynamicMesh3* Mesh;
            double        SnapToleranceSq;

            // triangle ID -> IntersectionVerts index for intersection pts that we still have to insert
            std::multimap<int, int> FaceVertices;
            // edge ID -> IntersectionVerts index for intersection pts that we still have to insert
            std::multimap<int, int> EdgeVertices;
            // Normals of original triangles (before the cut), for the planar walk fallback
            std::vector<glm::dvec3> BaseFaceNormals;
            // points on the mesh -- after the cut, these all correspond to mesh vertices
            std::vector<PtOnMesh> IntersectionVerts;
            // mapping of intersection segments to original mesh triangles and IntersectionVerts
            std::vector<SegmentToElements> Segments;

            void AddSegments( const MeshIntersection::IntersectionsQueryResult& Intersections, int WhichSide )
            {
                const size_t SegStart = Segments.size();
                Segments.resize( SegStart + Intersections.Segments.size() );

                // classify the points of each intersection segment as on-vertex, on-edge, or on-face
                for ( size_t SegIdx = 0; SegIdx < Intersections.Segments.size(); SegIdx++ )
                {
                    const MeshIntersection::SegmentIntersection& Seg      = Intersections.Segments[SegIdx];
                    SegmentToElements&                           SegToEls = Segments[SegStart + SegIdx];
                    SegToEls.BaseTID                                      = Seg.TriangleID[WhichSide];

                    Tri3 Tri;
                    Mesh->GetTriVertices( SegToEls.BaseTID, Tri[0], Tri[1], Tri[2] );
                    const Index3i TriVIDs       = Mesh->GetTriangle( SegToEls.BaseTID );
                    int           PrevOnEdgeIdx = -1;
                    glm::dvec3    PrevOnEdgePos{};
                    for ( int SegPtIdx = 0; SegPtIdx < 2; SegPtIdx++ )
                    {
                        const int NewPtIdx             = static_cast<int>( IntersectionVerts.size() );
                        PtOnMesh& Pt                   = IntersectionVerts.emplace_back();
                        Pt.Pos                         = Seg.Point[SegPtIdx];
                        SegToEls.PtOnMeshIdx[SegPtIdx] = NewPtIdx;

                        // decide whether the point is on a vertex, edge or triangle
                        const int OnVertexIdx = OnVertex( Tri, Pt.Pos );
                        if ( OnVertexIdx > -1 )
                        {
                            Pt.Type   = VertexType::Vertex;
                            Pt.ElemID = TriVIDs[OnVertexIdx];
                            continue;
                        }

                        int OnEdgeIdx = OnEdge( Tri, Pt.Pos );
                        if ( OnEdgeIdx > -1 )
                        {
                            // if segment is degenerate and stuck to one edge, see if it could cross
                            if ( PrevOnEdgeIdx == OnEdgeIdx &&
                                 DistanceSquared( PrevOnEdgePos, Pt.Pos ) < SnapToleranceSq )
                            {
                                const int OnEdgeReplaceIdx = OnEdgeWithSkip( Tri, Pt.Pos, OnEdgeIdx );
                                if ( OnEdgeReplaceIdx > -1 )
                                {
                                    OnEdgeIdx = OnEdgeReplaceIdx;
                                }
                            }
                            Pt.Type   = VertexType::Edge;
                            Pt.ElemID = Mesh->GetTriEdge( SegToEls.BaseTID, OnEdgeIdx );
                            assert( Pt.ElemID > -1 );
                            EdgeVertices.emplace( Pt.ElemID, NewPtIdx );

                            PrevOnEdgeIdx = OnEdgeIdx;
                            PrevOnEdgePos = Pt.Pos;
                            continue;
                        }

                        // wasn't vertex or edge, so it's a face vertex
                        Pt.Type   = VertexType::Face;
                        Pt.ElemID = SegToEls.BaseTID;
                        FaceVertices.emplace( Pt.ElemID, NewPtIdx );
                    }
                }
            }

            static void TakeAll( std::multimap<int, int>& Map, int Key, std::vector<int>& Out )
            {
                Out.clear();
                const auto Range = Map.equal_range( Key );
                for ( auto It = Range.first; It != Range.second; ++It )
                {
                    Out.push_back( It->second );
                }
                Map.erase( Range.first, Range.second );
            }

            void InsertFaceVertices()
            {
                Tri3             Tri;
                std::vector<int> PtIndices;
                while ( !FaceVertices.empty() )
                {
                    const int TID   = FaceVertices.begin()->first;
                    const int PtIdx = FaceVertices.begin()->second;
                    TakeAll( FaceVertices, TID, PtIndices );

                    PtOnMesh& Pt = IntersectionVerts[PtIdx];
                    Mesh->GetTriVertices( TID, Tri[0], Tri[1], Tri[2] );
                    // not robust to degenerate triangles, but in degenerate cases the point was snapped to a
                    // vertex or edge (unless the snap tolerance is too low)
                    const glm::dvec3 BaryCoords = VectorUtil::BarycentricCoords( Pt.Pos, Tri[0], Tri[1], Tri[2] );
                    DynamicMeshInfo::PokeTriangleInfo PokeInfo;
                    [[maybe_unused]] const MeshResult Result = Mesh->PokeTriangle( TID, BaryCoords, PokeInfo );
                    assert( Result == MeshResult::Ok );
                    const int PokeVID = PokeInfo.NewVertex;
                    // set to the intersection pos so that new vertices on both meshes have matching positions
                    Mesh->SetVertex( PokeVID, Pt.Pos );
                    Pt.ElemID = PokeVID;
                    Pt.Type   = VertexType::Vertex;

                    const Index3i PokeTriangles( TID, PokeInfo.NewTriangles.A, PokeInfo.NewTriangles.B );
                    // if there were other points on the face, redistribute them among the new triangles
                    for ( int RelocatePtIdx : PtIndices )
                    {
                        if ( PtIdx == RelocatePtIdx )
                        {
                            continue;
                        }
                        PtOnMesh& RelocatePt = IntersectionVerts[RelocatePtIdx];
                        UpdateFromPoke( RelocatePt, PokeInfo.NewVertex, PokeInfo.NewEdges, PokeTriangles );
                        if ( RelocatePt.Type == VertexType::Edge )
                        {
                            EdgeVertices.emplace( RelocatePt.ElemID, RelocatePtIdx );
                        }
                        else if ( RelocatePt.Type == VertexType::Face )
                        {
                            FaceVertices.emplace( RelocatePt.ElemID, RelocatePtIdx );
                        }
                    }
                }
            }

            void InsertEdgeVertices()
            {
                std::vector<int> PtIndices;
                while ( !EdgeVertices.empty() )
                {
                    const int EID   = EdgeVertices.begin()->first;
                    const int PtIdx = EdgeVertices.begin()->second;
                    TakeAll( EdgeVertices, EID, PtIndices );

                    PtOnMesh&  Pt = IntersectionVerts[PtIdx];
                    glm::dvec3 EA, EB;
                    Mesh->GetEdgeV( EID, EA, EB );
                    const Segment3<double> Seg( EA, EB );
                    const double           SplitParam = Seg.ProjectUnitRange( Pt.Pos );

                    DynamicMeshInfo::EdgeSplitInfo    SplitInfo;
                    [[maybe_unused]] const MeshResult Result = Mesh->SplitEdge( EID, SplitInfo, SplitParam );
                    assert( Result == MeshResult::Ok );

                    Mesh->SetVertex( SplitInfo.NewVertex, Pt.Pos );
                    Pt.ElemID = SplitInfo.NewVertex;
                    Pt.Type   = VertexType::Vertex;

                    // if there were other points on the edge, redistribute them to the new edges
                    const Index2i SplitEdges{ SplitInfo.OriginalEdge, SplitInfo.NewEdges.A };
                    for ( int RelocatePtIdx : PtIndices )
                    {
                        if ( PtIdx == RelocatePtIdx )
                        {
                            continue;
                        }
                        PtOnMesh& RelocatePt = IntersectionVerts[RelocatePtIdx];
                        UpdateFromSplit( RelocatePt, SplitInfo.NewVertex, SplitEdges );
                        if ( RelocatePt.Type == VertexType::Edge )
                        {
                            EdgeVertices.emplace( RelocatePt.ElemID, RelocatePtIdx );
                        }
                    }
                }
            }

            // VertexChains: packed chains of the vertices each segment was embedded along
            bool ConnectEdges( std::vector<int>& VertexChains )
            {
                std::vector<int> EmbeddedPath;
                bool             bSuccess = true; // remains true if we connect all edges

                for ( const SegmentToElements& Seg : Segments )
                {
                    if ( Seg.PtOnMeshIdx[0] == Seg.PtOnMeshIdx[1] )
                    {
                        continue; // degenerate case, but OK
                    }
                    const PtOnMesh& PtA = IntersectionVerts[Seg.PtOnMeshIdx[0]];
                    const PtOnMesh& PtB = IntersectionVerts[Seg.PtOnMeshIdx[1]];
                    if ( !( PtA.Type == VertexType::Vertex && PtB.Type == VertexType::Vertex &&
                            PtA.ElemID != IndexConstants::InvalidID && PtB.ElemID != IndexConstants::InvalidID ) )
                    {
                        // UE ensureMsgf: "Point insertion failed during mesh mesh cut!"
                        bSuccess = false;
                        continue;
                    }
                    if ( PtA.ElemID == PtB.ElemID )
                    {
                        VertexChains.push_back( 1 );
                        VertexChains.push_back( PtA.ElemID );
                        continue; // degenerate case, but OK
                    }

                    if ( Mesh->FindEdge( PtA.ElemID, PtB.ElemID ) != DynamicMesh3::InvalidID )
                    {
                        VertexChains.push_back( 2 );
                        VertexChains.push_back( PtA.ElemID );
                        VertexChains.push_back( PtB.ElemID );
                        continue; // already connected
                    }

                    MeshSurfacePath SurfacePath( Mesh );
                    const int       StartTID   = Mesh->GetVtxSingleTriangle( PtA.ElemID );
                    glm::dvec3 WalkPlaneNormal = glm::cross( BaseFaceNormals[Seg.BaseTID], PtB.Pos - PtA.Pos );
                    if ( Normalize( WalkPlaneNormal ) == 0 )
                    {
                        if ( DistanceSquared( PtA.Pos, PtB.Pos ) > SnapToleranceSq )
                        {
                            // separated path points: the degeneracy comes from colinear triangle vertices, spread
                            // along the original edges, which are already connected
                            continue;
                        }
                        // not separated: connect across a (likely degenerate) triangle with a walk normal that
                        // separates the triangle vertices even if the triangle collapsed to a line segment
                        WalkPlaneNormal = GetDegenTriangleEdgeDirection( Mesh, StartTID );
                        if ( Normalize( WalkPlaneNormal ) == 0 )
                        {
                            continue; // the triangle is a point, nothing to walk here
                        }
                    }
                    const bool bWalkSuccess = SurfacePath.AddViaPlanarWalk(
                         StartTID, PtA.ElemID, Mesh->GetVertex( PtA.ElemID ), -1, PtB.ElemID,
                         Mesh->GetVertex( PtB.ElemID ), WalkPlaneNormal, nullptr, false, ZeroTolerance<double>,
                         SnapToleranceSq, .001 );
                    if ( !bWalkSuccess )
                    {
                        bSuccess = false;
                        continue;
                    }
                    EmbeddedPath.clear();
                    if ( SurfacePath.EmbedSimplePath( EmbeddedPath, false, SnapToleranceSq,
                                                      EmbedSimplePathSettings::WithSimplification() ) )
                    {
                        assert( !EmbeddedPath.empty() && EmbeddedPath[0] == PtA.ElemID );
                        VertexChains.push_back( static_cast<int>( EmbeddedPath.size() ) );
                        VertexChains.insert( VertexChains.end(), EmbeddedPath.begin(), EmbeddedPath.end() );
                    }
                    else
                    {
                        bSuccess = false;
                    }
                }
                return bSuccess;
            }

            void UpdateFromSplit( PtOnMesh& Pt, int SplitVertex, const Index2i& SplitEdges ) const
            {
                // check if within tolerance of the new vtx
                if ( DistanceSquared( Pt.Pos, Mesh->GetVertex( SplitVertex ) ) < SnapToleranceSq )
                {
                    Pt.Type   = VertexType::Vertex;
                    Pt.ElemID = SplitVertex;
                    return;
                }
                // it was already on the edge, so it is on one of the sub-edges after the split: pick the closest
                const int EdgeIdx = ClosestEdge( SplitEdges, Pt.Pos );
                Pt.Type           = VertexType::Edge;
                Pt.ElemID         = SplitEdges[EdgeIdx];
            }

            void UpdateFromPoke( PtOnMesh& Pt, int PokeVertex, const Index3i& PokeEdges,
                                 const Index3i& PokeTris ) const
            {
                if ( DistanceSquared( Pt.Pos, Mesh->GetVertex( PokeVertex ) ) < SnapToleranceSq )
                {
                    Pt.Type   = VertexType::Vertex;
                    Pt.ElemID = PokeVertex;
                    return;
                }
                int EdgeIdx = OnEdge( PokeEdges, Pt.Pos, SnapToleranceSq );
                if ( EdgeIdx > -1 )
                {
                    Pt.Type   = VertexType::Edge;
                    Pt.ElemID = PokeEdges[EdgeIdx];
                    return;
                }
                for ( int j = 0; j < 3; ++j )
                {
                    if ( IsInTriangle( PokeTris[j], Pt.Pos ) )
                    {
                        assert( Pt.Type == VertexType::Face );
                        Pt.ElemID = PokeTris[j];
                        return;
                    }
                }
                // failsafe case: pt was outside of triangle -- project to edge
                EdgeIdx   = OnEdge( PokeEdges, Pt.Pos, std::numeric_limits<double>::max() );
                Pt.Type   = VertexType::Edge;
                Pt.ElemID = PokeEdges[EdgeIdx];
            }

            [[nodiscard]] int OnVertex( const Tri3& Tri, const glm::dvec3& V ) const
            {
                double BestDSq = SnapToleranceSq;
                int    BestIdx = -1;
                for ( int SubIdx = 0; SubIdx < 3; SubIdx++ )
                {
                    const double DSq = DistanceSquared( Tri[SubIdx], V );
                    if ( DSq < BestDSq )
                    {
                        BestIdx = SubIdx;
                        BestDSq = DSq;
                    }
                }
                return BestIdx;
            }

            [[nodiscard]] int OnEdge( const Tri3& Tri, const glm::dvec3& V ) const
            {
                return OnEdgeWithSkip( Tri, V, -1 );
            }

            [[nodiscard]] int OnEdgeWithSkip( const Tri3& Tri, const glm::dvec3& V, int SkipIdx ) const
            {
                double BestDSq = SnapToleranceSq;
                int    BestIdx = -1;
                for ( int Idx = 0; Idx < 3; Idx++ )
                {
                    if ( Idx == SkipIdx )
                    {
                        continue;
                    }
                    const Segment3<double> Seg( Tri[Idx], Tri[( Idx + 1 ) % 3] );
                    const double           DSq = Seg.DistanceSquared( V );
                    if ( DSq < BestDSq )
                    {
                        BestDSq = DSq;
                        BestIdx = Idx;
                    }
                }
                return BestIdx;
            }

            [[nodiscard]] double EdgeDistanceSquared( int EID, const glm::dvec3& Pos ) const
            {
                const Index2i          EVIDs = Mesh->GetEdgeV( EID );
                const Segment3<double> Seg( Mesh->GetVertex( EVIDs.A ), Mesh->GetVertex( EVIDs.B ) );
                return Seg.DistanceSquared( Pos );
            }

            [[nodiscard]] int ClosestEdge( const Index2i& EIDs, const glm::dvec3& Pos ) const
            {
                int    BestIdx = -1;
                double BestDSq = std::numeric_limits<double>::max();
                for ( int Idx = 0; Idx < 2; Idx++ )
                {
                    const double DSq = EdgeDistanceSquared( EIDs[Idx], Pos );
                    if ( DSq < BestDSq )
                    {
                        BestDSq = DSq;
                        BestIdx = Idx;
                    }
                }
                return BestIdx;
            }

            [[nodiscard]] int OnEdge( const Index3i& EIDs, const glm::dvec3& Pos, double BestDSq ) const
            {
                int BestIdx = -1;
                for ( int Idx = 0; Idx < 3; Idx++ )
                {
                    const double DSq = EdgeDistanceSquared( EIDs[Idx], Pos );
                    if ( DSq < BestDSq )
                    {
                        BestDSq = DSq;
                        BestIdx = Idx;
                    }
                }
                return BestIdx;
            }

            [[nodiscard]] bool IsInTriangle( int TID, const glm::dvec3& Pos ) const
            {
                Tri3 Tri;
                Mesh->GetTriVertices( TID, Tri[0], Tri[1], Tri[2] );
                const glm::dvec3 bary = VectorUtil::BarycentricCoords( Pos, Tri[0], Tri[1], Tri[2] );
                return ( bary.x >= 0 && bary.y >= 0 && bary.z >= 0 && bary.x < 1 && bary.y <= 1 && bary.z <= 1 );
            }
        };
    } // namespace

    bool MeshMeshCut::Cut( const MeshIntersection::IntersectionsQueryResult& Intersections )
    {
        ResetOutputs();
        bool      bSuccess        = true;
        const int MeshesToProcess = bMutuallyCut ? 2 : 1;
        for ( int MeshIdx = 0; MeshIdx < MeshesToProcess; MeshIdx++ )
        {
            CutWorkingInfo WorkingInfo( Mesh[MeshIdx], SnapTolerance );
            WorkingInfo.AddSegments( Intersections, MeshIdx ); // add intersection segments
            WorkingInfo.InsertFaceVertices();                  // insert vertices for segment endpoints on faces
            WorkingInfo.InsertEdgeVertices();                  // insert vertices for segment endpoints on edges
            // ensure that intersection segment endpoints are connected by direct edge paths
            if ( !WorkingInfo.ConnectEdges( VertexChains[MeshIdx] ) )
            {
                bSuccess = false;
            }
        }
        return bSuccess;
    }
} // namespace Desert::Geometry
