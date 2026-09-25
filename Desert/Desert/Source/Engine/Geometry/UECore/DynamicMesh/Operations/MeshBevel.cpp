// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/MeshBevel.cpp:75-131, 669-2082,
// adapted: see MeshBevel.hpp. Algo::CountIf / Algo::Reverse are written out, the progress-cancel checks are gone
// (UECore has no FProgressCancel), and every UE path that leaves a vertex Unknown records why.
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MeshBevel.hpp"

#include "Engine/Geometry/UECore/CompGeom/PolygonTriangulation.hpp"
#include "Engine/Geometry/UECore/Distance/DistLine3Line3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshIndexUtil.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/PolyEditingEdgeUtil.hpp"
#include "Engine/Geometry/UECore/MathUtil.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

#include <algorithm>

namespace Desert::Geometry
{
    namespace
    {
        bool AnyBoundaryEdge( const FDynamicMesh3& Mesh, const TArray<int>& EdgeList )
        {
            for ( const int EdgeID : EdgeList )
            {
                if ( Mesh.IsBoundaryEdge( EdgeID ) )
                    return true;
            }
            return false;
        }
    } // namespace

    void FMeshBevel::Refuse( const std::string& Reason )
    {
        if ( FailureReason.empty() )
            FailureReason = "FMeshBevel: " + Reason;
    }

    bool FMeshBevel::RefuseBowties( const FDynamicMesh3& Mesh, const TArray<int32>& MeshVertices )
    {
        for ( const int32 VertexID : MeshVertices )
        {
            if ( Mesh.IsBowtieVertex( VertexID ) )
            {
                // UE splits bowties first (FixBowties, B:415-574); SplitBowties is not ported.
                Refuse( "vertex " + std::to_string( VertexID ) +
                        " on the bevel edges is a bowtie (two separate triangle fans); split it first" );
                return true;
            }
        }
        return false;
    }

    bool FMeshBevel::InitializeFromGroupTopology( const FDynamicMesh3& Mesh, const FGroupTopology& Topology )
    {
        FailureReason.clear();
        for ( int32 TopoEdgeID = 0; TopoEdgeID < Topology.Edges.Num(); ++TopoEdgeID )
        {
            if ( Topology.IsIsolatedLoop( TopoEdgeID ) )
            {
                FEdgeLoop NewLoop;
                NewLoop.InitializeFromEdges( Mesh, Topology.Edges[TopoEdgeID].Span.Edges );
                AddBevelEdgeLoop( Mesh, NewLoop );
            }
            else
            {
                AddBevelGroupEdge( Mesh, Topology, TopoEdgeID );
            }
        }
        if ( !FailureReason.empty() )
            return false;
        BuildVertexSets( Mesh );
        return FailureReason.empty();
    }

    bool FMeshBevel::InitializeFromGroupTopologyEdges( const FDynamicMesh3& Mesh, const FGroupTopology& Topology,
                                                       const TArray<int32>& GroupEdges )
    {
        FailureReason.clear();
        for ( const int32 TopoEdgeID : GroupEdges )
        {
            if ( TopoEdgeID < 0 || TopoEdgeID >= Topology.Edges.Num() )
            {
                Refuse( "group edge " + std::to_string( TopoEdgeID ) + " is not in the topology (" +
                        std::to_string( Topology.Edges.Num() ) + " edges)" );
                return false;
            }
            // The all-edges initializer skips mesh-border group edges as UE does (a border has one side to cut);
            // an edge the caller NAMED is refused instead, so the request is never silently shrunk.
            if ( AnyBoundaryEdge( Mesh, Topology.Edges[TopoEdgeID].Span.Edges ) )
            {
                Refuse( "group edge " + std::to_string( TopoEdgeID ) +
                        " lies on the mesh border and has no second side to bevel" );
                return false;
            }
            if ( Topology.IsIsolatedLoop( TopoEdgeID ) )
            {
                FEdgeLoop NewLoop;
                NewLoop.InitializeFromEdges( Mesh, Topology.Edges[TopoEdgeID].Span.Edges );
                AddBevelEdgeLoop( Mesh, NewLoop );
            }
            else
            {
                AddBevelGroupEdge( Mesh, Topology, TopoEdgeID );
            }
        }
        if ( !FailureReason.empty() )
            return false;
        BuildVertexSets( Mesh );
        return FailureReason.empty();
    }

    FMeshBevel::FBevelVertex* FMeshBevel::GetBevelVertexFromVertexID( int32 VertexID, int32* IndexOut )
    {
        int32* FoundIndex = VertexIDToIndexMap.Find( VertexID );
        if ( FoundIndex == nullptr )
            return nullptr;
        if ( IndexOut != nullptr )
            *IndexOut = *FoundIndex;
        return &Vertices[*FoundIndex];
    }

    void FMeshBevel::AddBevelGroupEdge( const FDynamicMesh3& Mesh, const FGroupTopology& Topology,
                                        int32 GroupEdgeID )
    {
        const TArray<int32>& MeshEdgeList = Topology.Edges[GroupEdgeID].Span.Edges;

        // cannot bevel an edge on the mesh boundary
        if ( AnyBoundaryEdge( Mesh, MeshEdgeList ) )
            return;
        if ( RefuseBowties( Mesh, Topology.Edges[GroupEdgeID].Span.Vertices ) )
            return;

        const FIndex2i EdgeCornerIDs = Topology.Edges[GroupEdgeID].EndpointCorners;

        FBevelEdge  Edge;
        const int32 NewBevelEdgeIndex = Edges.Num();

        // Find the mesh vertices at either end of the group edge; create a new bevel vertex or add to an existing
        // one.
        for ( int32 ci = 0; ci < 2; ++ci )
        {
            const int32 CornerID           = EdgeCornerIDs[ci];
            const int32 VertexID           = Topology.Corners[CornerID].VertexID;
            Edge.bEndpointBoundaryFlag[ci] = Mesh.IsBoundaryVertex( VertexID );
            const int32   IncomingEdgeID   = ( ci == 0 ) ? MeshEdgeList[0] : MeshEdgeList.Last();
            int32         BevelVertexIndex = -1;
            FBevelVertex* VertInfo         = GetBevelVertexFromVertexID( VertexID, &BevelVertexIndex );
            if ( VertInfo == nullptr )
            {
                FBevelVertex NewVertex;
                NewVertex.VertexID = VertexID;
                BevelVertexIndex   = Vertices.Num();
                Vertices.Add( NewVertex );
                VertexIDToIndexMap.Add( VertexID, BevelVertexIndex );
                VertInfo = &Vertices[BevelVertexIndex];
            }
            VertInfo->IncomingBevelMeshEdges.Add( IncomingEdgeID );
            VertInfo->IncomingBevelEdgeIndices.Add( NewBevelEdgeIndex );
            Edge.BevelVertices[ci] = BevelVertexIndex;
        }

        // save the edge span
        Edge.MeshEdges.Append( MeshEdgeList );
        Edge.MeshVertices.Append( Topology.Edges[GroupEdgeID].Span.Vertices );

        Edge.MeshEdgeTris.Reserve( Edge.MeshEdges.Num() );
        for ( const int32 eid : Edge.MeshEdges )
            Edge.MeshEdgeTris.Add( Mesh.GetEdgeT( eid ) );

        Edge.InitialPositions.Reserve( Edge.MeshVertices.Num() );
        for ( const int32 vid : Edge.MeshVertices )
            Edge.InitialPositions.Add( Mesh.GetVertex( vid ) );

        Edge.EdgeIndex = Edges.Num();
        Edges.Add( MoveTemp( Edge ) );
    }

    void FMeshBevel::AddBevelEdgeLoop( const FDynamicMesh3& Mesh, const FEdgeLoop& MeshEdgeLoop )
    {
        // cannot bevel an edge on the mesh boundary
        if ( AnyBoundaryEdge( Mesh, MeshEdgeLoop.Edges ) )
            return;
        if ( RefuseBowties( Mesh, MeshEdgeLoop.Vertices ) )
            return;

        FBevelLoop Loop;
        Loop.MeshEdges    = MeshEdgeLoop.Edges;
        Loop.MeshVertices = MeshEdgeLoop.Vertices;

        Loop.MeshEdgeTris.Reserve( Loop.MeshEdges.Num() );
        for ( const int32 eid : Loop.MeshEdges )
            Loop.MeshEdgeTris.Add( Mesh.GetEdgeT( eid ) );

        Loop.InitialPositions.Reserve( Loop.MeshVertices.Num() );
        for ( const int32 vid : Loop.MeshVertices )
            Loop.InitialPositions.Add( Mesh.GetVertex( vid ) );

        Loops.Add( Loop );
    }

    void FMeshBevel::InitVertexSet( const FDynamicMesh3& Mesh, FBevelVertex& Vertex )
    {
        // collect up the triangle one-ring around the vertex, as a sequential list
        TArray<int>       GroupLengths;
        TArray<bool>      bGroupIsLoop;
        const EMeshResult Result =
             Mesh.GetVtxContiguousTriangles( Vertex.VertexID, Vertex.SortedTriangles, GroupLengths, bGroupIsLoop );
        if ( Result != EMeshResult::Ok || GroupLengths.Num() != 1 || Vertex.SortedTriangles.Num() < 2 )
        {
            Vertex.VertexType = EBevelVertexType::Unknown;
            Refuse( "vertex " + std::to_string( Vertex.VertexID ) + " has " +
                    std::to_string( GroupLengths.Num() ) + " triangle fans and " +
                    std::to_string( Vertex.SortedTriangles.Num() ) +
                    " triangles; a bevel vertex needs one fan of at least two" );
            return;
        }

        // orient the one-ring the same way for every vertex (the wedge walks below depend on it)
        const FIndex3i Tri0 = Mesh.GetTriangle( Vertex.SortedTriangles[0] ).GetCycled( Vertex.VertexID );
        const FIndex3i Tri1 = Mesh.GetTriangle( Vertex.SortedTriangles[1] ).GetCycled( Vertex.VertexID );
        if ( Tri0.C == Tri1.B )
            std::reverse( Vertex.SortedTriangles.begin(), Vertex.SortedTriangles.end() );

        // A boundary vertex keeps UE's BoundaryVertex type: its edge ends at the border and needs no vertex
        // meshing.
        if ( Mesh.IsBoundaryVertex( Vertex.VertexID ) )
        {
            Vertex.VertexType = EBevelVertexType::BoundaryVertex;
            return;
        }

        if ( Vertex.IncomingBevelMeshEdges.Num() == 1 )
            BuildTerminatorVertex( Vertex, Mesh );
        else
            BuildJunctionVertex( Vertex, Mesh );
    }

    void FMeshBevel::FinalizeTerminatorVertex( const FDynamicMesh3& Mesh, FBevelVertex& Vertex )
    {
        // Two terminators joined by the edge each would split along need that shared edge meshed as one quad.
        if ( Vertex.VertexType != EBevelVertexType::TerminatorVertex )
            return;
        const int32  OtherVertexID    = Vertex.TerminatorInfo.B;
        const int32* OtherBevelVtxIdx = VertexIDToIndexMap.Find( OtherVertexID );
        if ( OtherBevelVtxIdx == nullptr )
            return;
        const FBevelVertex& OtherVertex = Vertices[*OtherBevelVtxIdx];
        if ( OtherVertex.VertexType != EBevelVertexType::TerminatorVertex )
            return;
        const int32 MeshEdgeID = Mesh.FindEdge( Vertex.VertexID, OtherVertex.VertexID );
        if ( Mesh.IsEdge( MeshEdgeID ) && Vertex.TerminatorInfo.A == MeshEdgeID &&
             OtherVertex.TerminatorInfo.A == MeshEdgeID && !Vertex.IncomingBevelMeshEdges.Contains( MeshEdgeID ) )
        {
            Vertex.ConnectedBevelVertex = *OtherBevelVtxIdx;
        }
    }

    void FMeshBevel::BuildVertexSets( const FDynamicMesh3& Mesh )
    {
        // can be parallel
        for ( FBevelVertex& Vertex : Vertices )
            InitVertexSet( Mesh, Vertex );

        // resolve terminator connections only once every vertex has its type
        for ( FBevelVertex& Vertex : Vertices )
            FinalizeTerminatorVertex( Mesh, Vertex );
    }

    void FMeshBevel::BuildJunctionVertex( FBevelVertex& Vertex, const FDynamicMesh3& Mesh )
    {
        // Split the sorted one-ring into wedges between the incoming bevel edges. The wedges become separate
        // vertices when the vertex is unlinked, and each is displaced along its two border edges.
        const int32 NT            = Vertex.SortedTriangles.Num();
        int32       StartTriIndex = -1;
        for ( int32 k = 0; k < NT; ++k )
        {
            if ( FindSharedEdgeInTriangles( Mesh, Vertex.SortedTriangles[k],
                                            Vertex.SortedTriangles[( k + 1 ) % NT] ) ==
                 Vertex.IncomingBevelMeshEdges[0] )
            {
                StartTriIndex = ( k + 1 ) % NT; // start at second tri, so that bevel-edge is first edge in wedge
                break;
            }
        }
        if ( StartTriIndex == -1 )
        {
            Vertex.VertexType = EBevelVertexType::Unknown;
            Refuse( "junction vertex " + std::to_string( Vertex.VertexID ) + ": incoming bevel edge " +
                    std::to_string( Vertex.IncomingBevelMeshEdges[0] ) + " is not between two of its triangles" );
            return;
        }

        int32         CurTriIndex = StartTriIndex;
        FOneRingWedge CurWedge;
        CurWedge.WedgeVertex = Vertex.VertexID;
        CurWedge.Triangles.Add( Vertex.SortedTriangles[CurTriIndex] );
        CurWedge.BorderEdges.A = Vertex.IncomingBevelMeshEdges[0];
        for ( int32 k = 0; k < NT; ++k )
        {
            const int32 CurTri     = Vertex.SortedTriangles[CurTriIndex % NT];
            const int32 NextTri    = Vertex.SortedTriangles[( CurTriIndex + 1 ) % NT];
            const int32 SharedEdge = FindSharedEdgeInTriangles( Mesh, CurTri, NextTri );
            if ( Vertex.IncomingBevelMeshEdges.Contains( SharedEdge ) )
            {
                CurWedge.BorderEdges.B = SharedEdge;
                Vertex.Wedges.Add( CurWedge );
                CurWedge               = FOneRingWedge();
                CurWedge.WedgeVertex   = Vertex.VertexID;
                CurWedge.BorderEdges.A = SharedEdge;
            }
            CurWedge.Triangles.Add( NextTri );
            CurTriIndex++;
        }

        for ( FOneRingWedge& Wedge : Vertex.Wedges )
        {
            Wedge.BorderEdgeTriEdgeIndices.A =
                 Mesh.GetTriEdges( Wedge.Triangles[0] ).IndexOf( Wedge.BorderEdges.A );
            Wedge.BorderEdgeTriEdgeIndices.B =
                 Mesh.GetTriEdges( Wedge.Triangles.Last() ).IndexOf( Wedge.BorderEdges.B );
        }

        if ( Vertex.Wedges.Num() > 1 )
        {
            Vertex.VertexType = EBevelVertexType::JunctionVertex;
        }
        else
        {
            Vertex.VertexType = EBevelVertexType::Unknown;
            Refuse( "junction vertex " + std::to_string( Vertex.VertexID ) + " splits into " +
                    std::to_string( Vertex.Wedges.Num() ) +
                    " wedge(s); its incoming bevel edges need at least two" );
        }
    }

    void FMeshBevel::BuildTerminatorVertex( FBevelVertex& Vertex, const FDynamicMesh3& Mesh )
    {
        // A terminator's single incoming edge cannot split the one-ring in two by itself: a second, "ring split"
        // edge is chosen so the vertex can open into an edge, and the hole it leaves is filled later.
        Vertex.VertexType       = EBevelVertexType::Unknown;
        const std::string Where = "terminator vertex " + std::to_string( Vertex.VertexID );

        const int32    IncomingEdgeID  = Vertex.IncomingBevelMeshEdges[0];
        int32          RingSplitEdgeID = -1;
        const FIndex2i IncomingEdgeT   = Mesh.GetEdgeT( IncomingEdgeID );
        {
            const FIndex2i IncomingEdgeGroups( Mesh.GetTriangleGroup( IncomingEdgeT.A ),
                                               Mesh.GetTriangleGroup( IncomingEdgeT.B ) );

            // Start at a triangle of one of the incoming groups, so the other-group triangles are contiguous.
            const int32 NumTriangles = Vertex.SortedTriangles.Num();
            int32       StartIndex   = 0;
            for ( int32 k = 0; k < NumTriangles; ++k )
            {
                if ( IncomingEdgeGroups.Contains( Mesh.GetTriangleGroup( Vertex.SortedTriangles[k] ) ) )
                {
                    StartIndex = k;
                    break;
                }
            }

            TArray<int32> OtherGroupTris; // sorted wedge of triangles in neither group of the incoming edge
            TArray<int32> OtherGroups;    // group IDs encountered, in order
            for ( int32 k = 0; k < NumTriangles; ++k )
            {
                const int32 tid = Vertex.SortedTriangles[( StartIndex + k ) % NumTriangles];
                const int32 gid = Mesh.GetTriangleGroup( tid );
                if ( !IncomingEdgeGroups.Contains( gid ) )
                {
                    OtherGroupTris.Add( tid );
                    OtherGroups.AddUnique( gid );
                }
            }
            const int32 NumRemainingTris = OtherGroupTris.Num();

            if ( OtherGroups.Num() == 0 )
            {
                // No other group: split along the one-ring edge best aligned with the incoming edge direction.
                const int32 OtherVID = Mesh.GetEdgeV( IncomingEdgeID ).OtherElement( Vertex.VertexID );
                if ( OtherVID == IndexConstants::InvalidID )
                {
                    Refuse( Where + ": incoming edge " + std::to_string( IncomingEdgeID ) + " does not touch it" );
                    return;
                }
                const FVector3d CenterPos   = Mesh.GetVertex( Vertex.VertexID );
                FVector3d       IncomingDir = CenterPos - Mesh.GetVertex( OtherVID );
                Normalize( IncomingDir );
                double BestAlignmentScore = -FMathd::MaxReal;
                Mesh.EnumerateVertexEdges( Vertex.VertexID,
                                           [&]( int32 EID )
                                           {
                                               if ( EID == IncomingEdgeID )
                                                   return;
                                               const int32 OutVID =
                                                    Mesh.GetEdgeV( EID ).OtherElement( Vertex.VertexID );
                                               if ( OutVID == IndexConstants::InvalidID )
                                                   return;
                                               FVector3d OutgoingDir = Mesh.GetVertex( OutVID ) - CenterPos;
                                               Normalize( OutgoingDir );
                                               const double AlignmentScore = OutgoingDir.Dot( IncomingDir );
                                               if ( AlignmentScore > BestAlignmentScore )
                                               {
                                                   RingSplitEdgeID    = EID;
                                                   BestAlignmentScore = AlignmentScore;
                                               }
                                           } );
            }
            else if ( OtherGroups.Num() == 1 )
            {
                // Exactly one other group: split inside it, and the end-cap triangle JOINS that group (B:1061).
                Vertex.NewGroupID = OtherGroups[0];
                if ( OtherGroupTris.Num() == 1 )
                {
                    const FIndex3i TriEdges = Mesh.GetTriEdges( OtherGroupTris[0] );
                    for ( int32 j = 0; j < 3; ++j )
                    {
                        if ( Mesh.GetEdgeV( TriEdges[j] ).Contains( Vertex.VertexID ) )
                        {
                            RingSplitEdgeID = TriEdges[j];
                            break;
                        }
                    }
                }
                else if ( OtherGroupTris.Num() == 2 )
                {
                    RingSplitEdgeID = FindSharedEdgeInTriangles( Mesh, OtherGroupTris[0], OtherGroupTris[1] );
                }
                else
                {
                    const int32 j   = OtherGroupTris.Num() / 2;
                    RingSplitEdgeID = FindSharedEdgeInTriangles( Mesh, OtherGroupTris[j], OtherGroupTris[j + 1] );
                    if ( RingSplitEdgeID == -1 )
                    {
                        for ( int32 k = 0; k < NumRemainingTris; ++k )
                        {
                            RingSplitEdgeID = FindSharedEdgeInTriangles(
                                 Mesh, OtherGroupTris[k], OtherGroupTris[( k + 1 ) % NumRemainingTris] );
                            if ( RingSplitEdgeID != -1 )
                                break;
                        }
                    }
                }
            }
            else
            {
                // Several other groups: split along the first edge between two of them; the cap gets a new group.
                for ( int32 k = 0; k < OtherGroupTris.Num(); ++k )
                {
                    const int32 TriangleA = OtherGroupTris[k];
                    const int32 TriangleB = OtherGroupTris[( k + 1 ) % NumRemainingTris];
                    if ( Mesh.GetTriangleGroup( TriangleA ) != Mesh.GetTriangleGroup( TriangleB ) )
                    {
                        RingSplitEdgeID   = FindSharedEdgeInTriangles( Mesh, TriangleA, TriangleB );
                        Vertex.NewGroupID = -1;
                        break;
                    }
                }
            }
        }

        if ( RingSplitEdgeID == -1 )
        {
            Refuse( Where + ": no one-ring edge to split along opposite incoming edge " +
                    std::to_string( IncomingEdgeID ) );
            return;
        }

        const FIndex2i SplitEdgeV = Mesh.GetEdgeV( RingSplitEdgeID );
        Vertex.TerminatorInfo     = FIndex2i( RingSplitEdgeID, SplitEdgeV.OtherElement( Vertex.VertexID ) );

        TArray<int32> SplitTriSets[2];
        if ( !SplitInteriorVertexTrianglesIntoSubsets( &Mesh, Vertex.VertexID, IncomingEdgeID, RingSplitEdgeID,
                                                       SplitTriSets[0], SplitTriSets[1] ) )
        {
            Refuse( Where + ": edges " + std::to_string( IncomingEdgeID ) + " and " +
                    std::to_string( RingSplitEdgeID ) + " do not split its one-ring in two" );
            return;
        }

        Vertex.Wedges.SetNum( 2 );
        Vertex.Wedges[0].WedgeVertex = Vertex.VertexID;
        Vertex.Wedges[0].Triangles.Append( SplitTriSets[0] );
        Vertex.Wedges[1].WedgeVertex = Vertex.VertexID;
        Vertex.Wedges[1].Triangles.Append( SplitTriSets[1] );

        // Border edges of each wedge are the edges of its end triangles that touch the vertex and no wedge
        // neighbour.
        for ( FOneRingWedge& Wedge : Vertex.Wedges )
        {
            const int32    NumWedgeTris = Wedge.Triangles.Num();
            const FIndex2i VtxEdges0    = FindVertexEdgesInTriangle( Mesh, Wedge.Triangles[0], Vertex.VertexID );
            if ( NumWedgeTris == 1 )
            {
                Wedge.BorderEdges.A = VtxEdges0.A;
                Wedge.BorderEdges.B = VtxEdges0.B;
            }
            else
            {
                Wedge.BorderEdges.A =
                     Mesh.GetEdgeT( VtxEdges0.A ).Contains( Wedge.Triangles[1] ) ? VtxEdges0.B : VtxEdges0.A;
                const FIndex2i VtxEdges1 =
                     FindVertexEdgesInTriangle( Mesh, Wedge.Triangles[NumWedgeTris - 1], Vertex.VertexID );
                Wedge.BorderEdges.B = Mesh.GetEdgeT( VtxEdges1.A ).Contains( Wedge.Triangles[NumWedgeTris - 2] )
                                           ? VtxEdges1.B
                                           : VtxEdges1.A;
            }
            Wedge.BorderEdgeTriEdgeIndices.A =
                 Mesh.GetTriEdges( Wedge.Triangles[0] ).IndexOf( Wedge.BorderEdges.A );
            Wedge.BorderEdgeTriEdgeIndices.B =
                 Mesh.GetTriEdges( Wedge.Triangles.Last() ).IndexOf( Wedge.BorderEdges.B );
        }

        Vertex.VertexType = EBevelVertexType::TerminatorVertex;
    }

    // ---- Unlink (B:1174-1602): open the bevel edges into pairs of boundary edges ----

    void FMeshBevel::UnlinkEdges( FDynamicMesh3& Mesh )
    {
        for ( FBevelEdge& Edge : Edges )
            UnlinkBevelEdgeInterior( Mesh, Edge );
    }

    namespace
    {
        struct FVertexSplit
        {
            int32         VertexID = -1;
            bool          bOK      = false;
            TArray<int32> TriSets[2];
        };

        // The subset functions pick Set0 arbitrarily per vertex; make Set0 the same side of the span at every
        // vertex by requiring it to share a triangle with the previous vertex's Set0.
        void ReconcileTriangleSets( TArray<FVertexSplit>& SplitSequence )
        {
            const int32   N = SplitSequence.Num();
            TArray<int32> PrevTriSet0;
            for ( int32 k = 0; k < N; ++k )
            {
                if ( PrevTriSet0.Num() == 0 && SplitSequence[k].TriSets[0].Num() > 0 )
                {
                    PrevTriSet0 = SplitSequence[k].TriSets[0];
                }
                else
                {
                    bool bFoundInSet0 = false;
                    for ( const int32 tid : SplitSequence[k].TriSets[0] )
                    {
                        if ( PrevTriSet0.Contains( tid ) )
                        {
                            bFoundInSet0 = true;
                            break;
                        }
                    }
                    if ( !bFoundInSet0 )
                        Swap( SplitSequence[k].TriSets[0], SplitSequence[k].TriSets[1] );
                    PrevTriSet0 = SplitSequence[k].TriSets[0];
                }
            }
        }
    } // namespace

    bool FMeshBevel::SplitOrKeep( FDynamicMesh3& Mesh, int32 VertexID, const TArray<int32>& Triangles,
                                  const std::string& Where, int32& NewVertexOut )
    {
        FDynamicMesh3::FVertexSplitInfo SplitInfo;
        const EMeshResult               Result = Mesh.SplitVertex( VertexID, Triangles, SplitInfo );
        if ( Result == EMeshResult::Ok )
        {
            NewVertexOut = SplitInfo.NewVertex;
            return true;
        }
        // UE only ensure()s here and keeps the vertex shared, which later meshes a degenerate strip.
        Refuse( Where + ": SplitVertex(" + std::to_string( VertexID ) + ", " + std::to_string( Triangles.Num() ) +
                " triangles) failed with EMeshResult " + std::to_string( static_cast<int>( Result ) ) );
        NewVertexOut = VertexID;
        return false;
    }

    void FMeshBevel::UnlinkBevelEdgeInterior( FDynamicMesh3& Mesh, FBevelEdge& BevelEdge )
    {
        const int32          N = BevelEdge.MeshVertices.Num();
        TArray<FVertexSplit> SplitsToProcess;
        SplitsToProcess.SetNum( N );
        const std::string Where = "bevel edge " + std::to_string( BevelEdge.EdgeIndex );

        // Endpoints are split here only on the mesh boundary; otherwise UnlinkVertices owns them.
        const int32 EndVertex[2] = { 0, N - 1 };
        const int32 EndEdge[2]   = { 0, N - 2 };
        for ( int32 j = 0; j < 2; ++j )
        {
            FVertexSplit& Split = SplitsToProcess[EndVertex[j]];
            Split.VertexID      = BevelEdge.MeshVertices[EndVertex[j]];
            if ( !BevelEdge.bEndpointBoundaryFlag[j] )
                continue;
            const int32 SplitEdge = BevelEdge.MeshEdges[EndEdge[j]];
            Split.bOK             = SplitBoundaryVertexTrianglesIntoSubsets( &Mesh, Split.VertexID, SplitEdge,
                                                                             Split.TriSets[0], Split.TriSets[1] );
            if ( !Split.bOK )
                Refuse( Where + ": boundary end vertex " + std::to_string( Split.VertexID ) +
                        " cannot be split along edge " + std::to_string( SplitEdge ) );
        }
        for ( int32 k = 1; k < N - 1; ++k )
        {
            FVertexSplit& Split = SplitsToProcess[k];
            Split.VertexID      = BevelEdge.MeshVertices[k];
            // A span vertex on the mesh boundary stays shared by both sides (UE): the strip pinches there.
            if ( Mesh.IsBoundaryVertex( Split.VertexID ) )
                continue;
            Split.bOK = SplitInteriorVertexTrianglesIntoSubsets( &Mesh, Split.VertexID, BevelEdge.MeshEdges[k - 1],
                                                                 BevelEdge.MeshEdges[k], Split.TriSets[0],
                                                                 Split.TriSets[1] );
            if ( !Split.bOK )
                Refuse( Where + ": vertex " + std::to_string( Split.VertexID ) + " cannot be split along edges " +
                        std::to_string( BevelEdge.MeshEdges[k - 1] ) + " and " +
                        std::to_string( BevelEdge.MeshEdges[k] ) );
        }

        ReconcileTriangleSets( SplitsToProcess );

        for ( const FVertexSplit& Split : SplitsToProcess )
        {
            int32 NewVertex = Split.VertexID; // unsplit: the same vertex on both sides
            if ( Split.bOK )
                SplitOrKeep( Mesh, Split.VertexID, Split.TriSets[0], Where, NewVertex );
            BevelEdge.NewMeshVertices.Add( NewVertex );
        }

        for ( int32 k = 0; k < N - 1; ++k )
        {
            const int32 Edge0 = BevelEdge.MeshEdges[k];
            const int32 Edge1 = Mesh.FindEdge( BevelEdge.NewMeshVertices[k], BevelEdge.NewMeshVertices[k + 1] );
            BevelEdge.NewMeshEdges.Add( Edge1 );
            if ( Mesh.IsEdge( Edge1 ) && Edge0 != Edge1 && !MeshEdgePairs.Contains( Edge0 ) )
            {
                MeshEdgePairs.Add( Edge0, Edge1 );
                MeshEdgePairs.Add( Edge1, Edge0 );
            }
        }
    }

    void FMeshBevel::UnlinkBevelLoop( FDynamicMesh3& Mesh, FBevelLoop& BevelLoop )
    {
        const int32          N = BevelLoop.MeshVertices.Num();
        TArray<FVertexSplit> SplitsToProcess;
        SplitsToProcess.SetNum( N );
        for ( int32 k = 0; k < N; ++k )
        {
            FVertexSplit& Split = SplitsToProcess[k];
            Split.VertexID      = BevelLoop.MeshVertices[k];
            if ( Mesh.IsBoundaryVertex( Split.VertexID ) )
                continue; // shared by both sides, as on an edge span
            const int32 PrevEdge = ( k == 0 ) ? BevelLoop.MeshEdges.Last() : BevelLoop.MeshEdges[k - 1];
            const int32 CurEdge  = BevelLoop.MeshEdges[k];
            Split.bOK = SplitInteriorVertexTrianglesIntoSubsets( &Mesh, Split.VertexID, PrevEdge, CurEdge,
                                                                 Split.TriSets[0], Split.TriSets[1] );
            if ( !Split.bOK )
                Refuse( "bevel loop vertex " + std::to_string( Split.VertexID ) + " cannot be split along edges " +
                        std::to_string( PrevEdge ) + " and " + std::to_string( CurEdge ) );
        }

        ReconcileTriangleSets( SplitsToProcess );

        // Loops move TriSets[1] where edge spans move TriSets[0]; kept as in UE.
        for ( const FVertexSplit& Split : SplitsToProcess )
        {
            int32 NewVertex = Split.VertexID;
            if ( Split.bOK )
                SplitOrKeep( Mesh, Split.VertexID, Split.TriSets[1], "bevel loop", NewVertex );
            BevelLoop.NewMeshVertices.Add( NewVertex );
        }

        for ( int32 k = 0; k < N; ++k )
        {
            const int32 Edge0 = BevelLoop.MeshEdges[k];
            const int32 Edge1 =
                 Mesh.FindEdge( BevelLoop.NewMeshVertices[k], BevelLoop.NewMeshVertices[( k + 1 ) % N] );
            BevelLoop.NewMeshEdges.Add( Edge1 );
            if ( Mesh.IsEdge( Edge1 ) && Edge0 != Edge1 && !MeshEdgePairs.Contains( Edge0 ) )
            {
                MeshEdgePairs.Add( Edge0, Edge1 );
                MeshEdgePairs.Add( Edge1, Edge0 );
            }
        }
    }

    void FMeshBevel::UnlinkLoops( FDynamicMesh3& Mesh )
    {
        for ( FBevelLoop& Loop : Loops )
            UnlinkBevelLoop( Mesh, Loop );
    }

    void FMeshBevel::UnlinkVertices( FDynamicMesh3& Mesh )
    {
        // All terminators before any junction, as in UE.
        for ( FBevelVertex& Vertex : Vertices )
        {
            if ( Vertex.VertexType == EBevelVertexType::TerminatorVertex )
                UnlinkTerminatorVertex( Mesh, Vertex );
        }
        for ( FBevelVertex& Vertex : Vertices )
        {
            if ( Vertex.VertexType == EBevelVertexType::JunctionVertex )
                UnlinkJunctionVertex( Mesh, Vertex );
        }
    }

    void FMeshBevel::PairSplitWedgeBorderEdges( const FDynamicMesh3& Mesh, FBevelVertex& Vertex )
    {
        // A border edge the split duplicated has a new ID at the same index of the wedge's own end triangle.
        for ( FOneRingWedge& Wedge : Vertex.Wedges )
        {
            for ( int32 j = 0; j < 2; ++j )
            {
                const int32 OldWedgeEdgeID    = Wedge.BorderEdges[j];
                const int32 OldWedgeEdgeIndex = Wedge.BorderEdgeTriEdgeIndices[j];
                const int32 TriangleID        = ( j == 0 ) ? Wedge.Triangles[0] : Wedge.Triangles.Last();
                const int32 CurWedgeEdgeID    = Mesh.GetTriEdges( TriangleID )[OldWedgeEdgeIndex];
                if ( OldWedgeEdgeID == CurWedgeEdgeID )
                    continue;
                if ( !MeshEdgePairs.Contains( OldWedgeEdgeID ) )
                {
                    MeshEdgePairs.Add( OldWedgeEdgeID, CurWedgeEdgeID );
                    MeshEdgePairs.Add( CurWedgeEdgeID, OldWedgeEdgeID );
                }
                Wedge.BorderEdges[j] = CurWedgeEdgeID;
            }
        }
    }

    void FMeshBevel::UnlinkJunctionVertex( FDynamicMesh3& Mesh, FBevelVertex& Vertex )
    {
        // Wedge 0 keeps the original vertex; every other wedge gets its own copy.
        const std::string Where = "junction vertex " + std::to_string( Vertex.VertexID );
        for ( int32 k = 1; k < Vertex.Wedges.Num(); ++k )
        {
            FOneRingWedge& Wedge = Vertex.Wedges[k];
            SplitOrKeep( Mesh, Vertex.VertexID, Wedge.Triangles, Where, Wedge.WedgeVertex );
        }
        PairSplitWedgeBorderEdges( Mesh, Vertex );
    }

    void FMeshBevel::UnlinkTerminatorVertex( FDynamicMesh3& Mesh, FBevelVertex& BevelVertex )
    {
        const std::string Where = "terminator vertex " + std::to_string( BevelVertex.VertexID );
        if ( SplitOrKeep( Mesh, BevelVertex.VertexID, BevelVertex.Wedges[1].Triangles, Where,
                          BevelVertex.Wedges[1].WedgeVertex ) )
            PairSplitWedgeBorderEdges( Mesh, BevelVertex );
    }

    void FMeshBevel::FixUpUnlinkedBevelEdges( const FDynamicMesh3& Mesh )
    {
        for ( FBevelEdge& Edge : Edges )
        {
            // A sub-edge whose "new" edge is still the old one (always so for a single-edge span, whose ends are
            // split by the vertex unlinks) takes its partner from the pairs those unlinks recorded.
            const bool bSingleEdge        = Edge.MeshEdges.Num() == 1;
            bool       bFailedEdgePairing = false;
            for ( int32 Idx = 0; Idx < Edge.MeshEdges.Num(); ++Idx )
            {
                const bool bNewEdgeIsOld = Edge.MeshEdges[Idx] == Edge.NewMeshEdges[Idx];
                if ( !bSingleEdge && !bNewEdgeIsOld )
                    continue;
                const int32* FoundOtherEdge = MeshEdgePairs.Find( Edge.MeshEdges[Idx] );
                if ( FoundOtherEdge == nullptr )
                {
                    Refuse( "bevel edge " + std::to_string( Edge.EdgeIndex ) + ": mesh edge " +
                            std::to_string( Edge.MeshEdges[Idx] ) + " was not split into a pair" );
                    bFailedEdgePairing = true;
                    break;
                }
                Edge.NewMeshEdges[Idx] = *FoundOtherEdge;
            }
            if ( bFailedEdgePairing )
                continue;

            // Re-point the span's end vertices at the wedge vertices that now own its end edges.
            for ( int32 j = 0; j < 2; ++j )
            {
                const int32         vi          = ( j == 0 ) ? 0 : ( Edge.MeshVertices.Num() - 1 );
                const int32         ei          = ( j == 0 ) ? 0 : ( Edge.MeshEdges.Num() - 1 );
                const FBevelVertex* BevelVertex = GetBevelVertexFromVertexID( Edge.MeshVertices[vi] );
                if ( BevelVertex == nullptr )
                {
                    Refuse( "bevel edge " + std::to_string( Edge.EdgeIndex ) + ": end vertex " +
                            std::to_string( Edge.MeshVertices[vi] ) + " is not a bevel vertex" );
                    break;
                }
                int32&      V0       = Edge.MeshVertices[vi];
                int32&      V1       = Edge.NewMeshVertices[vi];
                const int32 E0       = Edge.MeshEdges[ei];
                const int32 E1       = Edge.NewMeshEdges[ei];
                bool        bFoundV0 = false;
                bool        bFoundV1 = false;
                for ( const FOneRingWedge& Wedge : BevelVertex->Wedges )
                {
                    for ( const int32 tid : Wedge.Triangles )
                    {
                        const FIndex3i TriEdges = Mesh.GetTriEdges( tid );
                        if ( TriEdges.Contains( E0 ) && !bFoundV0 )
                        {
                            V0       = Wedge.WedgeVertex;
                            bFoundV0 = true;
                            break;
                        }
                        if ( TriEdges.Contains( E1 ) && !bFoundV1 )
                        {
                            V1       = Wedge.WedgeVertex;
                            bFoundV1 = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    void FMeshBevel::DisplaceVertices( FDynamicMesh3& Mesh )
    {
        // Inset every beveled edge into its faces the way FInsetMeshRegion does: an 'inset line' per mesh edge,
        // each vertex at the nearest points of its pair of lines (their intersection when the face is planar).
        // Open spans keep their line sets, because the corner vertices combine the end lines of several spans.
        struct FEdgePathInsetLines
        {
            TArray<FLine3d> InsetLines0;
            TArray<FLine3d> InsetLines1;
        };
        TArray<FEdgePathInsetLines> AllInsetLines;
        AllInsetLines.SetNum( Edges.Num() );

        for ( int32 k = 0; k < Edges.Num(); ++k )
        {
            FBevelEdge& Edge = Edges[k];
            ComputeInsetLineSegmentsFromEdges( Mesh, Edge.MeshEdges, InsetDistance, AllInsetLines[k].InsetLines0 );
            SolveInsetVertexPositionsFromInsetLines( Mesh, AllInsetLines[k].InsetLines0, Edge.MeshVertices,
                                                     Edge.NewPositions0, false );
            ComputeInsetLineSegmentsFromEdges( Mesh, Edge.NewMeshEdges, InsetDistance,
                                               AllInsetLines[k].InsetLines1 );
            SolveInsetVertexPositionsFromInsetLines( Mesh, AllInsetLines[k].InsetLines1, Edge.NewMeshVertices,
                                                     Edge.NewPositions1, false );
        }

        for ( FBevelLoop& Loop : Loops )
        {
            TArray<FLine3d> InsetLines;
            ComputeInsetLineSegmentsFromEdges( Mesh, Loop.MeshEdges, InsetDistance, InsetLines );
            SolveInsetVertexPositionsFromInsetLines( Mesh, InsetLines, Loop.MeshVertices, Loop.NewPositions0,
                                                     true );
            ComputeInsetLineSegmentsFromEdges( Mesh, Loop.NewMeshEdges, InsetDistance, InsetLines );
            SolveInsetVertexPositionsFromInsetLines( Mesh, InsetLines, Loop.NewMeshVertices, Loop.NewPositions1,
                                                     true );
        }

        // Corners: each wedge vertex solves against the end inset lines of the bevel edges leaving it.
        for ( FBevelVertex& Vertex : Vertices )
        {
            if ( Vertex.VertexType == EBevelVertexType::Unknown )
                continue;
            for ( FOneRingWedge& Wedge : Vertex.Wedges )
            {
                const FVector3d CurPos = Mesh.GetVertex( Wedge.WedgeVertex );

                TArray<FLine3d> SolveLines;
                for ( const int32 j : Vertex.IncomingBevelEdgeIndices )
                {
                    if ( Edges[j].MeshVertices[0] == Wedge.WedgeVertex )
                        SolveLines.Add( AllInsetLines[j].InsetLines0[0] );
                    else if ( Edges[j].MeshVertices.Last() == Wedge.WedgeVertex )
                        SolveLines.Add( AllInsetLines[j].InsetLines0.Last() );
                    else if ( Edges[j].NewMeshVertices[0] == Wedge.WedgeVertex )
                        SolveLines.Add( AllInsetLines[j].InsetLines1[0] );
                    else if ( Edges[j].NewMeshVertices.Last() == Wedge.WedgeVertex )
                        SolveLines.Add( AllInsetLines[j].InsetLines1.Last() );
                }

                const std::string Where = "bevel vertex " + std::to_string( Vertex.VertexID ) + ", wedge vertex " +
                                          std::to_string( Wedge.WedgeVertex );
                // BoundaryVertex never gets here: its wedges are not built (UE's bIsSimpleBoundary is dead).
                if ( Vertex.VertexType == EBevelVertexType::TerminatorVertex )
                {
                    // UE silently leaves the vertex in place here (its ensure is commented out as "hit in Lyra").
                    if ( SolveLines.Num() != 1 )
                    {
                        Refuse( Where + ": terminator wedge touches " + std::to_string( SolveLines.Num() ) +
                                " bevel edge ends, expected 1" );
                        continue;
                    }
                    // Nearest point on the inset line can drift off the face the terminating edge runs into, so
                    // slide along the wedge mesh edge best aligned with that inset direction instead (UE's own
                    // stop-gap for "which topology edge should this vertex slide along").
                    const FVector3d InsetLinePosition = SolveLines[0].NearestPoint( CurPos );
                    Wedge.NewPosition                 = InsetLinePosition;
                    const FVector3d BaseInsetDir      = Normalized( InsetLinePosition - CurPos );
                    double          MaxDot            = -1;
                    FLine3d         MaxDotEdgeLine;
                    Mesh.EnumerateVertexVertices( Wedge.WedgeVertex,
                                                  [&]( int32 othervid )
                                                  {
                                                      const FLine3d EdgeLine = FLine3d::FromPoints(
                                                           CurPos, Mesh.GetVertex( othervid ) );
                                                      const double DirDot = EdgeLine.Direction.Dot( BaseInsetDir );
                                                      if ( DirDot > MaxDot )
                                                      {
                                                          MaxDot         = DirDot;
                                                          MaxDotEdgeLine = EdgeLine;
                                                      }
                                                  } );
                    if ( MaxDot > -1 )
                    {
                        FDistLine3Line3d LineIntersection( SolveLines[0], MaxDotEdgeLine );
                        LineIntersection.Get();
                        Wedge.NewPosition = LineIntersection.Line2ClosestPoint;
                    }
                    Wedge.bHaveNewPosition = true;
                }
                else
                {
                    if ( SolveLines.Num() < 2 )
                    {
                        Refuse( Where + ": junction wedge touches " + std::to_string( SolveLines.Num() ) +
                                " bevel edge ends, expected 2" );
                        continue;
                    }
                    Wedge.NewPosition =
                         SolveInsetVertexPositionFromLinePair( CurPos, SolveLines[0], SolveLines[1] );
                    Wedge.bHaveNewPosition = true;
                }
            }
        }

        // Bake. A span's end vertices belong to the corner solve above unless the end is a mesh boundary.
        auto SetDisplacedPositions = [&Mesh]( const TArray<int32>&     VerticesIn,
                                              const TArray<FVector3d>& PositionsIn, int32 InsetStart,
                                              int32 InsetEnd )
        {
            const int32 NumVertices = VerticesIn.Num();
            if ( PositionsIn.Num() != NumVertices )
                return;
            const int32 Stop = NumVertices - InsetEnd;
            for ( int32 k = InsetStart; k < Stop; ++k )
                Mesh.SetVertex( VerticesIn[k], PositionsIn[k] );
        };
        for ( const FBevelEdge& Edge : Edges )
        {
            const int32 InsetStart = Edge.bEndpointBoundaryFlag[0] ? 0 : 1;
            const int32 InsetEnd   = Edge.bEndpointBoundaryFlag[1] ? 0 : 1;
            SetDisplacedPositions( Edge.MeshVertices, Edge.NewPositions0, InsetStart, InsetEnd );
            SetDisplacedPositions( Edge.NewMeshVertices, Edge.NewPositions1, InsetStart, InsetEnd );
        }
        for ( const FBevelLoop& Loop : Loops )
        {
            SetDisplacedPositions( Loop.MeshVertices, Loop.NewPositions0, 0, 0 );
            SetDisplacedPositions( Loop.NewMeshVertices, Loop.NewPositions1, 0, 0 );
        }
        for ( const FBevelVertex& Vertex : Vertices )
        {
            for ( const FOneRingWedge& Wedge : Vertex.Wedges )
            {
                if ( Wedge.bHaveNewPosition )
                    Mesh.SetVertex( Wedge.WedgeVertex, Wedge.NewPosition );
            }
        }
    }

    int32 FMeshBevel::AppendOrRefuse( FDynamicMesh3& Mesh, int32 A, int32 B, int32 C, int32 GroupID,
                                      const std::string& Where )
    {
        const int32 TriangleID = Mesh.AppendTriangle( A, B, C, GroupID );
        if ( !Mesh.IsTriangle( TriangleID ) )
            Refuse( Where + ": triangle (" + std::to_string( A ) + ", " + std::to_string( B ) + ", " +
                    std::to_string( C ) + ") cannot be appended, AppendTriangle returned " +
                    std::to_string( TriangleID ) );
        return TriangleID;
    }

    void FMeshBevel::AppendJunctionVertexPolygon( FDynamicMesh3& Mesh, FBevelVertex& Vertex )
    {
        // UnlinkJunctionVertex() split the junction vertex into one vertex per (now disconnected) wedge. The
        // wedges are ordered so that their wedge vertices form a polygon with correct winding: mesh it as it
        // stands.
        TArray<FVector3d> PolygonPoints;
        for ( const FOneRingWedge& Wedge : Vertex.Wedges )
            PolygonPoints.Add( Mesh.GetVertex( Wedge.WedgeVertex ) );
        TArray<FIndex3i> Triangles;
        PolygonTriangulation::TriangulateSimplePolygon<double>( PolygonPoints, Triangles );
        if ( Triangles.Num() != PolygonPoints.Num() - 2 )
        {
            Refuse( "junction vertex " + std::to_string( Vertex.VertexID ) + ": its " +
                    std::to_string( PolygonPoints.Num() ) + "-gon triangulates into " +
                    std::to_string( Triangles.Num() ) + " triangles" );
            return;
        }
        Vertex.NewGroupID       = Mesh.AllocateTriangleGroup();
        const std::string Where = "junction vertex " + std::to_string( Vertex.VertexID );
        for ( const FIndex3i& Tri : Triangles )
        {
            const int32 TriangleID =
                 AppendOrRefuse( Mesh, Vertex.Wedges[Tri.A].WedgeVertex, Vertex.Wedges[Tri.B].WedgeVertex,
                                 Vertex.Wedges[Tri.C].WedgeVertex, Vertex.NewGroupID, Where );
            if ( Mesh.IsTriangle( TriangleID ) )
                Vertex.NewTriangles.Add( TriangleID );
        }
    }

    void FMeshBevel::AppendTerminatorVertexTriangle( FDynamicMesh3& Mesh, FBevelVertex& Vertex )
    {
        // UnlinkTerminatorVertex() opened a triangle-shaped hole next to the incoming quad strip. The wedges hold
        // the two vertices of the strip's end edge; the third is the far end of the ring-split edge, looked up
        // again because unlinking other vertices may have replaced the FarVertexID stored in TerminatorInfo.
        const std::string Where           = "terminator vertex " + std::to_string( Vertex.VertexID );
        const int32       RingSplitEdgeID = Vertex.TerminatorInfo.A;
        if ( !Mesh.IsEdge( RingSplitEdgeID ) )
        {
            Refuse( Where + ": ring-split edge " + std::to_string( RingSplitEdgeID ) + " no longer exists" );
            return;
        }
        const int32 FarVertexID = Mesh.GetEdgeV( RingSplitEdgeID ).OtherElement( Vertex.VertexID );
        const int32 QuadEdgeID  = Mesh.FindEdge( Vertex.Wedges[0].WedgeVertex, Vertex.Wedges[1].WedgeVertex );
        if ( !Mesh.IsEdge( QuadEdgeID ) || !Mesh.IsBoundaryEdge( QuadEdgeID ) )
        {
            Refuse( Where + ": wedge vertices " + std::to_string( Vertex.Wedges[0].WedgeVertex ) + " and " +
                    std::to_string( Vertex.Wedges[1].WedgeVertex ) + " share no open edge (edge " +
                    std::to_string( QuadEdgeID ) + ")" );
            return;
        }
        const FIndex2i QuadEdgeV = Mesh.GetOrientedBoundaryEdgeV( QuadEdgeID );
        // BuildTerminatorVertex gives the cap the group of the face it closes (B:1061), or -1 for a new group.
        const int32 UseGroupID = ( Vertex.NewGroupID >= 0 ) ? Vertex.NewGroupID : Mesh.AllocateTriangleGroup();
        const int32 TriangleID = AppendOrRefuse( Mesh, QuadEdgeV.B, QuadEdgeV.A, FarVertexID, UseGroupID, Where );
        if ( Mesh.IsTriangle( TriangleID ) )
            Vertex.NewTriangles.Add( TriangleID );
    }

    void FMeshBevel::AppendTerminatorVertexPairQuad( FDynamicMesh3& Mesh, FBevelVertex& Vertex0,
                                                     FBevelVertex& Vertex1 )
    {
        // Two terminators joined directly by the non-beveled ring-split edge both opened their side, so the hole
        // is a quad with a strip end edge at each end; the wedges alone give its corners.
        const std::string Where =
             "terminator pair " + std::to_string( Vertex0.VertexID ) + "/" + std::to_string( Vertex1.VertexID );
        const int32 QuadEdgeID0 = Mesh.FindEdge( Vertex0.Wedges[0].WedgeVertex, Vertex0.Wedges[1].WedgeVertex );
        const int32 QuadEdgeID1 = Mesh.FindEdge( Vertex1.Wedges[0].WedgeVertex, Vertex1.Wedges[1].WedgeVertex );
        if ( !Mesh.IsEdge( QuadEdgeID0 ) || !Mesh.IsEdge( QuadEdgeID1 ) || !Mesh.IsBoundaryEdge( QuadEdgeID0 ) ||
             !Mesh.IsBoundaryEdge( QuadEdgeID1 ) )
        {
            Refuse( Where + ": strip end edges " + std::to_string( QuadEdgeID0 ) + " and " +
                    std::to_string( QuadEdgeID1 ) + " are not both open" );
            return;
        }
        const FIndex2i QuadEdgeV0 = Mesh.GetOrientedBoundaryEdgeV( QuadEdgeID0 );
        const FIndex2i QuadEdgeV1 = Mesh.GetOrientedBoundaryEdgeV( QuadEdgeID1 );
        if ( Mesh.FindEdge( QuadEdgeV0.A, QuadEdgeV1.B ) == IndexConstants::InvalidID ||
             Mesh.FindEdge( QuadEdgeV0.B, QuadEdgeV1.A ) == IndexConstants::InvalidID )
        {
            Refuse( Where + ": the quad hole's connecting edges are missing" );
            return;
        }
        const int32 UseGroupID = ( Vertex0.NewGroupID >= 0 ) ? Vertex0.NewGroupID : Mesh.AllocateTriangleGroup();
        // quad order is V0.B, V0.A, V1.B, V1.A
        const int32 TriangleID0 =
             AppendOrRefuse( Mesh, QuadEdgeV0.B, QuadEdgeV0.A, QuadEdgeV1.B, UseGroupID, Where );
        if ( Mesh.IsTriangle( TriangleID0 ) )
            Vertex0.NewTriangles.Add( TriangleID0 );
        const int32 TriangleID1 =
             AppendOrRefuse( Mesh, QuadEdgeV0.B, QuadEdgeV1.B, QuadEdgeV1.A, UseGroupID, Where );
        if ( Mesh.IsTriangle( TriangleID1 ) )
            Vertex1.NewTriangles.Add( TriangleID1 );
    }

    void FMeshBevel::AppendEdgeQuads( FDynamicMesh3& Mesh, FBevelEdge& Edge )
    {
        const std::string Where    = "bevel edge " + std::to_string( Edge.EdgeIndex );
        const int32       NumEdges = Edge.MeshEdges.Num();
        if ( NumEdges != Edge.NewMeshEdges.Num() )
        {
            Refuse( Where + ": " + std::to_string( NumEdges ) + " mesh edges but " +
                    std::to_string( Edge.NewMeshEdges.Num() ) + " unlinked partners" );
            return;
        }
        Edge.NewGroupID = Mesh.AllocateTriangleGroup();
        // Each span is fully disconnected into edge pairs by now; join each pair with a quad.
        for ( int32 k = 0; k < NumEdges; ++k )
        {
            const int32 EdgeID0 = Edge.MeshEdges[k];
            int32       EdgeID1 = Edge.NewMeshEdges[k];
            // A single-edge span only gets its partner when the junction vertex is unlinked; .NewMeshEdges is not
            // updated then, but MeshEdgePairs is.
            if ( EdgeID0 == EdgeID1 )
            {
                if ( const int32* FoundEdgeID1 = MeshEdgePairs.Find( EdgeID0 ) )
                    EdgeID1 = *FoundEdgeID1;
            }
            FIndex2i QuadTris( IndexConstants::InvalidID, IndexConstants::InvalidID );
            if ( EdgeID0 == EdgeID1 || !Mesh.IsEdge( EdgeID1 ) )
            {
                Refuse( Where + ": mesh edge " + std::to_string( EdgeID0 ) + " has no unlinked partner (" +
                        std::to_string( EdgeID1 ) + ")" );
                Edge.StripQuads.Add( QuadTris );
                continue;
            }
            const FIndex2i EdgeV0 = Mesh.GetOrientedBoundaryEdgeV( EdgeID0 );
            const FIndex2i EdgeV1 = Mesh.GetOrientedBoundaryEdgeV( EdgeID1 );
            if ( EdgeV0.Contains( EdgeV1.A ) || EdgeV0.Contains( EdgeV1.B ) )
            {
                // The pair still shares one end, so only a triangle fits between them (UE-157531 hits this in
                // complex geometry scripts).
                const int32 OtherV = EdgeV0.Contains( EdgeV1.A ) ? EdgeV1.B : EdgeV1.A;
                QuadTris.A         = AppendOrRefuse( Mesh, EdgeV0.B, EdgeV0.A, OtherV, Edge.NewGroupID, Where );
            }
            else
            {
                QuadTris.A = AppendOrRefuse( Mesh, EdgeV0.B, EdgeV0.A, EdgeV1.B, Edge.NewGroupID, Where );
                QuadTris.B = AppendOrRefuse( Mesh, EdgeV1.B, EdgeV1.A, EdgeV0.B, Edge.NewGroupID, Where );
            }
            Edge.StripQuads.Add( QuadTris );
        }
    }

    void FMeshBevel::AppendLoopQuads( FDynamicMesh3& Mesh, FBevelLoop& Loop )
    {
        const int32 NumEdges = Loop.MeshEdges.Num();
        if ( NumEdges != Loop.NewMeshEdges.Num() )
        {
            Refuse( "bevel loop: " + std::to_string( NumEdges ) + " mesh edges but " +
                    std::to_string( Loop.NewMeshEdges.Num() ) + " unlinked partners" );
            return;
        }
        // One new group per pair of input groups the loop runs between.
        auto GetGroupKey = [&Mesh, &Loop]( int32 k )
        {
            const FIndex2i EdgeTris = Loop.MeshEdgeTris[k];
            const int32    Group0   = Mesh.GetTriangleGroup( EdgeTris.A );
            const int32    Group1   = Mesh.IsTriangle( EdgeTris.B ) ? Mesh.GetTriangleGroup( EdgeTris.B ) : -1;
            return FIndex2i( std::max( Group0, Group1 ), std::min( Group0, Group1 ) );
        };
        TMap<FIndex2i, int32> NewGroupIDs;
        for ( int32 k = 0; k < NumEdges; ++k )
        {
            const FIndex2i GroupKey = GetGroupKey( k );
            if ( !NewGroupIDs.Contains( GroupKey ) )
                Loop.NewGroupIDs.Add( NewGroupIDs.Add( GroupKey, Mesh.AllocateTriangleGroup() ) );
        }
        for ( int32 k = 0; k < NumEdges; ++k )
        {
            const std::string Where   = "bevel loop edge " + std::to_string( Loop.MeshEdges[k] );
            const int32       EdgeID0 = Loop.MeshEdges[k];
            const int32       EdgeID1 = Loop.NewMeshEdges[k];
            FIndex2i          QuadTris( IndexConstants::InvalidID, IndexConstants::InvalidID );
            if ( EdgeID0 == EdgeID1 || !Mesh.IsEdge( EdgeID1 ) )
            {
                Refuse( Where + ": no unlinked partner (" + std::to_string( EdgeID1 ) + ")" );
                Loop.StripQuads.Add( QuadTris );
                continue;
            }
            const int32    NewGroupID = NewGroupIDs[GetGroupKey( k )];
            const FIndex2i EdgeV0     = Mesh.GetOrientedBoundaryEdgeV( EdgeID0 );
            const FIndex2i EdgeV1     = Mesh.GetOrientedBoundaryEdgeV( EdgeID1 );
            QuadTris.A                = AppendOrRefuse( Mesh, EdgeV0.B, EdgeV0.A, EdgeV1.B, NewGroupID, Where );
            if ( EdgeV1.Contains( EdgeV0.B ) )
                Refuse( Where + ": the pair still shares vertex " + std::to_string( EdgeV0.B ) );
            else
                QuadTris.B = AppendOrRefuse( Mesh, EdgeV1.B, EdgeV1.A, EdgeV0.B, NewGroupID, Where );
            Loop.StripQuads.Add( QuadTris );
        }
    }

    void FMeshBevel::CreateBevelMeshing( FDynamicMesh3& Mesh )
    {
        for ( FBevelVertex& Vertex : Vertices )
        {
            if ( Vertex.VertexType == EBevelVertexType::JunctionVertex && Vertex.Wedges.Num() > 2 )
                AppendJunctionVertexPolygon( Mesh, Vertex );
        }
        for ( FBevelEdge& Edge : Edges )
            AppendEdgeQuads( Mesh, Edge );
        for ( FBevelLoop& Loop : Loops )
            AppendLoopQuads( Mesh, Loop );
        // Terminators last: the strip's end edge now exists and orients their triangle.
        TSet<FIndex2i> HandledQuadVtxPairs;
        for ( FBevelVertex& Vertex : Vertices )
        {
            if ( Vertex.VertexType != EBevelVertexType::TerminatorVertex )
                continue;
            if ( Vertex.ConnectedBevelVertex >= 0 )
            {
                FBevelVertex& OtherVertex = Vertices[Vertex.ConnectedBevelVertex];
                FIndex2i      VtxPair( Vertex.VertexID, OtherVertex.VertexID );
                VtxPair.Sort();
                if ( !HandledQuadVtxPairs.Contains( VtxPair ) )
                {
                    AppendTerminatorVertexPairQuad( Mesh, Vertex, OtherVertex );
                    HandledQuadVtxPairs.Add( VtxPair );
                }
            }
            else
            {
                AppendTerminatorVertexTriangle( Mesh, Vertex );
            }
        }
    }
} // namespace Desert::Geometry
