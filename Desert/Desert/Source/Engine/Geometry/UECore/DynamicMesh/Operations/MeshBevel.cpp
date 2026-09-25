// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/MeshBevel.cpp:75-131, 669-1172,
// adapted: see MeshBevel.hpp. Algo::CountIf / Algo::Reverse are written out, the progress-cancel checks are gone
// (UECore has no FProgressCancel), and every UE path that leaves a vertex Unknown records why.
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MeshBevel.hpp"

#include "Engine/Geometry/UECore/DynamicMesh/MeshIndexUtil.hpp"
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
} // namespace Desert::Geometry
