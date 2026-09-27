// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/Operations/MeshBoolean.cpp:208-216 (Compute),
// 218-791 (ComputeWithCustomInside), 1048-1188 (MergeEdges), 1190-1207 (FindNearestEdge), adapted: std/glm,
// namespace Desert::Geometry; see the header for the settings left out. Replacements:
//  - MeshTransforms::ApplyTransform -> ScaleTranslate below: the boolean only ever applies a uniform positive
//    scale and a translation (both inputs share one space), which leaves normals unchanged and needs no
//    orientation flip, so vertex positions are all it touches;
//  - FDynamicMeshEditor::AppendMesh + FMeshIndexMappings -> EnableMatchingAttributes(no clearing) +
//    AppendWithOffsets; mesh B's IDs map to the result by the constant offsets;
//  - FSparseDynamicOctree3 of the other mesh's cut-boundary edges -> a linear pass over those edges with the same
//    box test against the edge's current bounds (the octree only kept those boxes up to date; the nearest edge is
//    chosen among the same candidates, ties go to the lower position in CutBoundaryEdges);
//  - ParallelFor over triangles -> a serial loop (each iteration wrote only its own KeepTri slot);
//  - TSet/TMap -> std::set/std::map: the boundary vertices are matched in ascending VID order (UE: hashed set
//    order), so two runs give byte-identical results;
//  - the degenerate tolerance box for FAxisAlignedBox3d::MaxDim is computed inline (our box has no MaxDim).
#include "Engine/Geometry/MeshCore/Operations/MeshBoolean.hpp"

#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/MeshCore/DynamicMeshEditor.hpp"
#include "Engine/Geometry/MeshCore/Operations/MeshMeshCut.hpp"
#include "Engine/Geometry/MeshCore/SegmentTypes.hpp"
#include "Engine/Geometry/MeshCore/Selections/MeshConnectedComponents.hpp"
#include "Engine/Geometry/MeshCore/Spatial/PointHashGrid3.hpp"

#include <algorithm>
#include <set>
#include <unordered_set>
#include <utility>

namespace Desert::Geometry
{
    namespace
    {
        double MaxDim( const AxisAlignedBox3d& Box )
        {
            const glm::dvec3 D = Box.Max - Box.Min;
            return std::max( D.x, std::max( D.y, D.z ) );
        }

        // Scale then translate every vertex: P' = Scale * P + Translation (UE FTransformSRT3d::TransformPosition
        // with an identity rotation).
        void ScaleTranslate( DynamicMesh3& Mesh, double Scale, const glm::dvec3& Translation )
        {
            for ( int VID : Mesh.VertexIndicesItr() )
            {
                Mesh.SetVertex( VID, Scale * Mesh.GetVertex( VID ) + Translation );
            }
        }
    } // namespace

    bool MeshBoolean::Compute()
    {
        const CustomInsideMeshTest WindingTest{
             [Threshold = WindingThreshold]( const glm::dvec3& Pt, const CustomInsideTestContext& Context ) -> bool
             { return Context.OptionalWindingTree->IsInside( Pt, Threshold ); }, true };
        return ComputeWithCustomInside( WindingTest, WindingTest );
    }

    bool MeshBoolean::ComputeWithCustomInside( const CustomInsideMeshTest& InsideMeshA,
                                               const CustomInsideMeshTest& InsideMeshB )
    {
        // copy meshes
        DynamicMesh3 CutMeshB( *Meshes[1] );
        if ( Result != Meshes[0] )
        {
            *Result = *Meshes[0];
        }
        std::array<DynamicMesh3*, 2> CutMesh{ Result, &CutMeshB }; // just an alias to keep things organized

        // transform the copies to a shared space (centered at the origin and scaled to a unit cube)
        AxisAlignedBox3d CombinedAABB = CutMesh[0]->GetBounds();
        CombinedAABB.Contain( CutMesh[1]->GetBounds() );
        const double     ScaleFactor = 1.0 / std::clamp( MaxDim( CombinedAABB ), 0.01, 1000000.0 );
        const glm::dvec3 Center      = CombinedAABB.Center();
        for ( int MeshIdx = 0; MeshIdx < 2; MeshIdx++ )
        {
            ScaleTranslate( *CutMesh[MeshIdx], ScaleFactor, ScaleFactor * ( -Center ) );
        }

        // build spatial data and use it to find intersections
        std::array<DynamicMeshAABBTree3, 2> Spatial{ DynamicMeshAABBTree3( CutMesh[0] ),
                                                     DynamicMeshAABBTree3( CutMesh[1] ) };
        Spatial[0].SetTolerance( SnapTolerance );
        Spatial[1].SetTolerance( SnapTolerance );
        const MeshIntersection::IntersectionsQueryResult Intersections =
             Spatial[0].FindAllIntersections( Spatial[1], MeshQueryOptions(), MeshQueryOptions(),
                                              [this]( IntrTriangle3Triangle3& Intr )
                                              {
                                                  Intr.SetTolerance( SnapTolerance );
                                                  return Intr.Find();
                                              } );

        const bool bOpOnSingleMesh = OperatesOnSingleMesh( Operation );

        // cut the meshes
        MeshMeshCut Cut( CutMesh[0], CutMesh[1] );
        Cut.bMutuallyCut  = !bOpOnSingleMesh;
        Cut.SnapTolerance = SnapTolerance;
        Cut.Cut( Intersections );

        const int NumMeshesToProcess = bOpOnSingleMesh ? 1 : 2;

        // collapse tiny edges along cut boundary
        const double DegenerateEdgeTolSq =
             DegenerateEdgeTolFactor * DegenerateEdgeTolFactor * SnapTolerance * SnapTolerance;
        for ( int MeshIdx = 0; MeshIdx < NumMeshesToProcess; MeshIdx++ )
        {
            DynamicMesh3&           Mesh   = *CutMesh[MeshIdx];
            const std::vector<int>& Chains = Cut.VertexChains[MeshIdx];
            // convert vertex chains to edge IDs to simplify finding remaining candidate edges after collapses
            std::vector<int> EIDs;
            for ( size_t ChainIdx = 0; ChainIdx < Chains.size(); )
            {
                const size_t ChainLen = static_cast<size_t>( Chains[ChainIdx] );
                const size_t ChainEnd = ChainIdx + 1 + ChainLen;
                for ( size_t ChainSubIdx = ChainIdx + 1; ChainSubIdx + 1 < ChainEnd; ChainSubIdx++ )
                {
                    const int VID0 = Chains[ChainSubIdx];
                    const int VID1 = Chains[ChainSubIdx + 1];
                    if ( DistanceSquared( Mesh.GetVertex( VID0 ), Mesh.GetVertex( VID1 ) ) < DegenerateEdgeTolSq )
                    {
                        EIDs.push_back( Mesh.FindEdge( VID0, VID1 ) );
                    }
                }
                ChainIdx = ChainEnd;
            }
            std::unordered_set<int> AllEIDs( EIDs.begin(), EIDs.end() );
            for ( size_t Idx = 0; Idx < EIDs.size(); Idx++ )
            {
                const int EID = EIDs[Idx];
                if ( !Mesh.IsEdge( EID ) )
                {
                    continue;
                }
                glm::dvec3 A, B;
                Mesh.GetEdgeV( EID, A, B );
                if ( DistanceSquared( A, B ) > DegenerateEdgeTolSq )
                {
                    continue;
                }
                Index2i EV = Mesh.GetEdgeV( EID );
                // if the vertex we'd remove is on a seam, try removing the other one instead
                if ( Mesh.HasAttributes() && Mesh.Attributes()->IsSeamVertex( EV.B, false ) )
                {
                    std::swap( EV.A, EV.B );
                    // if both are on seams, the collapse would break the overlay's OnCollapseEdge assumptions
                    if ( Mesh.Attributes()->IsSeamVertex( EV.B, false ) )
                    {
                        continue;
                    }
                }
                DynamicMesh3::EdgeCollapseInfo CollapseInfo;
                if ( Mesh.CollapseEdge( EV.A, EV.B, .5, CollapseInfo ) == MeshResult::Ok )
                {
                    for ( int i = 0; i < 2; i++ )
                    {
                        if ( AllEIDs.count( CollapseInfo.RemovedEdges[i] ) != 0 )
                        {
                            const int ToAdd = CollapseInfo.KeptEdges[i];
                            if ( AllEIDs.insert( ToAdd ).second )
                            {
                                EIDs.push_back( ToAdd );
                            }
                        }
                    }
                }
            }
        }

        // edges that will become new boundary edges after the boolean op removes triangles on each mesh
        std::array<std::vector<int>, 2> CutBoundaryEdges;
        // vertices on the cut boundary that *may* not have a corresponding vertex on the other mesh
        std::array<std::set<int>, 2> PossUnmatchedBdryVerts;

        // delete geometry according to boolean rules, tracking the boundary edges
        {
            // first decide what triangles to delete for both meshes (*before* deleting anything so winding
            // doesn't get messed up!)
            std::array<std::vector<uint8_t>, 2> KeepTri;
            // double-checks the assumption that we delete the other surface when we keep a coplanar tri; only
            // mesh 0 needs it (the mesh we keep triangles from when we preserve coplanar surfaces)
            std::vector<int32_t> DeleteIfOtherKept;
            if ( NumMeshesToProcess > 1 )
            {
                DeleteIfOtherKept.assign( CutMesh[0]->MaxTriangleID(), -1 );
            }
            for ( int MeshIdx = 0; MeshIdx < NumMeshesToProcess; MeshIdx++ )
            {
                const CustomInsideMeshTest& UseInsideTest = MeshIdx == 0 ? InsideMeshB : InsideMeshA;
                DynamicMeshAABBTree3&       OtherSpatial  = Spatial[1 - MeshIdx];
                if ( !OtherSpatial.IsValid() )
                {
                    OtherSpatial.Build();
                }
                FastWindingTree  LocalWinding( &OtherSpatial, UseInsideTest.bRequiresWinding );
                FastWindingTree* UseWinding = UseInsideTest.bRequiresWinding ? &LocalWinding : nullptr;
                const CustomInsideTestContext InsideTestContext{ &OtherSpatial, UseWinding };

                DynamicMesh3& ProcessMesh = *CutMesh[MeshIdx];
                const int     MaxTriID    = ProcessMesh.MaxTriangleID();
                KeepTri[MeshIdx].assign( MaxTriID, 0 );
                const bool bCoplanarKeepSameDir =
                     ( Operation != BooleanOp::Difference && Operation != BooleanOp::TrimInside &&
                       Operation != BooleanOp::NewGroupInside );
                // whether to remove the inside triangles (e.g. for union) or the outside ones (for intersection)
                bool bRemoveInside = true;
                if ( Operation == BooleanOp::NewGroupOutside || Operation == BooleanOp::TrimOutside ||
                     Operation == BooleanOp::Intersect || ( Operation == BooleanOp::Difference && MeshIdx == 1 ) )
                {
                    bRemoveInside = false;
                }
                MeshNormals OtherNormals( OtherSpatial.GetMesh() );
                OtherNormals.ComputeTriangleNormals();
                const double           OnPlaneTolerance = SnapTolerance;
                const MeshQueryOptions NonDegenCoplanarCandidateFilter(
                     OnPlaneTolerance,
                     // filter degenerate triangles from matching; by convention their normal is the zero vector
                     [&OtherNormals]( int TID ) -> bool { return OtherNormals[TID] != glm::dvec3( 0 ); } );
                const MeshQueryOptions OnOtherMeshQuery( OnPlaneTolerance * 2 );

                for ( int TID = 0; TID < MaxTriID; TID++ )
                {
                    if ( !ProcessMesh.IsTriangle( TID ) )
                    {
                        continue;
                    }
                    std::array<glm::dvec3, 3> Tri;
                    ProcessMesh.GetTriVertices( TID, Tri[0], Tri[1], Tri[2] );
                    const glm::dvec3 Centroid = ( Tri[0] + Tri[1] + Tri[2] ) / 3.0;

                    // first check for the coplanar case
                    double    DSq = 0;
                    const int OtherTID =
                         OtherSpatial.FindNearestTriangle( Centroid, DSq, NonDegenCoplanarCandidateFilter );
                    if ( OtherTID > -1 ) // only consider it coplanar if there is a matching tri
                    {
                        const glm::dvec3 OtherNormal = OtherNormals[OtherTID];
                        const glm::dvec3 Normal      = ProcessMesh.GetTriNormal( TID );
                        const double     DotNormals  = glm::dot( OtherNormal, Normal );
                        // to be extra sure it's a coplanar match, check the vertices are *also* on the other mesh,
                        // with a more forgiving tolerance: they were likely cut right to the coplanar region's
                        // edge
                        bool bAllTrisOnOtherMesh = true;
                        for ( int Idx = 0; Idx < 3; Idx++ )
                        {
                            if ( OtherSpatial.FindNearestTriangle( Tri[Idx], DSq, OnOtherMeshQuery ) ==
                                 DynamicMesh3::InvalidID )
                            {
                                bAllTrisOnOtherMesh = false;
                                break;
                            }
                        }
                        if ( bAllTrisOnOtherMesh )
                        {
                            // coplanar tris favour the first mesh (delete from the other); fully degenerate tris
                            // are deleted too -- with no orientation, it is cracks in solids or spikes in the void
                            if ( MeshIdx != 0 || Normal == glm::dvec3( 0 ) )
                            {
                                KeepTri[MeshIdx][TID] = 0;
                                continue;
                            }
                            const bool bKeep      = ( DotNormals > 0 ) == bCoplanarKeepSameDir;
                            KeepTri[MeshIdx][TID] = bKeep ? 1 : 0;
                            if ( NumMeshesToProcess > 1 && bKeep )
                            {
                                // remember the coplanar pair we expect to be deleted; if it is kept after all
                                // (it wasn't coplanar), delete this one instead -- cleans up slivers near a cut
                                DeleteIfOtherKept[TID] = OtherTID;
                            }
                            continue;
                        }
                    }
                    // not coplanar; use the inside test
                    KeepTri[MeshIdx][TID] =
                         ( UseInsideTest.IsPointInsideFn( Centroid, InsideTestContext ) != bRemoveInside ) ? 1 : 0;
                }
            }

            // don't keep coplanar tris if the matched second-mesh tri we expected to delete was actually kept
            if ( NumMeshesToProcess > 1 )
            {
                for ( int TID : CutMesh[0]->TriangleIndicesItr() )
                {
                    const int32_t DeleteIfOtherKeptTID = DeleteIfOtherKept[TID];
                    if ( DeleteIfOtherKeptTID > -1 && KeepTri[1][DeleteIfOtherKeptTID] )
                    {
                        KeepTri[0][TID] = 0;
                    }
                }
            }

            for ( int MeshIdx = 0; MeshIdx < NumMeshesToProcess; MeshIdx++ )
            {
                DynamicMesh3& ProcessMesh = *CutMesh[MeshIdx];
                for ( int EID : ProcessMesh.EdgeIndicesItr() )
                {
                    const DynamicMesh3::Edge Edge = ProcessMesh.GetEdge( EID );
                    if ( Edge.Tri.B == IndexConstants::InvalidID ||
                         KeepTri[MeshIdx][Edge.Tri.A] == KeepTri[MeshIdx][Edge.Tri.B] )
                    {
                        continue;
                    }
                    CutBoundaryEdges[MeshIdx].push_back( EID );
                    PossUnmatchedBdryVerts[MeshIdx].insert( Edge.Vert.A );
                    PossUnmatchedBdryVerts[MeshIdx].insert( Edge.Vert.B );
                }
            }

            // now go ahead and delete from both meshes
            const bool bRegroupInsteadOfDelete =
                 Operation == BooleanOp::NewGroupInside || Operation == BooleanOp::NewGroupOutside;
            int              NewGroupID = -1;
            std::vector<int> NewGroupTris;
            if ( bRegroupInsteadOfDelete )
            {
                assert( NumMeshesToProcess == 1 );
                NewGroupID = CutMesh[0]->AllocateTriangleGroup();
            }
            for ( int MeshIdx = 0; MeshIdx < NumMeshesToProcess; MeshIdx++ )
            {
                DynamicMesh3& ProcessMesh = *CutMesh[MeshIdx];
                for ( int TID = 0; TID < static_cast<int>( KeepTri[MeshIdx].size() ); TID++ )
                {
                    if ( ProcessMesh.IsTriangle( TID ) && !KeepTri[MeshIdx][TID] )
                    {
                        if ( bRegroupInsteadOfDelete )
                        {
                            ProcessMesh.SetTriangleGroup( TID, NewGroupID );
                            NewGroupTris.push_back( TID );
                        }
                        else
                        {
                            ProcessMesh.RemoveTriangle( TID, true, false );
                        }
                    }
                }
            }
            if ( bRegroupInsteadOfDelete )
            {
                // the new group could include disconnected components; give them separate groups
                MeshConnectedComponents Components( CutMesh[0] );
                Components.FindConnectedTriangles( NewGroupTris );
                for ( int ComponentIdx = 1; ComponentIdx < Components.Num(); ComponentIdx++ )
                {
                    const int SplitGroupID = CutMesh[0]->AllocateTriangleGroup();
                    for ( int TID : Components.GetComponent( ComponentIdx ).Indices )
                    {
                        CutMesh[0]->SetTriangleGroup( TID, SplitGroupID );
                    }
                }
            }
        }

        // correspond vertices across both meshes (in cases where both meshes were processed)
        std::map<int, int> AllVIDMatches; // matched vertex IDs from cutmesh 0 to cutmesh 1
        if ( NumMeshesToProcess == 2 )
        {
            std::array<std::map<int, int>, 2> FoundMatchesMaps; // matched VIDs from mesh 1->0 and mesh 0->1
            const double                      SnapToleranceSq = SnapTolerance * SnapTolerance;

            // ensure segments that are now on boundaries have 1:1 vertex correspondence across meshes
            for ( int MeshIdx = 0; MeshIdx < 2; MeshIdx++ )
            {
                const int     OtherMeshIdx = 1 - MeshIdx;
                DynamicMesh3& OtherMesh    = *CutMesh[OtherMeshIdx];

                PointHashGrid3 OtherMeshPointHash( MaxDim( OtherMesh.GetBounds() ) / 64 );
                for ( int BoundaryVID : PossUnmatchedBdryVerts[OtherMeshIdx] )
                {
                    OtherMeshPointHash.InsertPointUnsafe( BoundaryVID, OtherMesh.GetVertex( BoundaryVID ) );
                }

                // the other mesh's cut-boundary edges whose current bounds touch the query box (UE: an octree)
                std::vector<int>& OtherBoundaryEdges = CutBoundaryEdges[OtherMeshIdx];
                auto EdgesInBox = [&OtherMesh, &OtherBoundaryEdges]( const AxisAlignedBox3d& QueryBox )
                {
                    std::vector<int> EdgesInRange;
                    for ( int EID : OtherBoundaryEdges )
                    {
                        glm::dvec3 A, B;
                        OtherMesh.GetEdgeV( EID, A, B );
                        const AxisAlignedBox3d EdgeBox( glm::min( A, B ), glm::max( A, B ) );
                        if ( MeshAABBTreeDetail::BoxIntersects( EdgeBox, QueryBox ) )
                        {
                            EdgesInRange.push_back( EID );
                        }
                    }
                    return EdgesInRange;
                };

                // OtherMesh VID -> ProcessMesh VID; keeps only the best match where several boundary vertices
                // map to one vertex on the other mesh's boundary
                std::map<int, int>& FoundMatches = FoundMatchesMaps[MeshIdx];
                for ( const int BoundaryVIDIn : PossUnmatchedBdryVerts[MeshIdx] )
                {
                    int BoundaryVID = BoundaryVIDIn;
                    if ( MeshIdx == 1 && FoundMatchesMaps[0].count( BoundaryVID ) != 0 )
                    {
                        continue; // was already snapped to a vertex
                    }

                    glm::dvec3 Pos     = CutMesh[MeshIdx]->GetVertex( BoundaryVID );
                    const auto VIDDist = OtherMeshPointHash.FindNearestInRadius(
                         Pos, SnapTolerance, [&Pos, &OtherMesh]( int VID )
                         { return DistanceSquared( Pos, OtherMesh.GetVertex( VID ) ); } );
                    int    NearestVID = VIDDist.first;  // ID of nearest vertex on other mesh
                    double DSq        = VIDDist.second; // square distance to that vertex

                    if ( NearestVID != DynamicMesh3::InvalidID )
                    {
                        const auto Match = FoundMatches.find( NearestVID );
                        if ( Match != FoundMatches.end() )
                        {
                            const double OldDSq = DistanceSquared( CutMesh[MeshIdx]->GetVertex( Match->second ),
                                                                   OtherMesh.GetVertex( NearestVID ) );
                            if ( DSq < OldDSq ) // new vertex is a better match than the old one
                            {
                                const int OldVID = Match->second;
                                Match->second    = BoundaryVID; // new VID is recorded as best match
                                // the old VID is now the unmatched one; it is matched below
                                BoundaryVID = OldVID;
                                Pos         = CutMesh[MeshIdx]->GetVertex( BoundaryVID );
                                DSq         = OldDSq;
                            }
                            NearestVID = DynamicMesh3::InvalidID; // one of these vertices will be unmatched
                        }
                        else
                        {
                            FoundMatches.emplace( NearestVID, BoundaryVID );
                        }
                    }

                    // no valid match: try to split the nearest edge to create one
                    if ( NearestVID == DynamicMesh3::InvalidID )
                    {
                        const AxisAlignedBox3d QueryBox( Pos - glm::dvec3( SnapTolerance ),
                                                         Pos + glm::dvec3( SnapTolerance ) );
                        const int OtherEID = FindNearestEdge( OtherMesh, EdgesInBox( QueryBox ), Pos );
                        if ( OtherEID != DynamicMesh3::InvalidID )
                        {
                            glm::dvec3 EdgePts[2];
                            OtherMesh.GetEdgeV( OtherEID, EdgePts[0], EdgePts[1] );
                            // only accept the match if it's not going to create a degenerate edge
                            if ( DistanceSquared( EdgePts[0], Pos ) > SnapToleranceSq &&
                                 DistanceSquared( EdgePts[1], Pos ) > SnapToleranceSq )
                            {
                                const Segment3<double>      Seg( EdgePts[0], EdgePts[1] );
                                const double                Along = Seg.ProjectUnitRange( Pos );
                                DynamicMesh3::EdgeSplitInfo SplitInfo;
                                if ( OtherMesh.SplitEdge( OtherEID, SplitInfo, Along ) == MeshResult::Ok )
                                {
                                    FoundMatches[SplitInfo.NewVertex] = BoundaryVID;
                                    OtherMesh.SetVertex( SplitInfo.NewVertex, Pos );
                                    OtherBoundaryEdges.push_back( SplitInfo.NewEdges.A );
                                    // the new vertex is matched by construction: it goes neither into
                                    // PossUnmatchedBdryVerts nor into the point hash
                                }
                            }
                        }
                    }
                }

                // actually snap the positions together for final matches
                for ( const auto& [OtherVID, ProcessVID] : FoundMatches )
                {
                    CutMesh[MeshIdx]->SetVertex( ProcessVID, OtherMesh.GetVertex( OtherVID ) );
                    // AllVIDMatches always maps from CutMesh 0 to 1
                    if ( MeshIdx == 0 )
                    {
                        AllVIDMatches[ProcessVID] = OtherVID;
                    }
                    else
                    {
                        AllVIDMatches[OtherVID] = ProcessVID;
                    }
                }
            }
        }

        if ( Operation == BooleanOp::Difference )
        {
            std::vector<int> AllTID;
            for ( int TID : CutMesh[1]->TriangleIndicesItr() )
            {
                AllTID.push_back( TID );
            }
            DynamicMeshEditor FlipEditor( CutMesh[1] );
            FlipEditor.ReverseTriangleOrientations( AllTID, true );
        }

        bool bSuccess = true;
        CreatedBoundaryEdges.clear();
        if ( NumMeshesToProcess > 1 )
        {
            Result->EnableMatchingAttributes( *CutMesh[1], false );
            DynamicMesh3::AppendInfo AppendOffsets;
            Result->AppendWithOffsets( *CutMesh[1], &AppendOffsets );
            bSuccess = MergeEdges( AppendOffsets, CutMesh, CutBoundaryEdges, AllVIDMatches ) && bSuccess;
        }
        // for NewGroupInside and NewGroupOutside, the cut doesn't create boundary edges
        else if ( Operation != BooleanOp::NewGroupInside && Operation != BooleanOp::NewGroupOutside )
        {
            CreatedBoundaryEdges = CutBoundaryEdges[0];
        }

        // put the result back in the input space
        ScaleTranslate( *Result, 1.0 / ScaleFactor, Center );
        return bSuccess;
    }

    bool MeshBoolean::MergeEdges( const DynamicMesh3::AppendInfo&        AppendOffsets,
                                  std::array<DynamicMesh3*, 2>&          CutMesh,
                                  const std::array<std::vector<int>, 2>& CutBoundaryEdges,
                                  const std::map<int, int>&              AllVIDMatches )
    {
        const auto NewVertex = [&AppendOffsets]( int VID ) { return VID + AppendOffsets.VertexOffset; };

        // translate the edge IDs from CutMesh[1] over to Result mesh edge IDs
        std::vector<int> OtherMeshEdges;
        for ( int OldMeshEID : CutBoundaryEdges[1] )
        {
            if ( !CutMesh[1]->IsEdge( OldMeshEID ) )
            {
                assert( false );
                continue;
            }
            const Index2i OtherEV   = CutMesh[1]->GetEdgeV( OldMeshEID );
            const int     MappedEID = Result->FindEdge( NewVertex( OtherEV.A ), NewVertex( OtherEV.B ) );
            if ( Result->IsBoundaryEdge( MappedEID ) )
            {
                OtherMeshEdges.push_back( MappedEID );
            }
        }

        // find "easy" match candidates using the already-made vertex correspondence
        std::vector<Index2i> CandidateMatches;
        std::vector<int>     UnmatchedEdges;
        for ( int EID : CutBoundaryEdges[0] )
        {
            if ( !Result->IsBoundaryEdge( EID ) )
            {
                continue;
            }
            const Index2i VIDs            = Result->GetEdgeV( EID );
            const auto    OtherA          = AllVIDMatches.find( VIDs.A );
            const auto    OtherB          = AllVIDMatches.find( VIDs.B );
            bool          bAddedCandidate = false;
            if ( OtherA != AllVIDMatches.end() && OtherB != AllVIDMatches.end() )
            {
                const int OtherEID = Result->FindEdge( NewVertex( OtherA->second ), NewVertex( OtherB->second ) );
                if ( OtherEID != DynamicMesh3::InvalidID )
                {
                    CandidateMatches.emplace_back( EID, OtherEID );
                    bAddedCandidate = true;
                }
            }
            if ( !bAddedCandidate )
            {
                UnmatchedEdges.push_back( EID );
            }
        }

        // merge the easy matches
        for ( const Index2i& Candidate : CandidateMatches )
        {
            if ( !Result->IsEdge( Candidate.A ) || !Result->IsBoundaryEdge( Candidate.A ) )
            {
                continue;
            }
            DynamicMesh3::MergeEdgesInfo MergeInfo;
            if ( Result->MergeEdges( Candidate.A, Candidate.B, MergeInfo, true ) != MeshResult::Ok )
            {
                UnmatchedEdges.push_back( Candidate.A );
            }
        }

        // filter matched edges from the edge array for the other mesh
        const auto IsOpen = [this]( int EID ) { return Result->IsEdge( EID ) && Result->IsBoundaryEdge( EID ); };
        OtherMeshEdges.erase( std::remove_if( OtherMeshEdges.begin(), OtherMeshEdges.end(),
                                              [&IsOpen]( int EID ) { return !IsOpen( EID ); } ),
                              OtherMeshEdges.end() );

        // see if we can match anything else
        bool bAllMatched = true;
        if ( !UnmatchedEdges.empty() )
        {
            // greedily match within snap tolerance
            const double SnapToleranceSq = SnapTolerance * SnapTolerance;
            for ( int OtherEID : OtherMeshEdges )
            {
                if ( !IsOpen( OtherEID ) )
                {
                    continue;
                }
                glm::dvec3 OA, OB;
                Result->GetEdgeV( OtherEID, OA, OB );
                for ( size_t UnmatchedIdx = 0; UnmatchedIdx < UnmatchedEdges.size(); UnmatchedIdx++ )
                {
                    const int EID = UnmatchedEdges[UnmatchedIdx];
                    if ( !IsOpen( EID ) )
                    {
                        // RemoveAtSwap
                        UnmatchedEdges[UnmatchedIdx] = UnmatchedEdges.back();
                        UnmatchedEdges.pop_back();
                        UnmatchedIdx--;
                        continue;
                    }
                    glm::dvec3 A, B;
                    Result->GetEdgeV( EID, A, B );
                    if ( DistanceSquared( OA, A ) < SnapToleranceSq && DistanceSquared( OB, B ) < SnapToleranceSq )
                    {
                        DynamicMesh3::MergeEdgesInfo MergeInfo;
                        if ( Result->MergeEdges( EID, OtherEID, MergeInfo, true ) == MeshResult::Ok )
                        {
                            UnmatchedEdges[UnmatchedIdx] = UnmatchedEdges.back();
                            UnmatchedEdges.pop_back();
                            break;
                        }
                    }
                }
            }
            // store the failure cases from the first mesh's array
            for ( int EID : UnmatchedEdges )
            {
                if ( IsOpen( EID ) )
                {
                    CreatedBoundaryEdges.push_back( EID );
                    bAllMatched = false;
                }
            }
        }
        // store the failure cases from the second mesh's array
        for ( int OtherEID : OtherMeshEdges )
        {
            if ( IsOpen( OtherEID ) )
            {
                CreatedBoundaryEdges.push_back( OtherEID );
                bAllMatched = false;
            }
        }
        return bAllMatched;
    }

    int MeshBoolean::FindNearestEdge( const DynamicMesh3& OnMesh, const std::vector<int>& EIDs,
                                      const glm::dvec3& Pos ) const
    {
        int        NearEID = DynamicMesh3::InvalidID;
        double     NearSqr = SnapTolerance * SnapTolerance;
        glm::dvec3 EdgePts[2];
        for ( int EID : EIDs )
        {
            OnMesh.GetEdgeV( EID, EdgePts[0], EdgePts[1] );
            const Segment3<double> Seg( EdgePts[0], EdgePts[1] );
            const double           DSqr = Seg.DistanceSquared( Pos );
            if ( DSqr < NearSqr )
            {
                NearEID = EID;
                NearSqr = DSqr;
            }
        }
        return NearEID;
    }
} // namespace Desert::Geometry
