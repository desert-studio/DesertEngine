// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/Operations/MeshBevel.cpp:47-131, 576-2082,
// 3740-3812, 3814-3969, adapted: see MeshBevel.hpp. Algo::CountIf / Algo::Reverse are written out, the
// progress-cancel checks are gone (UECore has no FProgressCancel), and every UE path that leaves a vertex Unknown
// records why. ComputeMaterialIDs fixes three UE slips, see there.
#include "Engine/Geometry/UECore/DynamicMesh/Operations/MeshBevel.hpp"
#include "Engine/Geometry/UECore/MapLookup.hpp"

#include "Engine/Geometry/UECore/CompGeom/PolygonTriangulation.hpp"
#include "Engine/Geometry/UECore/Distance/DistLine3Line3.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshIndexUtil.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/MeshNormals.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/PolyEditingEdgeUtil.hpp"
#include "Engine/Geometry/UECore/FrameTypes.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Operations/PolyEditingUVUtil.hpp"
#include "Engine/Geometry/UECore/DynamicMesh/Parameterization/DynamicMeshUVEditor.hpp"
#include "Engine/Geometry/UECore/MathUtil.hpp"
#include "Engine/Geometry/UECore/MeshBoundaryLoops.hpp"
#include "Engine/Geometry/UECore/VectorTypes.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <string>

namespace Desert::Geometry
{
    namespace
    {
        bool AnyBoundaryEdge( const DynamicMesh3& Mesh, const std::vector<int>& EdgeList )
        {
            return std::ranges::any_of( EdgeList,
                                        [&Mesh]( const int EdgeID ) { return Mesh.IsBoundaryEdge( EdgeID ); } );
        }

        void QuadsToTris( const DynamicMesh3& Mesh, const std::vector<Index2i>& Quads,
                          std::vector<int32_t>& TrisOut, bool bReset )
        {
            if ( bReset )
                TrisOut.clear();
            for ( const Index2i& Quad : Quads )
            {
                if ( Mesh.IsTriangle( Quad.A ) )
                    TrisOut.push_back( Quad.A );
                if ( Mesh.IsTriangle( Quad.B ) )
                    TrisOut.push_back( Quad.B );
            }
        }
    } // namespace

    void MeshBevel::Refuse( const std::string& Reason )
    {
        if ( m_FailureReason.empty() )
            m_FailureReason = "MeshBevel: " + Reason;
    }

    bool MeshBevel::RefuseBowties( const DynamicMesh3& Mesh, const std::vector<int32_t>& MeshVertices )
    {
        const auto Bowtie = std::ranges::find_if( MeshVertices, [&Mesh]( const int32_t VertexID )
                                                  { return Mesh.IsBowtieVertex( VertexID ); } );
        if ( Bowtie == MeshVertices.end() )
            return false;
        // UE splits bowties first (FixBowties, B:415-574); SplitBowties is not ported.
        Refuse( "vertex " + std::to_string( *Bowtie ) +
                " on the bevel edges is a bowtie (two separate triangle fans); split it first" );
        return true;
    }

    bool MeshBevel::InitializeFromGroupTopology( const DynamicMesh3& Mesh, const GroupTopology& Topology )
    {
        m_FailureReason.clear();
        for ( int32_t TopoEdgeID = 0; TopoEdgeID < static_cast<int32_t>( Topology.m_Edges.size() ); ++TopoEdgeID )
        {
            if ( Topology.IsIsolatedLoop( TopoEdgeID ) )
            {
                EdgeLoop NewLoop;
                NewLoop.InitializeFromEdges( Mesh, Topology.m_Edges[TopoEdgeID].Span.Edges );
                AddBevelEdgeLoop( Mesh, NewLoop );
            }
            else
            {
                AddBevelGroupEdge( Mesh, Topology, TopoEdgeID );
            }
        }
        if ( !m_FailureReason.empty() )
            return false;
        BuildVertexSets( Mesh );
        return m_FailureReason.empty();
    }

    bool MeshBevel::InitializeFromGroupTopologyEdges( const DynamicMesh3& Mesh, const GroupTopology& Topology,
                                                      const std::vector<int32_t>& GroupEdges )
    {
        m_FailureReason.clear();
        for ( const int32_t TopoEdgeID : GroupEdges )
        {
            if ( TopoEdgeID < 0 || TopoEdgeID >= static_cast<int32_t>( Topology.m_Edges.size() ) )
            {
                Refuse( "group edge " + std::to_string( TopoEdgeID ) + " is not in the topology (" +
                        std::to_string( static_cast<int32_t>( Topology.m_Edges.size() ) ) + " edges)" );
                return false;
            }
            // The all-edges initializer skips mesh-border group edges as UE does (a border has one side to cut);
            // an edge the caller NAMED is refused instead, so the request is never silently shrunk.
            if ( AnyBoundaryEdge( Mesh, Topology.m_Edges[TopoEdgeID].Span.Edges ) )
            {
                Refuse( "group edge " + std::to_string( TopoEdgeID ) +
                        " lies on the mesh border and has no second side to bevel" );
                return false;
            }
            if ( Topology.IsIsolatedLoop( TopoEdgeID ) )
            {
                EdgeLoop NewLoop;
                NewLoop.InitializeFromEdges( Mesh, Topology.m_Edges[TopoEdgeID].Span.Edges );
                AddBevelEdgeLoop( Mesh, NewLoop );
            }
            else
            {
                AddBevelGroupEdge( Mesh, Topology, TopoEdgeID );
            }
        }
        if ( !m_FailureReason.empty() )
            return false;
        BuildVertexSets( Mesh );
        return m_FailureReason.empty();
    }

    MeshBevel::BevelVertex* MeshBevel::GetBevelVertexFromVertexID( int32_t VertexID, int32_t* IndexOut )
    {
        int32_t* FoundIndex = FindValue( m_VertexIDToIndexMap, VertexID );
        if ( FoundIndex == nullptr )
            return nullptr;
        if ( IndexOut != nullptr )
            *IndexOut = *FoundIndex;
        return &m_Vertices[*FoundIndex];
    }

    void MeshBevel::AddBevelGroupEdge( const DynamicMesh3& Mesh, const GroupTopology& Topology,
                                       int32_t GroupEdgeID )
    {
        const std::vector<int32_t>& MeshEdgeList = Topology.m_Edges[GroupEdgeID].Span.Edges;

        // cannot bevel an edge on the mesh boundary
        if ( AnyBoundaryEdge( Mesh, MeshEdgeList ) )
            return;
        if ( RefuseBowties( Mesh, Topology.m_Edges[GroupEdgeID].Span.Vertices ) )
            return;

        const Index2i EdgeCornerIDs = Topology.m_Edges[GroupEdgeID].EndpointCorners;

        BevelEdge     Edge;
        const int32_t NewBevelEdgeIndex = static_cast<int32_t>( m_Edges.size() );

        // Find the mesh vertices at either end of the group edge; create a new bevel vertex or add to an existing
        // one.
        for ( int32_t ci = 0; ci < 2; ++ci )
        {
            const int32_t CornerID         = EdgeCornerIDs[ci];
            const int32_t VertexID         = Topology.m_Corners[CornerID].VertexID;
            Edge.bEndpointBoundaryFlag[ci] = Mesh.IsBoundaryVertex( VertexID );
            const int32_t IncomingEdgeID   = ( ci == 0 ) ? MeshEdgeList[0] : MeshEdgeList.back();
            int32_t       BevelVertexIndex = -1;
            BevelVertex*  VertInfo         = GetBevelVertexFromVertexID( VertexID, &BevelVertexIndex );
            if ( VertInfo == nullptr )
            {
                BevelVertex NewVertex;
                NewVertex.VertexID = VertexID;
                BevelVertexIndex   = static_cast<int32_t>( m_Vertices.size() );
                m_Vertices.push_back( NewVertex );
                m_VertexIDToIndexMap.insert_or_assign( VertexID, BevelVertexIndex );
                VertInfo = &m_Vertices[BevelVertexIndex];
            }
            VertInfo->IncomingBevelMeshEdges.push_back( IncomingEdgeID );
            VertInfo->IncomingBevelEdgeIndices.push_back( NewBevelEdgeIndex );
            Edge.BevelVertices[ci] = BevelVertexIndex;
        }

        // save the edge span
        Edge.MeshEdges.insert( Edge.MeshEdges.end(), MeshEdgeList.begin(), MeshEdgeList.end() );
        Edge.MeshVertices.insert( Edge.MeshVertices.end(), Topology.m_Edges[GroupEdgeID].Span.Vertices.begin(),
                                  Topology.m_Edges[GroupEdgeID].Span.Vertices.end() );

        Edge.MeshEdgeTris.reserve( static_cast<int32_t>( Edge.MeshEdges.size() ) );
        for ( const int32_t eid : Edge.MeshEdges )
            Edge.MeshEdgeTris.push_back( Mesh.GetEdgeT( eid ) );

        Edge.InitialPositions.reserve( static_cast<int32_t>( Edge.MeshVertices.size() ) );
        for ( const int32_t vid : Edge.MeshVertices )
            Edge.InitialPositions.push_back( Mesh.GetVertex( vid ) );

        Edge.EdgeIndex = static_cast<int32_t>( m_Edges.size() );
        m_Edges.push_back( std::move( Edge ) );
    }

    void MeshBevel::AddBevelEdgeLoop( const DynamicMesh3& Mesh, const EdgeLoop& MeshEdgeLoop )
    {
        // cannot bevel an edge on the mesh boundary
        if ( AnyBoundaryEdge( Mesh, MeshEdgeLoop.Edges ) )
            return;
        if ( RefuseBowties( Mesh, MeshEdgeLoop.Vertices ) )
            return;

        BevelLoop Loop;
        Loop.MeshEdges    = MeshEdgeLoop.Edges;
        Loop.MeshVertices = MeshEdgeLoop.Vertices;

        Loop.MeshEdgeTris.reserve( static_cast<int32_t>( Loop.MeshEdges.size() ) );
        for ( const int32_t eid : Loop.MeshEdges )
            Loop.MeshEdgeTris.push_back( Mesh.GetEdgeT( eid ) );

        Loop.InitialPositions.reserve( static_cast<int32_t>( Loop.MeshVertices.size() ) );
        for ( const int32_t vid : Loop.MeshVertices )
            Loop.InitialPositions.push_back( Mesh.GetVertex( vid ) );

        m_Loops.push_back( Loop );
    }

    void MeshBevel::InitVertexSet( const DynamicMesh3& Mesh, BevelVertex& Vertex )
    {
        // collect up the triangle one-ring around the vertex, as a sequential list
        std::vector<int>  GroupLengths;
        std::vector<bool> bGroupIsLoop;
        const MeshResult  Result =
             Mesh.GetVtxContiguousTriangles( Vertex.VertexID, Vertex.SortedTriangles, GroupLengths, bGroupIsLoop );
        if ( Result != MeshResult::Ok || static_cast<int32_t>( GroupLengths.size() ) != 1 ||
             static_cast<int32_t>( Vertex.SortedTriangles.size() ) < 2 )
        {
            Vertex.VertexType = BevelVertexType::Unknown;
            Refuse( "vertex " + std::to_string( Vertex.VertexID ) + " has " +
                    std::to_string( static_cast<int32_t>( GroupLengths.size() ) ) + " triangle fans and " +
                    std::to_string( static_cast<int32_t>( Vertex.SortedTriangles.size() ) ) +
                    " triangles; a bevel vertex needs one fan of at least two" );
            return;
        }

        // orient the one-ring the same way for every vertex (the wedge walks below depend on it)
        const Index3i Tri0 = Mesh.GetTriangle( Vertex.SortedTriangles[0] ).GetCycled( Vertex.VertexID );
        const Index3i Tri1 = Mesh.GetTriangle( Vertex.SortedTriangles[1] ).GetCycled( Vertex.VertexID );
        if ( Tri0.C == Tri1.B )
            std::reverse( Vertex.SortedTriangles.begin(), Vertex.SortedTriangles.end() );

        // A boundary vertex keeps UE's BoundaryVertex type: its edge ends at the border and needs no vertex
        // meshing.
        if ( Mesh.IsBoundaryVertex( Vertex.VertexID ) )
        {
            Vertex.VertexType = BevelVertexType::BoundaryVertex;
            return;
        }

        if ( static_cast<int32_t>( Vertex.IncomingBevelMeshEdges.size() ) == 1 )
            BuildTerminatorVertex( Vertex, Mesh );
        else
            BuildJunctionVertex( Vertex, Mesh );
    }

    void MeshBevel::FinalizeTerminatorVertex( const DynamicMesh3& Mesh, BevelVertex& Vertex )
    {
        // Two terminators joined by the edge each would split along need that shared edge meshed as one quad.
        if ( Vertex.VertexType != BevelVertexType::TerminatorVertex )
            return;
        const int32_t  OtherVertexID    = Vertex.TerminatorInfo.B;
        const int32_t* OtherBevelVtxIdx = FindValue( m_VertexIDToIndexMap, OtherVertexID );
        if ( OtherBevelVtxIdx == nullptr )
            return;
        const BevelVertex& OtherVertex = m_Vertices[*OtherBevelVtxIdx];
        if ( OtherVertex.VertexType != BevelVertexType::TerminatorVertex )
            return;
        const int32_t MeshEdgeID = Mesh.FindEdge( Vertex.VertexID, OtherVertex.VertexID );
        if ( Mesh.IsEdge( MeshEdgeID ) && Vertex.TerminatorInfo.A == MeshEdgeID &&
             OtherVertex.TerminatorInfo.A == MeshEdgeID &&
             !( std::find( Vertex.IncomingBevelMeshEdges.begin(), Vertex.IncomingBevelMeshEdges.end(),
                           MeshEdgeID ) != Vertex.IncomingBevelMeshEdges.end() ) )
        {
            Vertex.ConnectedBevelVertex = *OtherBevelVtxIdx;
        }
    }

    void MeshBevel::BuildVertexSets( const DynamicMesh3& Mesh )
    {
        // can be parallel
        for ( BevelVertex& Vertex : m_Vertices )
            InitVertexSet( Mesh, Vertex );

        // resolve terminator connections only once every vertex has its type
        for ( BevelVertex& Vertex : m_Vertices )
            FinalizeTerminatorVertex( Mesh, Vertex );
    }

    void MeshBevel::BuildJunctionVertex( BevelVertex& Vertex, const DynamicMesh3& Mesh )
    {
        // Split the sorted one-ring into wedges between the incoming bevel edges. The wedges become separate
        // vertices when the vertex is unlinked, and each is displaced along its two border edges.
        const int32_t NT            = static_cast<int32_t>( Vertex.SortedTriangles.size() );
        int32_t       StartTriIndex = -1;
        for ( int32_t k = 0; k < NT; ++k )
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
            Vertex.VertexType = BevelVertexType::Unknown;
            Refuse( "junction vertex " + std::to_string( Vertex.VertexID ) + ": incoming bevel edge " +
                    std::to_string( Vertex.IncomingBevelMeshEdges[0] ) + " is not between two of its triangles" );
            return;
        }

        int32_t       CurTriIndex = StartTriIndex;
        OneRingWedge  CurWedge;
        CurWedge.WedgeVertex = Vertex.VertexID;
        CurWedge.Triangles.push_back( Vertex.SortedTriangles[CurTriIndex] );
        CurWedge.BorderEdges.A = Vertex.IncomingBevelMeshEdges[0];
        for ( int32_t k = 0; k < NT; ++k )
        {
            const int32_t CurTri     = Vertex.SortedTriangles[CurTriIndex % NT];
            const int32_t NextTri    = Vertex.SortedTriangles[( CurTriIndex + 1 ) % NT];
            const int32_t SharedEdge = FindSharedEdgeInTriangles( Mesh, CurTri, NextTri );
            if ( ( std::find( Vertex.IncomingBevelMeshEdges.begin(), Vertex.IncomingBevelMeshEdges.end(),
                              SharedEdge ) != Vertex.IncomingBevelMeshEdges.end() ) )
            {
                CurWedge.BorderEdges.B = SharedEdge;
                Vertex.Wedges.push_back( CurWedge );
                CurWedge               = OneRingWedge();
                CurWedge.WedgeVertex   = Vertex.VertexID;
                CurWedge.BorderEdges.A = SharedEdge;
            }
            CurWedge.Triangles.push_back( NextTri );
            CurTriIndex++;
        }

        for ( OneRingWedge& Wedge : Vertex.Wedges )
        {
            Wedge.BorderEdgeTriEdgeIndices.A =
                 Mesh.GetTriEdges( Wedge.Triangles[0] ).IndexOf( Wedge.BorderEdges.A );
            Wedge.BorderEdgeTriEdgeIndices.B =
                 Mesh.GetTriEdges( Wedge.Triangles.back() ).IndexOf( Wedge.BorderEdges.B );
        }

        if ( static_cast<int32_t>( Vertex.Wedges.size() ) > 1 )
        {
            Vertex.VertexType = BevelVertexType::JunctionVertex;
        }
        else
        {
            Vertex.VertexType = BevelVertexType::Unknown;
            Refuse( "junction vertex " + std::to_string( Vertex.VertexID ) + " splits into " +
                    std::to_string( static_cast<int32_t>( Vertex.Wedges.size() ) ) +
                    " wedge(s); its incoming bevel edges need at least two" );
        }
    }

    void MeshBevel::BuildTerminatorVertex( BevelVertex& Vertex, const DynamicMesh3& Mesh )
    {
        // A terminator's single incoming edge cannot split the one-ring in two by itself: a second, "ring split"
        // edge is chosen so the vertex can open into an edge, and the hole it leaves is filled later.
        Vertex.VertexType       = BevelVertexType::Unknown;
        const std::string Where = "terminator vertex " + std::to_string( Vertex.VertexID );

        const int32_t  IncomingEdgeID  = Vertex.IncomingBevelMeshEdges[0];
        int32_t        RingSplitEdgeID = -1;
        const Index2i  IncomingEdgeT   = Mesh.GetEdgeT( IncomingEdgeID );
        {
            const Index2i IncomingEdgeGroups( Mesh.GetTriangleGroup( IncomingEdgeT.A ),
                                              Mesh.GetTriangleGroup( IncomingEdgeT.B ) );

            // Start at a triangle of one of the incoming groups, so the other-group triangles are contiguous.
            const int32_t NumTriangles = static_cast<int32_t>( Vertex.SortedTriangles.size() );
            int32_t       StartIndex   = 0;
            for ( int32_t k = 0; k < NumTriangles; ++k )
            {
                if ( IncomingEdgeGroups.Contains( Mesh.GetTriangleGroup( Vertex.SortedTriangles[k] ) ) )
                {
                    StartIndex = k;
                    break;
                }
            }

            std::vector<int32_t> OtherGroupTris; // sorted wedge of triangles in neither group of the incoming edge
            std::vector<int32_t> OtherGroups;    // group IDs encountered, in order
            for ( int32_t k = 0; k < NumTriangles; ++k )
            {
                const int32_t tid = Vertex.SortedTriangles[( StartIndex + k ) % NumTriangles];
                const int32_t gid = Mesh.GetTriangleGroup( tid );
                if ( !IncomingEdgeGroups.Contains( gid ) )
                {
                    OtherGroupTris.push_back( tid );
                    if ( std::find( OtherGroups.begin(), OtherGroups.end(), gid ) == OtherGroups.end() )
                    {
                        OtherGroups.push_back( gid );
                    }
                }
            }
            const int32_t NumRemainingTris = static_cast<int32_t>( OtherGroupTris.size() );

            if ( OtherGroups.empty() )
            {
                // No other group: split along the one-ring edge best aligned with the incoming edge direction.
                const int32_t OtherVID = Mesh.GetEdgeV( IncomingEdgeID ).OtherElement( Vertex.VertexID );
                if ( OtherVID == IndexConstants::InvalidID )
                {
                    Refuse( Where + ": incoming edge " + std::to_string( IncomingEdgeID ) + " does not touch it" );
                    return;
                }
                const glm::dvec3 CenterPos   = Mesh.GetVertex( Vertex.VertexID );
                glm::dvec3       IncomingDir = CenterPos - Mesh.GetVertex( OtherVID );
                Normalize( IncomingDir );
                double BestAlignmentScore = -std::numeric_limits<double>::max();
                Mesh.EnumerateVertexEdges( Vertex.VertexID,
                                           [&]( int32_t EID )
                                           {
                                               if ( EID == IncomingEdgeID )
                                                   return;
                                               const int32_t OutVID =
                                                    Mesh.GetEdgeV( EID ).OtherElement( Vertex.VertexID );
                                               if ( OutVID == IndexConstants::InvalidID )
                                                   return;
                                               glm::dvec3 OutgoingDir = Mesh.GetVertex( OutVID ) - CenterPos;
                                               Normalize( OutgoingDir );
                                               const double AlignmentScore = glm::dot( OutgoingDir, IncomingDir );
                                               if ( AlignmentScore > BestAlignmentScore )
                                               {
                                                   RingSplitEdgeID    = EID;
                                                   BestAlignmentScore = AlignmentScore;
                                               }
                                           } );
            }
            else if ( static_cast<int32_t>( OtherGroups.size() ) == 1 )
            {
                // Exactly one other group: split inside it, and the end-cap triangle JOINS that group (B:1061).
                Vertex.NewGroupID = OtherGroups[0];
                if ( static_cast<int32_t>( OtherGroupTris.size() ) == 1 )
                {
                    const Index3i TriEdges = Mesh.GetTriEdges( OtherGroupTris[0] );
                    for ( int32_t j = 0; j < 3; ++j )
                    {
                        if ( Mesh.GetEdgeV( TriEdges[j] ).Contains( Vertex.VertexID ) )
                        {
                            RingSplitEdgeID = TriEdges[j];
                            break;
                        }
                    }
                }
                else if ( static_cast<int32_t>( OtherGroupTris.size() ) == 2 )
                {
                    RingSplitEdgeID = FindSharedEdgeInTriangles( Mesh, OtherGroupTris[0], OtherGroupTris[1] );
                }
                else
                {
                    const int32_t j = static_cast<int32_t>( OtherGroupTris.size() ) / 2;
                    RingSplitEdgeID = FindSharedEdgeInTriangles( Mesh, OtherGroupTris[j], OtherGroupTris[j + 1] );
                    if ( RingSplitEdgeID == -1 )
                    {
                        for ( int32_t k = 0; k < NumRemainingTris; ++k )
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
                for ( int32_t k = 0; k < static_cast<int32_t>( OtherGroupTris.size() ); ++k )
                {
                    const int32_t TriangleA = OtherGroupTris[k];
                    const int32_t TriangleB = OtherGroupTris[( k + 1 ) % NumRemainingTris];
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

        const Index2i SplitEdgeV = Mesh.GetEdgeV( RingSplitEdgeID );
        Vertex.TerminatorInfo    = Index2i( RingSplitEdgeID, SplitEdgeV.OtherElement( Vertex.VertexID ) );

        std::vector<int32_t> SplitTriSets[2];
        if ( !SplitInteriorVertexTrianglesIntoSubsets( &Mesh, Vertex.VertexID, IncomingEdgeID, RingSplitEdgeID,
                                                       SplitTriSets[0], SplitTriSets[1] ) )
        {
            Refuse( Where + ": edges " + std::to_string( IncomingEdgeID ) + " and " +
                    std::to_string( RingSplitEdgeID ) + " do not split its one-ring in two" );
            return;
        }

        Vertex.Wedges.resize( 2 );
        Vertex.Wedges[0].WedgeVertex = Vertex.VertexID;
        Vertex.Wedges[0].Triangles.insert( Vertex.Wedges[0].Triangles.end(), SplitTriSets[0].begin(),
                                           SplitTriSets[0].end() );
        Vertex.Wedges[1].WedgeVertex = Vertex.VertexID;
        Vertex.Wedges[1].Triangles.insert( Vertex.Wedges[1].Triangles.end(), SplitTriSets[1].begin(),
                                           SplitTriSets[1].end() );

        // Border edges of each wedge are the edges of its end triangles that touch the vertex and no wedge
        // neighbour.
        for ( OneRingWedge& Wedge : Vertex.Wedges )
        {
            const int32_t  NumWedgeTris = static_cast<int32_t>( Wedge.Triangles.size() );
            const Index2i  VtxEdges0    = FindVertexEdgesInTriangle( Mesh, Wedge.Triangles[0], Vertex.VertexID );
            if ( NumWedgeTris == 1 )
            {
                Wedge.BorderEdges.A = VtxEdges0.A;
                Wedge.BorderEdges.B = VtxEdges0.B;
            }
            else
            {
                Wedge.BorderEdges.A =
                     Mesh.GetEdgeT( VtxEdges0.A ).Contains( Wedge.Triangles[1] ) ? VtxEdges0.B : VtxEdges0.A;
                const Index2i VtxEdges1 =
                     FindVertexEdgesInTriangle( Mesh, Wedge.Triangles[NumWedgeTris - 1], Vertex.VertexID );
                Wedge.BorderEdges.B = Mesh.GetEdgeT( VtxEdges1.A ).Contains( Wedge.Triangles[NumWedgeTris - 2] )
                                           ? VtxEdges1.B
                                           : VtxEdges1.A;
            }
            Wedge.BorderEdgeTriEdgeIndices.A =
                 Mesh.GetTriEdges( Wedge.Triangles[0] ).IndexOf( Wedge.BorderEdges.A );
            Wedge.BorderEdgeTriEdgeIndices.B =
                 Mesh.GetTriEdges( Wedge.Triangles.back() ).IndexOf( Wedge.BorderEdges.B );
        }

        Vertex.VertexType = BevelVertexType::TerminatorVertex;
    }

    // ---- Unlink (B:1174-1602): open the bevel edges into pairs of boundary edges ----

    void MeshBevel::UnlinkEdges( DynamicMesh3& Mesh )
    {
        for ( BevelEdge& Edge : m_Edges )
            UnlinkBevelEdgeInterior( Mesh, Edge );
    }

    namespace
    {
        struct VertexSplit
        {
            int32_t         VertexID = -1;
            bool            bOK      = false;
            std::vector<int32_t> TriSets[2];
        };

        // The subset functions pick Set0 arbitrarily per vertex; make Set0 the same side of the span at every
        // vertex by requiring it to share a triangle with the previous vertex's Set0.
        void ReconcileTriangleSets( std::vector<VertexSplit>& SplitSequence )
        {
            const int32_t        N = static_cast<int32_t>( SplitSequence.size() );
            std::vector<int32_t> PrevTriSet0;
            for ( int32_t k = 0; k < N; ++k )
            {
                if ( PrevTriSet0.empty() && !SplitSequence[k].TriSets[0].empty() )
                {
                    PrevTriSet0 = SplitSequence[k].TriSets[0];
                }
                else
                {
                    bool bFoundInSet0 = false;
                    for ( const int32_t tid : SplitSequence[k].TriSets[0] )
                    {
                        if ( ( std::find( PrevTriSet0.begin(), PrevTriSet0.end(), tid ) != PrevTriSet0.end() ) )
                        {
                            bFoundInSet0 = true;
                            break;
                        }
                    }
                    if ( !bFoundInSet0 )
                        std::swap( SplitSequence[k].TriSets[0], SplitSequence[k].TriSets[1] );
                    PrevTriSet0 = SplitSequence[k].TriSets[0];
                }
            }
        }
    } // namespace

    bool MeshBevel::SplitOrKeep( DynamicMesh3& Mesh, int32_t VertexID, const std::vector<int32_t>& Triangles,
                                 const std::string& Where, int32_t& NewVertexOut )
    {
        DynamicMesh3::VertexSplitInfo SplitInfo;
        const MeshResult              Result = Mesh.SplitVertex( VertexID, Triangles, SplitInfo );
        if ( Result == MeshResult::Ok )
        {
            NewVertexOut = SplitInfo.NewVertex;
            return true;
        }
        // UE only ensure()s here and keeps the vertex shared, which later meshes a degenerate strip.
        Refuse( Where + ": SplitVertex(" + std::to_string( VertexID ) + ", " +
                std::to_string( static_cast<int32_t>( Triangles.size() ) ) +
                " triangles) failed with MeshResult " + std::to_string( static_cast<int>( Result ) ) );
        NewVertexOut = VertexID;
        return false;
    }

    void MeshBevel::UnlinkBevelEdgeInterior( DynamicMesh3& Mesh, BevelEdge& BevelEdge )
    {
        const int32_t             N = static_cast<int32_t>( BevelEdge.MeshVertices.size() );
        std::vector<VertexSplit>  SplitsToProcess;
        SplitsToProcess.resize( N );
        const std::string Where = "bevel edge " + std::to_string( BevelEdge.EdgeIndex );

        // Endpoints are split here only on the mesh boundary; otherwise UnlinkVertices owns them.
        const int32_t EndVertex[2] = { 0, N - 1 };
        const int32_t EndEdge[2]   = { 0, N - 2 };
        for ( int32_t j = 0; j < 2; ++j )
        {
            VertexSplit& Split  = SplitsToProcess[EndVertex[j]];
            Split.VertexID      = BevelEdge.MeshVertices[EndVertex[j]];
            if ( !BevelEdge.bEndpointBoundaryFlag[j] )
                continue;
            const int32_t SplitEdge = BevelEdge.MeshEdges[EndEdge[j]];
            Split.bOK               = SplitBoundaryVertexTrianglesIntoSubsets( &Mesh, Split.VertexID, SplitEdge,
                                                                               Split.TriSets[0], Split.TriSets[1] );
            if ( !Split.bOK )
                Refuse( Where + ": boundary end vertex " + std::to_string( Split.VertexID ) +
                        " cannot be split along edge " + std::to_string( SplitEdge ) );
        }
        for ( int32_t k = 1; k < N - 1; ++k )
        {
            VertexSplit& Split  = SplitsToProcess[k];
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

        for ( const VertexSplit& Split : SplitsToProcess )
        {
            int32_t NewVertex = Split.VertexID; // unsplit: the same vertex on both sides
            if ( Split.bOK )
                SplitOrKeep( Mesh, Split.VertexID, Split.TriSets[0], Where, NewVertex );
            BevelEdge.NewMeshVertices.push_back( NewVertex );
        }

        for ( int32_t k = 0; k < N - 1; ++k )
        {
            const int32_t Edge0 = BevelEdge.MeshEdges[k];
            const int32_t Edge1 = Mesh.FindEdge( BevelEdge.NewMeshVertices[k], BevelEdge.NewMeshVertices[k + 1] );
            BevelEdge.NewMeshEdges.push_back( Edge1 );
            if ( Mesh.IsEdge( Edge1 ) && Edge0 != Edge1 && !m_MeshEdgePairs.contains( Edge0 ) )
            {
                m_MeshEdgePairs.insert_or_assign( Edge0, Edge1 );
                m_MeshEdgePairs.insert_or_assign( Edge1, Edge0 );
            }
        }
    }

    void MeshBevel::UnlinkBevelLoop( DynamicMesh3& Mesh, BevelLoop& BevelLoop )
    {
        const int32_t             N = static_cast<int32_t>( BevelLoop.MeshVertices.size() );
        std::vector<VertexSplit>  SplitsToProcess;
        SplitsToProcess.resize( N );
        for ( int32_t k = 0; k < N; ++k )
        {
            VertexSplit& Split  = SplitsToProcess[k];
            Split.VertexID      = BevelLoop.MeshVertices[k];
            if ( Mesh.IsBoundaryVertex( Split.VertexID ) )
                continue; // shared by both sides, as on an edge span
            const int32_t PrevEdge = ( k == 0 ) ? BevelLoop.MeshEdges.back() : BevelLoop.MeshEdges[k - 1];
            const int32_t CurEdge  = BevelLoop.MeshEdges[k];
            Split.bOK = SplitInteriorVertexTrianglesIntoSubsets( &Mesh, Split.VertexID, PrevEdge, CurEdge,
                                                                 Split.TriSets[0], Split.TriSets[1] );
            if ( !Split.bOK )
                Refuse( "bevel loop vertex " + std::to_string( Split.VertexID ) + " cannot be split along edges " +
                        std::to_string( PrevEdge ) + " and " + std::to_string( CurEdge ) );
        }

        ReconcileTriangleSets( SplitsToProcess );

        // Loops move TriSets[1] where edge spans move TriSets[0]; kept as in UE.
        for ( const VertexSplit& Split : SplitsToProcess )
        {
            int32_t NewVertex = Split.VertexID;
            if ( Split.bOK )
                SplitOrKeep( Mesh, Split.VertexID, Split.TriSets[1], "bevel loop", NewVertex );
            BevelLoop.NewMeshVertices.push_back( NewVertex );
        }

        for ( int32_t k = 0; k < N; ++k )
        {
            const int32_t Edge0 = BevelLoop.MeshEdges[k];
            const int32_t Edge1 =
                 Mesh.FindEdge( BevelLoop.NewMeshVertices[k], BevelLoop.NewMeshVertices[( k + 1 ) % N] );
            BevelLoop.NewMeshEdges.push_back( Edge1 );
            if ( Mesh.IsEdge( Edge1 ) && Edge0 != Edge1 && !m_MeshEdgePairs.contains( Edge0 ) )
            {
                m_MeshEdgePairs.insert_or_assign( Edge0, Edge1 );
                m_MeshEdgePairs.insert_or_assign( Edge1, Edge0 );
            }
        }
    }

    void MeshBevel::UnlinkLoops( DynamicMesh3& Mesh )
    {
        for ( BevelLoop& Loop : m_Loops )
            UnlinkBevelLoop( Mesh, Loop );
    }

    void MeshBevel::UnlinkVertices( DynamicMesh3& Mesh )
    {
        // All terminators before any junction, as in UE.
        for ( BevelVertex& Vertex : m_Vertices )
        {
            if ( Vertex.VertexType == BevelVertexType::TerminatorVertex )
                UnlinkTerminatorVertex( Mesh, Vertex );
        }
        for ( BevelVertex& Vertex : m_Vertices )
        {
            if ( Vertex.VertexType == BevelVertexType::JunctionVertex )
                UnlinkJunctionVertex( Mesh, Vertex );
        }
    }

    void MeshBevel::PairSplitWedgeBorderEdges( const DynamicMesh3& Mesh, BevelVertex& Vertex )
    {
        // A border edge the split duplicated has a new ID at the same index of the wedge's own end triangle.
        for ( OneRingWedge& Wedge : Vertex.Wedges )
        {
            for ( int32_t j = 0; j < 2; ++j )
            {
                const int32_t OldWedgeEdgeID    = Wedge.BorderEdges[j];
                const int32_t OldWedgeEdgeIndex = Wedge.BorderEdgeTriEdgeIndices[j];
                const int32_t TriangleID        = ( j == 0 ) ? Wedge.Triangles[0] : Wedge.Triangles.back();
                const int32_t CurWedgeEdgeID    = Mesh.GetTriEdges( TriangleID )[OldWedgeEdgeIndex];
                if ( OldWedgeEdgeID == CurWedgeEdgeID )
                    continue;
                if ( !m_MeshEdgePairs.contains( OldWedgeEdgeID ) )
                {
                    m_MeshEdgePairs.insert_or_assign( OldWedgeEdgeID, CurWedgeEdgeID );
                    m_MeshEdgePairs.insert_or_assign( CurWedgeEdgeID, OldWedgeEdgeID );
                }
                Wedge.BorderEdges[j] = CurWedgeEdgeID;
            }
        }
    }

    void MeshBevel::UnlinkJunctionVertex( DynamicMesh3& Mesh, BevelVertex& Vertex )
    {
        // Wedge 0 keeps the original vertex; every other wedge gets its own copy.
        const std::string Where = "junction vertex " + std::to_string( Vertex.VertexID );
        for ( int32_t k = 1; k < static_cast<int32_t>( Vertex.Wedges.size() ); ++k )
        {
            OneRingWedge& Wedge = Vertex.Wedges[k];
            SplitOrKeep( Mesh, Vertex.VertexID, Wedge.Triangles, Where, Wedge.WedgeVertex );
        }
        PairSplitWedgeBorderEdges( Mesh, Vertex );
    }

    void MeshBevel::UnlinkTerminatorVertex( DynamicMesh3& Mesh, BevelVertex& BevelVertex )
    {
        const std::string Where = "terminator vertex " + std::to_string( BevelVertex.VertexID );
        if ( SplitOrKeep( Mesh, BevelVertex.VertexID, BevelVertex.Wedges[1].Triangles, Where,
                          BevelVertex.Wedges[1].WedgeVertex ) )
            PairSplitWedgeBorderEdges( Mesh, BevelVertex );
    }

    void MeshBevel::FixUpUnlinkedBevelEdges( const DynamicMesh3& Mesh )
    {
        for ( BevelEdge& Edge : m_Edges )
        {
            // A sub-edge whose "new" edge is still the old one (always so for a single-edge span, whose ends are
            // split by the vertex unlinks) takes its partner from the pairs those unlinks recorded.
            const bool bSingleEdge        = static_cast<int32_t>( Edge.MeshEdges.size() ) == 1;
            bool       bFailedEdgePairing = false;
            for ( int32_t Idx = 0; Idx < static_cast<int32_t>( Edge.MeshEdges.size() ); ++Idx )
            {
                const bool bNewEdgeIsOld = Edge.MeshEdges[Idx] == Edge.NewMeshEdges[Idx];
                if ( !bSingleEdge && !bNewEdgeIsOld )
                    continue;
                const int32_t* FoundOtherEdge = FindValue( m_MeshEdgePairs, Edge.MeshEdges[Idx] );
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
            for ( int32_t j = 0; j < 2; ++j )
            {
                const int32_t       vi = ( j == 0 ) ? 0 : ( static_cast<int32_t>( Edge.MeshVertices.size() ) - 1 );
                const int32_t       ei = ( j == 0 ) ? 0 : ( static_cast<int32_t>( Edge.MeshEdges.size() ) - 1 );
                const BevelVertex*  BevelVertex = GetBevelVertexFromVertexID( Edge.MeshVertices[vi] );
                if ( BevelVertex == nullptr )
                {
                    Refuse( "bevel edge " + std::to_string( Edge.EdgeIndex ) + ": end vertex " +
                            std::to_string( Edge.MeshVertices[vi] ) + " is not a bevel vertex" );
                    break;
                }
                int32_t&      V0       = Edge.MeshVertices[vi];
                int32_t&      V1       = Edge.NewMeshVertices[vi];
                const int32_t E0       = Edge.MeshEdges[ei];
                const int32_t E1       = Edge.NewMeshEdges[ei];
                bool          bFoundV0 = false;
                bool          bFoundV1 = false;
                for ( const OneRingWedge& Wedge : BevelVertex->Wedges )
                {
                    for ( const int32_t tid : Wedge.Triangles )
                    {
                        const Index3i TriEdges = Mesh.GetTriEdges( tid );
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

    void MeshBevel::DisplaceVertices( DynamicMesh3& Mesh )
    {
        // Inset every beveled edge into its faces the way InsetMeshRegion does: an 'inset line' per mesh edge,
        // each vertex at the nearest points of its pair of lines (their intersection when the face is planar).
        // Open spans keep their line sets, because the corner vertices combine the end lines of several spans.
        struct EdgePathInsetLines
        {
            std::vector<Line3d> InsetLines0;
            std::vector<Line3d> InsetLines1;
        };
        std::vector<EdgePathInsetLines> AllInsetLines;
        AllInsetLines.resize( static_cast<int32_t>( m_Edges.size() ) );

        for ( int32_t k = 0; k < static_cast<int32_t>( m_Edges.size() ); ++k )
        {
            BevelEdge& Edge = m_Edges[k];
            ComputeInsetLineSegmentsFromEdges( Mesh, Edge.MeshEdges, m_InsetDistance,
                                               AllInsetLines[k].InsetLines0 );
            SolveInsetVertexPositionsFromInsetLines( Mesh, AllInsetLines[k].InsetLines0, Edge.MeshVertices,
                                                     Edge.NewPositions0, false );
            ComputeInsetLineSegmentsFromEdges( Mesh, Edge.NewMeshEdges, m_InsetDistance,
                                               AllInsetLines[k].InsetLines1 );
            SolveInsetVertexPositionsFromInsetLines( Mesh, AllInsetLines[k].InsetLines1, Edge.NewMeshVertices,
                                                     Edge.NewPositions1, false );
        }

        for ( BevelLoop& Loop : m_Loops )
        {
            std::vector<Line3d> InsetLines;
            ComputeInsetLineSegmentsFromEdges( Mesh, Loop.MeshEdges, m_InsetDistance, InsetLines );
            SolveInsetVertexPositionsFromInsetLines( Mesh, InsetLines, Loop.MeshVertices, Loop.NewPositions0,
                                                     true );
            ComputeInsetLineSegmentsFromEdges( Mesh, Loop.NewMeshEdges, m_InsetDistance, InsetLines );
            SolveInsetVertexPositionsFromInsetLines( Mesh, InsetLines, Loop.NewMeshVertices, Loop.NewPositions1,
                                                     true );
        }

        // Corners: each wedge vertex solves against the end inset lines of the bevel edges leaving it.
        for ( BevelVertex& Vertex : m_Vertices )
        {
            if ( Vertex.VertexType == BevelVertexType::Unknown )
                continue;
            for ( OneRingWedge& Wedge : Vertex.Wedges )
            {
                const glm::dvec3 CurPos = Mesh.GetVertex( Wedge.WedgeVertex );

                std::vector<Line3d> SolveLines;
                for ( const int32_t j : Vertex.IncomingBevelEdgeIndices )
                {
                    if ( m_Edges[j].MeshVertices[0] == Wedge.WedgeVertex )
                        SolveLines.push_back( AllInsetLines[j].InsetLines0[0] );
                    else if ( m_Edges[j].MeshVertices.back() == Wedge.WedgeVertex )
                        SolveLines.push_back( AllInsetLines[j].InsetLines0.back() );
                    else if ( m_Edges[j].NewMeshVertices[0] == Wedge.WedgeVertex )
                        SolveLines.push_back( AllInsetLines[j].InsetLines1[0] );
                    else if ( m_Edges[j].NewMeshVertices.back() == Wedge.WedgeVertex )
                        SolveLines.push_back( AllInsetLines[j].InsetLines1.back() );
                }

                const std::string Where = "bevel vertex " + std::to_string( Vertex.VertexID ) + ", wedge vertex " +
                                          std::to_string( Wedge.WedgeVertex );
                // BoundaryVertex never gets here: its wedges are not built (UE's bIsSimpleBoundary is dead).
                if ( Vertex.VertexType == BevelVertexType::TerminatorVertex )
                {
                    // UE silently leaves the vertex in place here (its ensure is commented out as "hit in Lyra").
                    if ( static_cast<int32_t>( SolveLines.size() ) != 1 )
                    {
                        Refuse( Where + ": terminator wedge touches " +
                                std::to_string( static_cast<int32_t>( SolveLines.size() ) ) +
                                " bevel edge ends, expected 1" );
                        continue;
                    }
                    // Nearest point on the inset line can drift off the face the terminating edge runs into, so
                    // slide along the wedge mesh edge best aligned with that inset direction instead (UE's own
                    // stop-gap for "which topology edge should this vertex slide along").
                    const glm::dvec3 InsetLinePosition = SolveLines[0].NearestPoint( CurPos );
                    Wedge.NewPosition                 = InsetLinePosition;
                    const glm::dvec3 BaseInsetDir      = Normalized( InsetLinePosition - CurPos );
                    double          MaxDot            = -1;
                    Line3d           MaxDotEdgeLine;
                    Mesh.EnumerateVertexVertices( Wedge.WedgeVertex,
                                                  [&]( int32_t othervid )
                                                  {
                                                      const Line3d EdgeLine = Line3d::FromPoints(
                                                           CurPos, Mesh.GetVertex( othervid ) );
                                                      const double DirDot =
                                                           glm::dot( EdgeLine.Direction, BaseInsetDir );
                                                      if ( DirDot > MaxDot )
                                                      {
                                                          MaxDot         = DirDot;
                                                          MaxDotEdgeLine = EdgeLine;
                                                      }
                                                  } );
                    if ( MaxDot > -1 )
                    {
                        DistLine3Line3d LineIntersection( SolveLines[0], MaxDotEdgeLine );
                        LineIntersection.Get();
                        Wedge.NewPosition = LineIntersection.m_Line2ClosestPoint;
                    }
                    Wedge.bHaveNewPosition = true;
                }
                else
                {
                    if ( static_cast<int32_t>( SolveLines.size() ) < 2 )
                    {
                        Refuse( Where + ": junction wedge touches " +
                                std::to_string( static_cast<int32_t>( SolveLines.size() ) ) +
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
        auto SetDisplacedPositions = [&Mesh]( const std::vector<int32_t>&    VerticesIn,
                                              const std::vector<glm::dvec3>& PositionsIn, int32_t InsetStart,
                                              int32_t InsetEnd )
        {
            const int32_t NumVertices = static_cast<int32_t>( VerticesIn.size() );
            if ( static_cast<int32_t>( PositionsIn.size() ) != NumVertices )
                return;
            const int32_t Stop = NumVertices - InsetEnd;
            for ( int32_t k = InsetStart; k < Stop; ++k )
                Mesh.SetVertex( VerticesIn[k], PositionsIn[k] );
        };
        for ( const BevelEdge& Edge : m_Edges )
        {
            const int32_t InsetStart = Edge.bEndpointBoundaryFlag[0] ? 0 : 1;
            const int32_t InsetEnd   = Edge.bEndpointBoundaryFlag[1] ? 0 : 1;
            SetDisplacedPositions( Edge.MeshVertices, Edge.NewPositions0, InsetStart, InsetEnd );
            SetDisplacedPositions( Edge.NewMeshVertices, Edge.NewPositions1, InsetStart, InsetEnd );
        }
        for ( const BevelLoop& Loop : m_Loops )
        {
            SetDisplacedPositions( Loop.MeshVertices, Loop.NewPositions0, 0, 0 );
            SetDisplacedPositions( Loop.NewMeshVertices, Loop.NewPositions1, 0, 0 );
        }
        for ( const BevelVertex& Vertex : m_Vertices )
        {
            for ( const OneRingWedge& Wedge : Vertex.Wedges )
            {
                if ( Wedge.bHaveNewPosition )
                    Mesh.SetVertex( Wedge.WedgeVertex, Wedge.NewPosition );
            }
        }
    }

    int32_t MeshBevel::AppendOrRefuse( DynamicMesh3& Mesh, int32_t A, int32_t B, int32_t C, int32_t GroupID,
                                       const std::string& Where )
    {
        const int32_t TriangleID = Mesh.AppendTriangle( A, B, C, GroupID );
        if ( !Mesh.IsTriangle( TriangleID ) )
            Refuse( Where + ": triangle (" + std::to_string( A ) + ", " + std::to_string( B ) + ", " +
                    std::to_string( C ) + ") cannot be appended, AppendTriangle returned " +
                    std::to_string( TriangleID ) );
        return TriangleID;
    }

    void MeshBevel::AppendJunctionVertexPolygon( DynamicMesh3& Mesh, BevelVertex& Vertex )
    {
        // UnlinkJunctionVertex() split the junction vertex into one vertex per (now disconnected) wedge. The
        // wedges are ordered so that their wedge vertices form a polygon with correct winding: mesh it as it
        // stands.
        std::vector<glm::dvec3> PolygonPoints;
        for ( const OneRingWedge& Wedge : Vertex.Wedges )
            PolygonPoints.push_back( Mesh.GetVertex( Wedge.WedgeVertex ) );
        std::vector<Index3i> Triangles;
        PolygonTriangulation::TriangulateSimplePolygon<double>( PolygonPoints, Triangles );
        if ( static_cast<int32_t>( Triangles.size() ) != static_cast<int32_t>( PolygonPoints.size() ) - 2 )
        {
            Refuse( "junction vertex " + std::to_string( Vertex.VertexID ) + ": its " +
                    std::to_string( static_cast<int32_t>( PolygonPoints.size() ) ) + "-gon triangulates into " +
                    std::to_string( static_cast<int32_t>( Triangles.size() ) ) + " triangles" );
            return;
        }
        Vertex.NewGroupID       = Mesh.AllocateTriangleGroup();
        const std::string Where = "junction vertex " + std::to_string( Vertex.VertexID );
        for ( const Index3i& Tri : Triangles )
        {
            const int32_t TriangleID =
                 AppendOrRefuse( Mesh, Vertex.Wedges[Tri.A].WedgeVertex, Vertex.Wedges[Tri.B].WedgeVertex,
                                 Vertex.Wedges[Tri.C].WedgeVertex, Vertex.NewGroupID, Where );
            if ( Mesh.IsTriangle( TriangleID ) )
                Vertex.NewTriangles.push_back( TriangleID );
        }
    }

    void MeshBevel::AppendTerminatorVertexTriangle( DynamicMesh3& Mesh, BevelVertex& Vertex )
    {
        // UnlinkTerminatorVertex() opened a triangle-shaped hole next to the incoming quad strip. The wedges hold
        // the two vertices of the strip's end edge; the third is the far end of the ring-split edge, looked up
        // again because unlinking other vertices may have replaced the FarVertexID stored in TerminatorInfo.
        const std::string Where           = "terminator vertex " + std::to_string( Vertex.VertexID );
        const int32_t     RingSplitEdgeID = Vertex.TerminatorInfo.A;
        if ( !Mesh.IsEdge( RingSplitEdgeID ) )
        {
            Refuse( Where + ": ring-split edge " + std::to_string( RingSplitEdgeID ) + " no longer exists" );
            return;
        }
        const int32_t FarVertexID = Mesh.GetEdgeV( RingSplitEdgeID ).OtherElement( Vertex.VertexID );
        const int32_t QuadEdgeID  = Mesh.FindEdge( Vertex.Wedges[0].WedgeVertex, Vertex.Wedges[1].WedgeVertex );
        if ( !Mesh.IsEdge( QuadEdgeID ) || !Mesh.IsBoundaryEdge( QuadEdgeID ) )
        {
            Refuse( Where + ": wedge vertices " + std::to_string( Vertex.Wedges[0].WedgeVertex ) + " and " +
                    std::to_string( Vertex.Wedges[1].WedgeVertex ) + " share no open edge (edge " +
                    std::to_string( QuadEdgeID ) + ")" );
            return;
        }
        const Index2i QuadEdgeV = Mesh.GetOrientedBoundaryEdgeV( QuadEdgeID );
        // BuildTerminatorVertex gives the cap the group of the face it closes (B:1061), or -1 for a new group.
        const int32_t UseGroupID = ( Vertex.NewGroupID >= 0 ) ? Vertex.NewGroupID : Mesh.AllocateTriangleGroup();
        const int32_t TriangleID =
             AppendOrRefuse( Mesh, QuadEdgeV.B, QuadEdgeV.A, FarVertexID, UseGroupID, Where );
        if ( Mesh.IsTriangle( TriangleID ) )
            Vertex.NewTriangles.push_back( TriangleID );
    }

    void MeshBevel::AppendTerminatorVertexPairQuad( DynamicMesh3& Mesh, BevelVertex& Vertex0,
                                                    BevelVertex& Vertex1 )
    {
        // Two terminators joined directly by the non-beveled ring-split edge both opened their side, so the hole
        // is a quad with a strip end edge at each end; the wedges alone give its corners.
        const std::string Where =
             "terminator pair " + std::to_string( Vertex0.VertexID ) + "/" + std::to_string( Vertex1.VertexID );
        const int32_t QuadEdgeID0 = Mesh.FindEdge( Vertex0.Wedges[0].WedgeVertex, Vertex0.Wedges[1].WedgeVertex );
        const int32_t QuadEdgeID1 = Mesh.FindEdge( Vertex1.Wedges[0].WedgeVertex, Vertex1.Wedges[1].WedgeVertex );
        if ( !Mesh.IsEdge( QuadEdgeID0 ) || !Mesh.IsEdge( QuadEdgeID1 ) || !Mesh.IsBoundaryEdge( QuadEdgeID0 ) ||
             !Mesh.IsBoundaryEdge( QuadEdgeID1 ) )
        {
            Refuse( Where + ": strip end edges " + std::to_string( QuadEdgeID0 ) + " and " +
                    std::to_string( QuadEdgeID1 ) + " are not both open" );
            return;
        }
        const Index2i QuadEdgeV0 = Mesh.GetOrientedBoundaryEdgeV( QuadEdgeID0 );
        const Index2i QuadEdgeV1 = Mesh.GetOrientedBoundaryEdgeV( QuadEdgeID1 );
        if ( Mesh.FindEdge( QuadEdgeV0.A, QuadEdgeV1.B ) == IndexConstants::InvalidID ||
             Mesh.FindEdge( QuadEdgeV0.B, QuadEdgeV1.A ) == IndexConstants::InvalidID )
        {
            Refuse( Where + ": the quad hole's connecting edges are missing" );
            return;
        }
        const int32_t UseGroupID = ( Vertex0.NewGroupID >= 0 ) ? Vertex0.NewGroupID : Mesh.AllocateTriangleGroup();
        // quad order is V0.B, V0.A, V1.B, V1.A
        const int32_t TriangleID0 =
             AppendOrRefuse( Mesh, QuadEdgeV0.B, QuadEdgeV0.A, QuadEdgeV1.B, UseGroupID, Where );
        if ( Mesh.IsTriangle( TriangleID0 ) )
            Vertex0.NewTriangles.push_back( TriangleID0 );
        const int32_t TriangleID1 =
             AppendOrRefuse( Mesh, QuadEdgeV0.B, QuadEdgeV1.B, QuadEdgeV1.A, UseGroupID, Where );
        if ( Mesh.IsTriangle( TriangleID1 ) )
            Vertex1.NewTriangles.push_back( TriangleID1 );
    }

    void MeshBevel::AppendEdgeQuads( DynamicMesh3& Mesh, BevelEdge& Edge )
    {
        const std::string Where    = "bevel edge " + std::to_string( Edge.EdgeIndex );
        const int32_t     NumEdges = static_cast<int32_t>( Edge.MeshEdges.size() );
        if ( NumEdges != static_cast<int32_t>( Edge.NewMeshEdges.size() ) )
        {
            Refuse( Where + ": " + std::to_string( NumEdges ) + " mesh edges but " +
                    std::to_string( static_cast<int32_t>( Edge.NewMeshEdges.size() ) ) + " unlinked partners" );
            return;
        }
        Edge.NewGroupID = Mesh.AllocateTriangleGroup();
        // Each span is fully disconnected into edge pairs by now; join each pair with a quad.
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            // UE falls back to MeshEdgePairs here for a single-edge span; FixUpUnlinkedBevelEdges has already
            // written those partners into NewMeshEdges (or refused), so the fallback is not ported.
            const int32_t EdgeID0 = Edge.MeshEdges[k];
            const int32_t EdgeID1 = Edge.NewMeshEdges[k];
            Index2i       QuadTris( IndexConstants::InvalidID, IndexConstants::InvalidID );
            if ( EdgeID0 == EdgeID1 || !Mesh.IsEdge( EdgeID1 ) )
            {
                Refuse( Where + ": mesh edge " + std::to_string( EdgeID0 ) + " has no unlinked partner (" +
                        std::to_string( EdgeID1 ) + ")" );
                Edge.StripQuads.push_back( QuadTris );
                continue;
            }
            const Index2i EdgeV0 = Mesh.GetOrientedBoundaryEdgeV( EdgeID0 );
            const Index2i EdgeV1 = Mesh.GetOrientedBoundaryEdgeV( EdgeID1 );
            if ( EdgeV0.Contains( EdgeV1.A ) || EdgeV0.Contains( EdgeV1.B ) )
            {
                // The pair still shares one end, so only a triangle fits between them (UE-157531 hits this in
                // complex geometry scripts).
                const int32_t OtherV = EdgeV0.Contains( EdgeV1.A ) ? EdgeV1.B : EdgeV1.A;
                QuadTris.A           = AppendOrRefuse( Mesh, EdgeV0.B, EdgeV0.A, OtherV, Edge.NewGroupID, Where );
            }
            else
            {
                QuadTris.A = AppendOrRefuse( Mesh, EdgeV0.B, EdgeV0.A, EdgeV1.B, Edge.NewGroupID, Where );
                QuadTris.B = AppendOrRefuse( Mesh, EdgeV1.B, EdgeV1.A, EdgeV0.B, Edge.NewGroupID, Where );
            }
            Edge.StripQuads.push_back( QuadTris );
        }
    }

    void MeshBevel::AppendLoopQuads( DynamicMesh3& Mesh, BevelLoop& Loop )
    {
        const int32_t NumEdges = static_cast<int32_t>( Loop.MeshEdges.size() );
        if ( NumEdges != static_cast<int32_t>( Loop.NewMeshEdges.size() ) )
        {
            Refuse( "bevel loop: " + std::to_string( NumEdges ) + " mesh edges but " +
                    std::to_string( static_cast<int32_t>( Loop.NewMeshEdges.size() ) ) + " unlinked partners" );
            return;
        }
        // One new group per pair of input groups the loop runs between.
        auto GetGroupKey = [&Mesh, &Loop]( int32_t k )
        {
            const Index2i  EdgeTris = Loop.MeshEdgeTris[k];
            const int32_t  Group0   = Mesh.GetTriangleGroup( EdgeTris.A );
            const int32_t  Group1   = Mesh.IsTriangle( EdgeTris.B ) ? Mesh.GetTriangleGroup( EdgeTris.B ) : -1;
            return Index2i( std::max( Group0, Group1 ), std::min( Group0, Group1 ) );
        };
        std::unordered_map<Index2i, int32_t> NewGroupIDs;
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            const Index2i GroupKey = GetGroupKey( k );
            if ( !NewGroupIDs.contains( GroupKey ) )
                Loop.NewGroupIDs.push_back(
                     NewGroupIDs.insert_or_assign( GroupKey, Mesh.AllocateTriangleGroup() ).first->second );
        }
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            const std::string Where   = "bevel loop edge " + std::to_string( Loop.MeshEdges[k] );
            const int32_t     EdgeID0 = Loop.MeshEdges[k];
            const int32_t     EdgeID1 = Loop.NewMeshEdges[k];
            Index2i           QuadTris( IndexConstants::InvalidID, IndexConstants::InvalidID );
            if ( EdgeID0 == EdgeID1 || !Mesh.IsEdge( EdgeID1 ) )
            {
                Refuse( Where + ": no unlinked partner (" + std::to_string( EdgeID1 ) + ")" );
                Loop.StripQuads.push_back( QuadTris );
                continue;
            }
            const int32_t  NewGroupID = NewGroupIDs[GetGroupKey( k )];
            const Index2i  EdgeV0     = Mesh.GetOrientedBoundaryEdgeV( EdgeID0 );
            const Index2i  EdgeV1     = Mesh.GetOrientedBoundaryEdgeV( EdgeID1 );
            QuadTris.A                = AppendOrRefuse( Mesh, EdgeV0.B, EdgeV0.A, EdgeV1.B, NewGroupID, Where );
            if ( EdgeV1.Contains( EdgeV0.B ) )
                Refuse( Where + ": the pair still shares vertex " + std::to_string( EdgeV0.B ) );
            else
                QuadTris.B = AppendOrRefuse( Mesh, EdgeV1.B, EdgeV1.A, EdgeV0.B, NewGroupID, Where );
            Loop.StripQuads.push_back( QuadTris );
        }
    }

    void MeshBevel::CreateBevelMeshing( DynamicMesh3& Mesh )
    {
        for ( BevelVertex& Vertex : m_Vertices )
        {
            if ( Vertex.VertexType == BevelVertexType::JunctionVertex &&
                 static_cast<int32_t>( Vertex.Wedges.size() ) > 2 )
                AppendJunctionVertexPolygon( Mesh, Vertex );
        }
        for ( BevelEdge& Edge : m_Edges )
            AppendEdgeQuads( Mesh, Edge );
        for ( BevelLoop& Loop : m_Loops )
            AppendLoopQuads( Mesh, Loop );
        // Terminators last: the strip's end edge now exists and orients their triangle.
        std::unordered_set<Index2i> HandledQuadVtxPairs;
        for ( BevelVertex& Vertex : m_Vertices )
        {
            if ( Vertex.VertexType != BevelVertexType::TerminatorVertex )
                continue;
            if ( Vertex.ConnectedBevelVertex >= 0 )
            {
                BevelVertex& OtherVertex = m_Vertices[Vertex.ConnectedBevelVertex];
                Index2i      VtxPair( Vertex.VertexID, OtherVertex.VertexID );
                VtxPair.Sort();
                if ( !HandledQuadVtxPairs.contains( VtxPair ) )
                {
                    AppendTerminatorVertexPairQuad( Mesh, Vertex, OtherVertex );
                    HandledQuadVtxPairs.insert( VtxPair );
                }
            }
            else
            {
                AppendTerminatorVertexTriangle( Mesh, Vertex );
            }
        }
    }

    namespace
    {
        // Stand-in for UE's FUniformTessellate (Operations/UniformTessellate.cpp, 2138 lines, not ported): the one
        // thing the bevel asks of it is to split every edge of a small polygon patch into TessellationNum + 1
        // equal segments and fill each triangle with the matching barycentric lattice. Input vertex IDs are kept
        // (the junction code finds its corners by ID) and every sub-triangle keeps its parent's winding.
        // OutBarycentrics, for a 3-vertex input, gets every output vertex's weights of input vertices 0, 1, 2,
        // taken from its lattice indices (UE carries them through its tessellator in vertex colours instead).
        DynamicMesh3 UniformTessellatePatch( const DynamicMesh3& Mesh, int32_t TessellationNum,
                                             std::vector<glm::dvec3>* OutBarycentrics = nullptr )
        {
            DynamicMesh3            Out;
            std::vector<glm::dvec3> Bary;
            for ( int32_t VertexID = 0; VertexID < Mesh.MaxVertexID(); ++VertexID )
            {
                Out.AppendVertex( Mesh.GetVertex( VertexID ) );
                Bary.push_back( glm::dvec3( VertexID == 0 ? 1.0 : 0.0, VertexID == 1 ? 1.0 : 0.0,
                                            VertexID == 2 ? 1.0 : 0.0 ) );
            }
            const int32_t N = TessellationNum + 1;
            // interior vertices of each input edge, ordered from its lower to its higher vertex ID
            std::unordered_map<Index2i, std::vector<int32_t>> EdgeVertices;
            auto EdgePoint = [&]( int32_t From, int32_t To, int32_t Step ) -> int32_t
            {
                const Index2i Key( std::min( From, To ), std::max( From, To ) );
                if ( !EdgeVertices.contains( Key ) )
                {
                    std::vector<int32_t>& Span =
                         EdgeVertices.insert_or_assign( Key, std::vector<int32_t>() ).first->second;
                    const glm::dvec3 Lo = Out.GetVertex( Key.A );
                    const glm::dvec3 Hi = Out.GetVertex( Key.B );
                    for ( int32_t s = 1; s < N; ++s )
                    {
                        const double T = static_cast<double>( s ) / N;
                        Span.push_back( Out.AppendVertex( Lerp( Lo, Hi, T ) ) );
                        Bary.push_back( Lerp( Bary[Key.A], Bary[Key.B], T ) );
                    }
                }
                const std::vector<int32_t>& Span = EdgeVertices[Key];
                return ( From < To ) ? Span[Step - 1] : Span[N - 1 - Step];
            };
            for ( const int32_t TriangleID : Mesh.TriangleIndicesItr() )
            {
                const Index3i    Tri = Mesh.GetTriangle( TriangleID );
                const glm::dvec3 A   = Mesh.GetVertex( Tri.A );
                const glm::dvec3 B   = Mesh.GetVertex( Tri.B );
                const glm::dvec3 C   = Mesh.GetVertex( Tri.C );
                // lattice point (i, j) = A + i/N (B - A) + j/N (C - A), i + j <= N
                std::vector<int32_t> Lattice;
                Lattice.assign( ( N + 1 ) * ( N + 1 ), -1 );
                auto At = [&Lattice, N]( int32_t i, int32_t j ) -> int32_t& { return Lattice[i + j * ( N + 1 )]; };
                for ( int32_t j = 0; j <= N; ++j )
                {
                    for ( int32_t i = 0; i + j <= N; ++i )
                    {
                        const int32_t k = N - i - j;
                        if ( i == 0 && j == 0 )
                            At( i, j ) = Tri.A;
                        else if ( i == N )
                            At( i, j ) = Tri.B;
                        else if ( j == N )
                            At( i, j ) = Tri.C;
                        else if ( j == 0 )
                            At( i, j ) = EdgePoint( Tri.A, Tri.B, i );
                        else if ( i == 0 )
                            At( i, j ) = EdgePoint( Tri.A, Tri.C, j );
                        else if ( k == 0 )
                            At( i, j ) = EdgePoint( Tri.B, Tri.C, j );
                        else
                        {
                            const double WA = static_cast<double>( k ) / N;
                            const double WB = static_cast<double>( i ) / N;
                            const double WC = static_cast<double>( j ) / N;
                            At( i, j )      = Out.AppendVertex( WA * A + WB * B + WC * C );
                            Bary.push_back( WA * Bary[Tri.A] + WB * Bary[Tri.B] + WC * Bary[Tri.C] );
                        }
                    }
                }
                for ( int32_t j = 0; j < N; ++j )
                {
                    for ( int32_t i = 0; i + j < N; ++i )
                    {
                        Out.AppendTriangle( At( i, j ), At( i + 1, j ), At( i, j + 1 ) );
                        if ( i + j < N - 1 )
                            Out.AppendTriangle( At( i + 1, j ), At( i + 1, j + 1 ), At( i, j + 1 ) );
                    }
                }
            }
            if ( OutBarycentrics != nullptr )
                *OutBarycentrics = std::move( Bary );
            return Out;
        }
    } // namespace

    void MeshBevel::AppendEdgeQuads_Multi( DynamicMesh3& Mesh, BevelEdge& Edge )
    {
        const std::string Where    = "bevel edge " + std::to_string( Edge.EdgeIndex );
        const int32_t     NumEdges = static_cast<int32_t>( Edge.MeshEdges.size() );
        if ( NumEdges != static_cast<int32_t>( Edge.NewMeshEdges.size() ) )
        {
            Refuse( Where + ": " + std::to_string( NumEdges ) + " mesh edges but " +
                    std::to_string( static_cast<int32_t>( Edge.NewMeshEdges.size() ) ) + " unlinked partners" );
            return;
        }
        Edge.NewGroupID = Mesh.AllocateTriangleGroup();

        struct EdgePair
        {
            Index2i EdgeV0;
            Index2i EdgeV1;
        };
        std::vector<EdgePair> SequentialQuadEdges;
        // Each span is fully disconnected into edge pairs by now; each pair is subdivided into a column of quads.
        // UE falls back to the one-segment AppendEdgeQuads when a pair is missing or still shares a vertex; that
        // would silently mix segment counts at the junctions, so here it is a refusal (FixUpUnlinkedBevelEdges has
        // already written the MeshEdgePairs partners of single-edge spans into NewMeshEdges).
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            const int32_t EdgeID0 = Edge.MeshEdges[k];
            const int32_t EdgeID1 = Edge.NewMeshEdges[k];
            if ( EdgeID0 == EdgeID1 || !Mesh.IsEdge( EdgeID1 ) )
            {
                Refuse( Where + ": mesh edge " + std::to_string( EdgeID0 ) + " has no unlinked partner (" +
                        std::to_string( EdgeID1 ) + ")" );
                return;
            }
            const Index2i EdgeV0 = Mesh.GetOrientedBoundaryEdgeV( EdgeID0 );
            const Index2i EdgeV1 = Mesh.GetOrientedBoundaryEdgeV( EdgeID1 );
            if ( EdgeV0.Contains( EdgeV1.A ) || EdgeV0.Contains( EdgeV1.B ) )
            {
                Refuse( Where + ": mesh edges " + std::to_string( EdgeID0 ) + " and " + std::to_string( EdgeID1 ) +
                        " still share a vertex, so no subdivided quad fits between them" );
                return;
            }
            SequentialQuadEdges.push_back( { EdgeV0, EdgeV1 } );
        }

        // the rows of vertices of the QuadGridPatch of this strip
        const int32_t                     N = m_NumSubdivisions;
        std::vector<std::vector<int32_t>> VertexSpans;
        VertexSpans.resize( N + 2 );
        // appends a column to VertexSpans, with new vertices on the interior of the edge between StartVID and
        // EndVID
        auto AppendNewVertColumn = [&Mesh, &VertexSpans, N]( int32_t StartVID, int32_t EndVID )
        {
            const glm::dvec3 StartPos = Mesh.GetVertex( StartVID );
            const glm::dvec3 EndPos   = Mesh.GetVertex( EndVID );
            VertexSpans[0].push_back( StartVID );
            for ( int32_t j = 0; j < N; ++j )
            {
                const double T = static_cast<double>( j + 1 ) / static_cast<double>( N + 1 );
                VertexSpans[j + 1].push_back( Mesh.AppendVertex( Lerp( StartPos, EndPos, T ) ) );
            }
            VertexSpans[N + 1].push_back( EndVID );
        };
        // appends the column of an already-generated adjacent strip: a bevel vertex joining exactly two bevel
        // edges gets no polygon, so the two strips share that column
        auto AppendExistingVertColumn = [&VertexSpans, N]( const QuadGridPatch* AdjacentQuadPatch,
                                                           int32_t              CornerVertexID ) -> bool
        {
            const int32_t ColumnIdx = AdjacentQuadPatch->FindColumnIndex( CornerVertexID );
            if ( ColumnIdx < 0 )
                return false;
            std::vector<int32_t> ColumnVerts;
            AdjacentQuadPatch->GetVertexColumn( ColumnIdx, ColumnVerts );
            // the column should start with CornerVertexID, but the adjacent patch may have been built reversed
            if ( ColumnVerts.back() == CornerVertexID )
                std::reverse( ColumnVerts.begin(), ColumnVerts.end() );
            for ( int32_t j = 0; j <= N + 1; ++j )
                VertexSpans[j].push_back( ColumnVerts[j] );
            return true;
        };
        auto GetConnectedQuadStripRef = [this]( int32_t BevelVertexIdx,
                                                int32_t CurBevelEdgeIdx ) -> const QuadGridPatch*
        {
            if ( BevelVertexIdx == -1 )
                return nullptr;
            const BevelVertex& Vtx = m_Vertices[BevelVertexIdx];
            if ( Vtx.VertexType != BevelVertexType::JunctionVertex ||
                 static_cast<int32_t>( Vtx.Wedges.size() ) != 2 )
                return nullptr;
            for ( const int32_t EdgeIdx : Vtx.IncomingBevelEdgeIndices )
            {
                if ( EdgeIdx != CurBevelEdgeIdx )
                {
                    const BevelEdge& OtherEdge = m_Edges[EdgeIdx];
                    return OtherEdge.StripQuadPatch.IsEmpty() ? nullptr : &OtherEdge.StripQuadPatch;
                }
            }
            return nullptr;
        };

        // the code below expects the edges spanning vertices 0,1,2 ordered (1,0),(2,1), not (0,1),(1,2)
        if ( static_cast<int32_t>( SequentialQuadEdges.size() ) > 1 &&
             SequentialQuadEdges[0].EdgeV0.B == SequentialQuadEdges[1].EdgeV0.A )
            std::reverse( SequentialQuadEdges.begin(), SequentialQuadEdges.end() );
        // SequentialQuadEdges may run against [BevelVertices.A, BevelVertices.B]; then the previous and next
        // patches are swapped
        const int32_t OriginalPrevVtxID =
             ( Edge.BevelVertices.A >= 0 ) ? m_Vertices[Edge.BevelVertices.A].VertexID : -1;
        const int32_t OriginalNextVtxID =
             ( Edge.BevelVertices.B >= 0 ) ? m_Vertices[Edge.BevelVertices.B].VertexID : -1;
        const Index2i EdgeStartQuadVerts( SequentialQuadEdges[0].EdgeV0.B, SequentialQuadEdges[0].EdgeV1.A );
        const Index2i EdgeEndQuadVerts( SequentialQuadEdges[NumEdges - 1].EdgeV0.A,
                                        SequentialQuadEdges[NumEdges - 1].EdgeV1.B );
        Index2i       BevelEdgeVerts( Edge.BevelVertices.A, Edge.BevelVertices.B );
        if ( EdgeStartQuadVerts.Contains( OriginalNextVtxID ) || EdgeEndQuadVerts.Contains( OriginalPrevVtxID ) )
            std::swap( BevelEdgeVerts.A, BevelEdgeVerts.B );
        const QuadGridPatch* PrevQuadPatch = GetConnectedQuadStripRef( BevelEdgeVerts.A, Edge.EdgeIndex );
        const QuadGridPatch* NextQuadPatch = GetConnectedQuadStripRef( BevelEdgeVerts.B, Edge.EdgeIndex );

        // one column of vertices per initial vertex along the bevel edge
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            const Index2i  EdgeV0 = SequentialQuadEdges[k].EdgeV0;
            const Index2i  EdgeV1 = SequentialQuadEdges[k].EdgeV1;
            const int32_t  QuadA  = EdgeV0.B;
            const int32_t  QuadB  = EdgeV0.A;
            const int32_t  QuadC  = EdgeV1.B;
            const int32_t  QuadD  = EdgeV1.A;
            if ( k == 0 && ( PrevQuadPatch == nullptr || !AppendExistingVertColumn( PrevQuadPatch, QuadA ) ) )
                AppendNewVertColumn( QuadA, QuadD );
            if ( k != NumEdges - 1 || NextQuadPatch == nullptr ||
                 !AppendExistingVertColumn( NextQuadPatch, QuadB ) )
                AppendNewVertColumn( QuadB, QuadC );
        }

        // the quads between the columns
        std::vector<std::vector<Index2i>> QuadSpans;
        QuadSpans.resize( N + 1 );
        const int32_t NumStrips = static_cast<int32_t>( VertexSpans.size() ) - 1;
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            for ( int32_t j = 0; j < NumStrips; ++j )
            {
                Index2i QuadTris;
                QuadTris.A = AppendOrRefuse( Mesh, VertexSpans[j][k], VertexSpans[j][k + 1], VertexSpans[j + 1][k],
                                             Edge.NewGroupID, Where );
                QuadTris.B = AppendOrRefuse( Mesh, VertexSpans[j + 1][k + 1], VertexSpans[j + 1][k],
                                             VertexSpans[j][k + 1], Edge.NewGroupID, Where );
                QuadSpans[j].push_back( QuadTris );
                Edge.StripQuads.push_back( QuadTris );
            }
        }
        if ( m_FailureReason.empty() &&
             !Edge.StripQuadPatch.InitializeFromQuadPatch( Mesh, QuadSpans, VertexSpans ) )
            Refuse( Where + ": its " + std::to_string( NumStrips ) + " x " + std::to_string( NumEdges ) +
                    " quads do not form a quad grid" );
    }

    void MeshBevel::AppendLoopQuads_Multi( DynamicMesh3& Mesh, BevelLoop& Loop )
    {
        const int32_t NumEdges = static_cast<int32_t>( Loop.MeshEdges.size() );
        if ( NumEdges != static_cast<int32_t>( Loop.NewMeshEdges.size() ) )
        {
            Refuse( "bevel loop: " + std::to_string( NumEdges ) + " mesh edges but " +
                    std::to_string( static_cast<int32_t>( Loop.NewMeshEdges.size() ) ) + " unlinked partners" );
            return;
        }
        // One new group per pair of input groups the loop runs between.
        auto GetGroupKey = [&Mesh, &Loop]( int32_t k )
        {
            const Index2i  EdgeTris = Loop.MeshEdgeTris[k];
            const int32_t  Group0   = Mesh.GetTriangleGroup( EdgeTris.A );
            const int32_t  Group1   = Mesh.IsTriangle( EdgeTris.B ) ? Mesh.GetTriangleGroup( EdgeTris.B ) : -1;
            return Index2i( std::max( Group0, Group1 ), std::min( Group0, Group1 ) );
        };
        std::unordered_map<Index2i, int32_t> NewGroupIDs;
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            const Index2i GroupKey = GetGroupKey( k );
            if ( !NewGroupIDs.contains( GroupKey ) )
                Loop.NewGroupIDs.push_back(
                     NewGroupIDs.insert_or_assign( GroupKey, Mesh.AllocateTriangleGroup() ).first->second );
        }

        struct EdgePair
        {
            Index2i EdgeV0;
            Index2i EdgeV1;
        };
        std::vector<EdgePair> SequentialQuadEdges;
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            const int32_t EdgeID0 = Loop.MeshEdges[k];
            const int32_t EdgeID1 = Loop.NewMeshEdges[k];
            // UE falls back to the one-segment AppendLoopQuads here; see AppendEdgeQuads_Multi
            if ( EdgeID0 == EdgeID1 || !Mesh.IsEdge( EdgeID1 ) )
            {
                Refuse( "bevel loop edge " + std::to_string( EdgeID0 ) + ": no unlinked partner (" +
                        std::to_string( EdgeID1 ) + ")" );
                return;
            }
            SequentialQuadEdges.push_back(
                 { Mesh.GetOrientedBoundaryEdgeV( EdgeID0 ), Mesh.GetOrientedBoundaryEdgeV( EdgeID1 ) } );
        }
        const bool bEdgeHasReverseOrder = static_cast<int32_t>( SequentialQuadEdges.size() ) > 1 &&
                                          SequentialQuadEdges[0].EdgeV0.B == SequentialQuadEdges[1].EdgeV0.A;

        const int32_t                     N = m_NumSubdivisions;
        std::vector<std::vector<int32_t>> VertexSpans;
        VertexSpans.resize( N + 2 );
        auto AppendVertColumn = [&Mesh, &VertexSpans, N]( int32_t StartVID, int32_t EndVID )
        {
            const glm::dvec3 StartPos = Mesh.GetVertex( StartVID );
            const glm::dvec3 EndPos   = Mesh.GetVertex( EndVID );
            VertexSpans[0].push_back( StartVID );
            for ( int32_t j = 0; j < N; ++j )
            {
                const double T = static_cast<double>( j + 1 ) / static_cast<double>( N + 1 );
                VertexSpans[j + 1].push_back( Mesh.AppendVertex( Lerp( StartPos, EndPos, T ) ) );
            }
            VertexSpans[N + 1].push_back( EndVID );
        };
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            Index2i EdgeV0 = SequentialQuadEdges[k].EdgeV0;
            Index2i EdgeV1 = SequentialQuadEdges[k].EdgeV1;
            if ( bEdgeHasReverseOrder )
            {
                EdgeV0.Swap();
                EdgeV1.Swap();
            }
            if ( k == 0 )
                AppendVertColumn( EdgeV0.B, EdgeV1.A );
            if ( k != NumEdges - 1 )
            {
                AppendVertColumn( EdgeV0.A, EdgeV1.B );
            }
            else
            {
                // the loop closes onto its first column, repeated so the patch has one column per quad + 1
                for ( int32_t j = 0; j <= N + 1; ++j )
                {
                    const int32_t FirstColVal = VertexSpans[j][0];
                    VertexSpans[j].push_back( FirstColVal );
                }
            }
        }

        std::vector<std::vector<Index2i>> QuadSpans;
        QuadSpans.resize( N + 1 );
        const int32_t SwapOffset0 = bEdgeHasReverseOrder ? 1 : 0;
        const int32_t SwapOffset1 = bEdgeHasReverseOrder ? 0 : 1;
        const int32_t NumStrips   = static_cast<int32_t>( VertexSpans.size() ) - 1;
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            const std::string Where      = "bevel loop edge " + std::to_string( Loop.MeshEdges[k] );
            const int32_t     NewGroupID = NewGroupIDs[GetGroupKey( k )];
            for ( int32_t j = 0; j < NumStrips; ++j )
            {
                Index2i QuadTris;
                QuadTris.A =
                     AppendOrRefuse( Mesh, VertexSpans[j][k + SwapOffset0], VertexSpans[j][k + SwapOffset1],
                                     VertexSpans[j + 1][k], NewGroupID, Where );
                QuadTris.B = AppendOrRefuse( Mesh, VertexSpans[j + 1][k + SwapOffset1],
                                             VertexSpans[j + 1][k + SwapOffset0], VertexSpans[j][k + 1],
                                             NewGroupID, Where );
                QuadSpans[j].push_back( QuadTris );
                Loop.StripQuads.push_back( QuadTris );
            }
        }
        if ( m_FailureReason.empty() &&
             !Loop.StripQuadPatch.InitializeFromQuadPatch( Mesh, QuadSpans, VertexSpans ) )
            Refuse( "bevel loop: its " + std::to_string( NumStrips ) + " x " + std::to_string( NumEdges ) +
                    " quads do not form a quad grid" );
    }

    namespace
    {
        double Dot2( const glm::dvec2& A, const glm::dvec2& B )
        {
            return A.x * B.x + A.y * B.y;
        }
        double Length2( const glm::dvec2& V )
        {
            return std::sqrt( Dot2( V, V ) );
        }
        glm::dvec2 Normalized2( const glm::dvec2& V )
        {
            const double Length = Length2( V );
            return Length > ZeroTolerance<double> ? V * ( 1.0 / Length ) : glm::dvec2( 0.0, 0.0 );
        }
        // VectorUtil::VectorTanHalfAngle: tan of half the angle between two unit vectors
        double TanHalfAngle( const glm::dvec2& A, const glm::dvec2& B )
        {
            const double CosAngle = Dot2( A, B );
            return std::sqrt(
                 std::clamp( ( 1.0 - CosAngle ) / ( 1.0 + CosAngle ), 0.0, std::numeric_limits<double>::max() ) );
        }

        // UE B:2931-2968: per border vertex k, the offset of UVPosition in k's frame (X along the border's central
        // difference, Y = PerpCW(X)) and its polygon mean-value weight (Floater), the weights normalized to sum 1.
        std::vector<glm::dvec3> MeanValueFrameWeights( const glm::dvec2&              UVPosition,
                                                       const std::vector<glm::dvec2>& BorderPolygon )
        {
            const int32_t           NumLoopVerts = static_cast<int32_t>( BorderPolygon.size() );
            std::vector<glm::dvec3> Weights;
            Weights.reserve( NumLoopVerts );
            double WeightSum = 0.0;
            for ( int32_t k = 0; k < NumLoopVerts; ++k )
            {
                const glm::dvec2 BoundaryUVPosition = BorderPolygon[k];
                const glm::dvec2 Prev               = BorderPolygon[( k - 1 + NumLoopVerts ) % NumLoopVerts];
                const glm::dvec2 Next               = BorderPolygon[( k + 1 ) % NumLoopVerts];
                const glm::dvec2 BoundaryFrameX     = Normalized2( Next - Prev );
                const glm::dvec2 BoundaryFrameY( BoundaryFrameX.y, -BoundaryFrameX.x );
                const glm::dvec2 DeltaUV = UVPosition - BoundaryUVPosition;
                const double    Dist    = Length2( DeltaUV );
                double          Weight  = 1.0;
                if ( Dist > ZeroTolerance<double> )
                {
                    const glm::dvec2 DeltaP = Normalized2( BoundaryUVPosition - UVPosition );
                    const double    T1     = TanHalfAngle( Normalized2( Prev - UVPosition ), DeltaP );
                    const double    T2     = TanHalfAngle( Normalized2( Next - UVPosition ), DeltaP );
                    Weight                 = ( T1 + T2 ) / Dist;
                }
                Weights.push_back(
                     glm::dvec3( Dot2( BoundaryFrameX, DeltaUV ), Dot2( BoundaryFrameY, DeltaUV ), Weight ) );
                WeightSum += Weight;
            }
            for ( glm::dvec3& Weight : Weights )
                Weight.z /= WeightSum;
            return Weights;
        }
    } // namespace

    bool MeshBevel::HasRoundProfile() const
    {
        return std::abs( m_RoundWeight ) > ZeroTolerance<float>;
    }

    void MeshBevel::AppendJunctionVertexPolygon_Multi( DynamicMesh3& Mesh, BevelVertex& Vertex )
    {
        // UnlinkJunctionVertex() split the junction vertex into one vertex per wedge, ordered so the wedge
        // vertices wind the polygon correctly; the strips now add their subdivision vertices along each polygon
        // side. The polygon is filled with a regular tessellation whose boundary is exactly that vertex loop.
        const std::string Where = "junction vertex " + std::to_string( Vertex.VertexID );

        // Walk the wedges: the end column of the strip running from wedge vertex A to the next one, B, is one side
        // of the polygon. PolygonVertices/Points get each side without its B (the next side starts there);
        // PolygonCornerVIDs gets each A.
        int32_t           NumEdgeVerts = 0;
        std::vector<glm::dvec3> PolygonCorners;
        std::vector<int32_t>    PolygonVertices;
        std::vector<int32_t>    PolygonCornerVIDs;
        const int32_t           NumWedges = static_cast<int32_t>( Vertex.Wedges.size() );
        for ( int32_t wi = 0; wi < NumWedges; ++wi )
        {
            const int32_t   A = Vertex.Wedges[wi].WedgeVertex;
            const int32_t   B = Vertex.Wedges[( wi + 1 ) % NumWedges].WedgeVertex;
            std::vector<int32_t> Side;
            // UE searches every bevel edge's end columns (IncomingBevelEdgeIndices might do, per its comment)
            for ( const BevelEdge& Span : m_Edges )
            {
                if ( Span.StripQuadPatch.IsEmpty() )
                    continue;
                for ( const int32_t Column : { 0, Span.StripQuadPatch.NumVertexCols() - 1 } )
                {
                    std::vector<int32_t> EndColumn;
                    Span.StripQuadPatch.GetVertexColumn( Column, EndColumn );
                    if ( EndColumn[0] == B && EndColumn.back() == A )
                        std::reverse( EndColumn.begin(), EndColumn.end() );
                    if ( EndColumn[0] == A && EndColumn.back() == B )
                    {
                        Side = EndColumn;
                        break;
                    }
                }
                if ( !Side.empty() )
                    break;
            }
            // UE triangulates the plain polygon when a side is missing or the sides differ; that leaves a corner
            // with fewer segments than its strips, so both are refusals here.
            if ( Side.empty() )
            {
                Refuse( Where + ": no bevel strip column runs from wedge vertex " + std::to_string( A ) + " to " +
                        std::to_string( B ) );
                return;
            }
            if ( NumEdgeVerts != 0 && static_cast<int32_t>( Side.size() ) != NumEdgeVerts )
            {
                Refuse( Where + ": its sides have " + std::to_string( NumEdgeVerts ) + " and " +
                        std::to_string( static_cast<int32_t>( Side.size() ) ) + " vertices" );
                return;
            }
            NumEdgeVerts = static_cast<int32_t>( Side.size() );
            PolygonCorners.push_back( Mesh.GetVertex( A ) );
            PolygonCornerVIDs.push_back( A );
            for ( int32_t j = 0; j < static_cast<int32_t>( Side.size() ) - 1; ++j )
                PolygonVertices.push_back( Side[j] );
        }
        Vertex.NewGroupID = Mesh.AllocateTriangleGroup();

        const int32_t NCorners = static_cast<int32_t>( PolygonCorners.size() );
        if ( NCorners == 4 )
        {
            // quad: a NumEdgeVerts x NumEdgeVerts grid whose border is the polygon, bilinear interior
            std::vector<int32_t> VertexGrid;
            VertexGrid.assign( NumEdgeVerts * NumEdgeVerts, -1 );
            auto At = [&VertexGrid, NumEdgeVerts]( int32_t xi, int32_t yi ) -> int32_t&
            { return VertexGrid[xi + yi * NumEdgeVerts]; };
            int32_t PolygonIdx = 0;
            for ( int32_t xi = 0; xi < NumEdgeVerts; ++xi )
                At( xi, 0 ) = PolygonVertices[PolygonIdx++];
            for ( int32_t yi = 1; yi < NumEdgeVerts - 1; ++yi )
                At( NumEdgeVerts - 1, yi ) = PolygonVertices[PolygonIdx++];
            for ( int32_t xi = NumEdgeVerts - 1; xi >= 0; --xi )
                At( xi, NumEdgeVerts - 1 ) = PolygonVertices[PolygonIdx++];
            for ( int32_t yi = NumEdgeVerts - 2; yi >= 1; --yi )
                At( 0, yi ) = PolygonVertices[PolygonIdx++];
            const glm::dvec3 V00 = Mesh.GetVertex( At( 0, 0 ) );
            const glm::dvec3 V10 = Mesh.GetVertex( At( NumEdgeVerts - 1, 0 ) );
            const glm::dvec3 V01 = Mesh.GetVertex( At( 0, NumEdgeVerts - 1 ) );
            const glm::dvec3 V11 = Mesh.GetVertex( At( NumEdgeVerts - 1, NumEdgeVerts - 1 ) );
            // only the four corners go into InteriorBorderLoop; the round profile blends the curves between them
            Vertex.InteriorBorderLoop =
                 std::vector<int32_t>( { At( 0, 0 ), At( NumEdgeVerts - 1, 0 ), At( 0, NumEdgeVerts - 1 ),
                                         At( NumEdgeVerts - 1, NumEdgeVerts - 1 ) } );
            for ( int32_t yi = 1; yi < NumEdgeVerts - 1; ++yi )
            {
                const double    ty   = static_cast<double>( yi ) / static_cast<double>( NumEdgeVerts - 1 );
                const glm::dvec3 RowA = Lerp( V00, V01, ty );
                const glm::dvec3 RowB = Lerp( V10, V11, ty );
                for ( int32_t xi = 1; xi < NumEdgeVerts - 1; ++xi )
                {
                    const double tx = static_cast<double>( xi ) / static_cast<double>( NumEdgeVerts - 1 );
                    At( xi, yi )    = Mesh.AppendVertex( Lerp( RowA, RowB, tx ) );
                    BevelVertex_InteriorVertex InteriorVertex;
                    InteriorVertex.VertexID = At( xi, yi );
                    InteriorVertex.BorderFrameWeight.push_back( glm::dvec3( tx, ty, 0.0 ) );
                    Vertex.InteriorVertices.push_back( InteriorVertex );
                }
            }
            for ( int32_t y0 = 0; y0 < NumEdgeVerts - 1; ++y0 )
            {
                for ( int32_t x0 = 0; x0 < NumEdgeVerts - 1; ++x0 )
                {
                    const int32_t T0 = AppendOrRefuse( Mesh, At( x0 + 1, y0 ), At( x0, y0 ), At( x0, y0 + 1 ),
                                                       Vertex.NewGroupID, Where );
                    if ( Mesh.IsTriangle( T0 ) )
                        Vertex.NewTriangles.push_back( T0 );
                    const int32_t T1 = AppendOrRefuse( Mesh, At( x0, y0 + 1 ), At( x0 + 1, y0 + 1 ),
                                                       At( x0 + 1, y0 ), Vertex.NewGroupID, Where );
                    if ( Mesh.IsTriangle( T1 ) )
                        Vertex.NewTriangles.push_back( T1 );
                }
            }
            return;
        }

        // Triangle, or a centroid fan for 5+ corners, tessellated uniformly. UE's valence-3 path (B:2654-2740)
        // tessellates a reference triangle to carry barycentric weights in vertex colours; here the tessellator
        // emits them from its lattice indices, and the tessellated corner triangle itself gives UE's positions,
        // so both cases share UE's general-case boundary matching (B:2860-2903).
        DynamicMesh3 TmpMesh;
        for ( int32_t k = 0; k < NCorners; ++k )
            TmpMesh.AppendVertex( PolygonCorners[k] );
        if ( NCorners == 3 )
        {
            TmpMesh.AppendTriangle( 0, 1, 2 );
        }
        else
        {
            glm::dvec3 Centroid = glm::dvec3( 0 );
            for ( const int32_t VertexID : PolygonVertices )
                Centroid += Mesh.GetVertex( VertexID );
            Centroid /= static_cast<double>( static_cast<int32_t>( PolygonVertices.size() ) );
            const int32_t CentroidID = TmpMesh.AppendVertex( Centroid );
            for ( int32_t i = 0; i < NCorners; ++i )
                TmpMesh.AppendTriangle( i, ( i + 1 ) % NCorners, CentroidID );
        }
        std::vector<glm::dvec3> Barycentrics;
        DynamicMesh3            Tess =
             UniformTessellatePatch( TmpMesh, NumEdgeVerts - 2, NCorners == 3 ? &Barycentrics : nullptr );

        // Walk the patch border from corner 0 alongside PolygonVertices; corner 1 must be NumEdgeVerts - 1 steps
        // on.
        const int32_t        NV = static_cast<int32_t>( PolygonVertices.size() );
        MeshBoundaryLoops    BoundaryLoops( &Tess, true );
        std::vector<int32_t> VertexMap;
        VertexMap.assign( Tess.MaxVertexID(), -1 );
        bool            bMapped = false;
        std::vector<int32_t> BoundaryLoopVerts;
        for ( int32_t k = 0; k < BoundaryLoops.GetLoopCount() && !bMapped; ++k )
        {
            std::vector<int32_t>& LoopVerts = BoundaryLoopVerts;
            LoopVerts                       = BoundaryLoops.m_Loops[k].Vertices;
            if ( static_cast<int32_t>( LoopVerts.size() ) != NV )
                continue;
            auto IndexOf = [&LoopVerts]( int32_t VertexID )
            {
                for ( int32_t i = 0; i < static_cast<int32_t>( LoopVerts.size() ); ++i )
                {
                    if ( LoopVerts[i] == VertexID )
                        return i;
                }
                return -1;
            };
            if ( IndexOf( 0 ) < 0 || IndexOf( 1 ) < 0 )
                continue;
            if ( LoopVerts[( IndexOf( 0 ) + NumEdgeVerts - 1 ) % NV] != 1 )
                std::reverse( LoopVerts.begin(), LoopVerts.end() );
            const int32_t FoundIdx = IndexOf( 0 );
            if ( LoopVerts[( FoundIdx + NumEdgeVerts - 1 ) % NV] != 1 )
                continue;
            for ( int32_t j = 0; j < NV; ++j )
                VertexMap[LoopVerts[( FoundIdx + j ) % NV]] = PolygonVertices[j];
            bMapped = true;
        }
        if ( !bMapped )
        {
            Refuse( Where + ": the tessellated " + std::to_string( NCorners ) + "-gon has no border loop of " +
                    std::to_string( NV ) + " vertices matching its corners" );
            return;
        }
        // 5+ corners, round: the patch flattened conformally (UE B:2840-2853) gives each interior vertex its
        // offsets in the border frames and its mean-value coordinates (B:2905-2968). Only the round profile reads
        // them, so a flat bevel does not run (or depend on) the spectral solve UE always runs.
        const bool        bMeanValuePatch = NCorners >= 5 && HasRoundProfile();
        std::vector<glm::dvec2> PatchUVs;
        if ( bMeanValuePatch )
        {
            Tess.EnableAttributes();
            DynamicMeshUVOverlay*  UVOverlay = Tess.Attributes()->PrimaryUV();
            DynamicMeshUVEditor    UVEditor( &Tess, UVOverlay );
            std::vector<int32_t>   AllTriangles;
            for ( const int32_t TriangleID : Tess.TriangleIndicesItr() )
                AllTriangles.push_back( TriangleID );
            std::vector<int32_t> TessToUV;
            bool            bIdentityMap = false;
            UVEditor.SetToPerVertexUVs( TessToUV, bIdentityMap );
            if ( !UVEditor.SetTriangleUVsFromFreeBoundarySpectralConformal( AllTriangles, true, true ) ||
                 !UVEditor.ScaleUVAreaTo3DArea( AllTriangles, true ) )
            {
                Refuse( Where + ": the spectral conformal flattening of its " + std::to_string( NCorners ) +
                        "-gon round patch failed" );
                return;
            }
            PatchUVs.assign( Tess.MaxVertexID(), glm::dvec2( 0.0, 0.0 ) );
            for ( const int32_t VertexID : Tess.VertexIndicesItr() )
            {
                const glm::vec2 UV = UVOverlay->GetElement( TessToUV[VertexID] );
                PatchUVs[VertexID] = glm::dvec2( UV.x, UV.y );
            }
        }
        std::vector<glm::dvec2> BorderPolygon;
        for ( const int32_t VertexID : BoundaryLoopVerts )
            BorderPolygon.push_back( PatchUVs.empty() ? glm::dvec2( 0.0, 0.0 ) : PatchUVs[VertexID] );

        // a triangle keeps its three corners and its interior vertices' barycentrics for the round profile
        if ( NCorners == 3 )
            Vertex.InteriorBorderLoop = PolygonCornerVIDs;
        for ( const int32_t VertexID : Tess.VertexIndicesItr() )
        {
            if ( VertexMap[VertexID] != -1 )
                continue;
            VertexMap[VertexID] = Mesh.AppendVertex( Tess.GetVertex( VertexID ) );
            if ( NCorners == 3 || bMeanValuePatch )
            {
                BevelVertex_InteriorVertex InteriorVertex;
                InteriorVertex.VertexID = VertexMap[VertexID];
                if ( NCorners == 3 )
                    InteriorVertex.BorderFrameWeight.push_back( Barycentrics[VertexID] );
                else
                    InteriorVertex.BorderFrameWeight = MeanValueFrameWeights( PatchUVs[VertexID], BorderPolygon );
                Vertex.InteriorVertices.push_back( InteriorVertex );
            }
        }
        if ( bMeanValuePatch )
        {
            for ( const int32_t VertexID : BoundaryLoopVerts )
                Vertex.InteriorBorderLoop.push_back( VertexMap[VertexID] );
        }
        // PolygonVertices is always constructed in reversed orientation, so patch will be flipped otherwise
        Tess.ReverseOrientation();
        for ( const int32_t TriangleID : Tess.TriangleIndicesItr() )
        {
            const Index3i  Tri           = Tess.GetTriangle( TriangleID );
            const int32_t  NewTriangleID = AppendOrRefuse( Mesh, VertexMap[Tri.A], VertexMap[Tri.B],
                                                           VertexMap[Tri.C], Vertex.NewGroupID, Where );
            if ( Mesh.IsTriangle( NewTriangleID ) )
                Vertex.NewTriangles.push_back( NewTriangleID );
        }
    }

    namespace
    {
        // The end column of a terminator's strip at the terminator (UE CollectTerminatorVtxInfo, B:3085-3118),
        // oriented so its first edge runs along the open boundary. Empty with Error set when the strip does not
        // end in the terminator's two wedge vertices.
        std::vector<int32_t> TerminatorColumn( const DynamicMesh3& Mesh, const QuadGridPatch& Patch,
                                               int32_t WedgeVertexA, int32_t WedgeVertexB, std::string& Error )
        {
            std::vector<int32_t> Column;
            const int32_t   ColumnIndex = Patch.FindColumnIndex( WedgeVertexA );
            if ( ColumnIndex != 0 && ColumnIndex != Patch.NumVertexCols() - 1 )
            {
                Error = "wedge vertex " + std::to_string( WedgeVertexA ) + " is in no end column of its strip";
                return {};
            }
            Patch.GetVertexColumn( ColumnIndex, Column );
            if ( !( std::find( Column.begin(), Column.end(), WedgeVertexB ) != Column.end() ) )
            {
                Error = "the strip's end column misses wedge vertex " + std::to_string( WedgeVertexB );
                return {};
            }
            const int32_t FirstEdgeID = Mesh.FindEdge( Column[0], Column[1] );
            if ( FirstEdgeID == IndexConstants::InvalidID || !Mesh.IsBoundaryEdge( FirstEdgeID ) )
            {
                Error = "the strip's end column does not start on an open edge";
                return {};
            }
            if ( Mesh.GetOrientedBoundaryEdgeV( FirstEdgeID ).A != Column[0] )
                std::reverse( Column.begin(), Column.end() );
            return Column;
        }
    } // namespace

    void MeshBevel::AppendTerminatorVertexTriangles_Multi( DynamicMesh3& Mesh, BevelVertex& Vertex )
    {
        // As AppendTerminatorVertexTriangle, but the strip's end edge is now a column of NumSubdivisions + 1
        // edges: fan them to the far vertex of the ring-split edge.
        const std::string Where           = "terminator vertex " + std::to_string( Vertex.VertexID );
        const int32_t     RingSplitEdgeID = Vertex.TerminatorInfo.A;
        if ( !Mesh.IsEdge( RingSplitEdgeID ) )
        {
            Refuse( Where + ": ring-split edge " + std::to_string( RingSplitEdgeID ) + " no longer exists" );
            return;
        }
        const BevelEdge&           IncomingEdge = m_Edges[Vertex.IncomingBevelEdgeIndices[0]];
        const int32_t         FarVertexID  = Mesh.GetEdgeV( RingSplitEdgeID ).OtherElement( Vertex.VertexID );
        std::string           Error;
        const std::vector<int32_t> QuadStripEdgeVerts =
             TerminatorColumn( Mesh, IncomingEdge.StripQuadPatch, Vertex.Wedges[0].WedgeVertex,
                               Vertex.Wedges[1].WedgeVertex, Error );
        if ( QuadStripEdgeVerts.empty() )
        {
            Refuse( Where + ": " + Error );
            return;
        }
        const int32_t UseGroupID = ( Vertex.NewGroupID >= 0 ) ? Vertex.NewGroupID : Mesh.AllocateTriangleGroup();
        for ( int32_t k = 0; k < static_cast<int32_t>( QuadStripEdgeVerts.size() ) - 1; ++k )
        {
            const int32_t TriangleID = AppendOrRefuse( Mesh, QuadStripEdgeVerts[k + 1], QuadStripEdgeVerts[k],
                                                       FarVertexID, UseGroupID, Where );
            if ( Mesh.IsTriangle( TriangleID ) )
                Vertex.NewTriangles.push_back( TriangleID );
        }
    }

    void MeshBevel::AppendTerminatorVertexPairQuad_Multi( DynamicMesh3& Mesh, BevelVertex& Vertex0,
                                                          BevelVertex& Vertex1 )
    {
        // Two directly connected terminators: their strips' end columns face each other; join them with quads.
        const std::string Where = "terminator vertices " + std::to_string( Vertex0.VertexID ) + " and " +
                                  std::to_string( Vertex1.VertexID );
        std::string           Error;
        const std::vector<int32_t> QuadStripEdgeVerts0 =
             TerminatorColumn( Mesh, m_Edges[Vertex0.IncomingBevelEdgeIndices[0]].StripQuadPatch,
                               Vertex0.Wedges[0].WedgeVertex, Vertex0.Wedges[1].WedgeVertex, Error );
        const std::vector<int32_t> QuadStripEdgeVerts1 =
             TerminatorColumn( Mesh, m_Edges[Vertex1.IncomingBevelEdgeIndices[0]].StripQuadPatch,
                               Vertex1.Wedges[0].WedgeVertex, Vertex1.Wedges[1].WedgeVertex, Error );
        if ( QuadStripEdgeVerts0.empty() || QuadStripEdgeVerts1.empty() )
        {
            Refuse( Where + ": " + Error );
            return;
        }
        if ( QuadStripEdgeVerts0.size() != QuadStripEdgeVerts1.size() )
        {
            Refuse( Where + ": their end columns have " +
                    std::to_string( static_cast<int32_t>( QuadStripEdgeVerts0.size() ) ) + " and " +
                    std::to_string( static_cast<int32_t>( QuadStripEdgeVerts1.size() ) ) + " vertices" );
            return;
        }
        const int32_t UseGroupID = ( Vertex0.NewGroupID >= 0 ) ? Vertex0.NewGroupID : Mesh.AllocateTriangleGroup();
        const int32_t NumEdges   = static_cast<int32_t>( QuadStripEdgeVerts0.size() ) - 1;
        for ( int32_t k = 0; k < NumEdges; ++k )
        {
            const int32_t A0   = QuadStripEdgeVerts0[k];
            const int32_t B0   = QuadStripEdgeVerts0[k + 1];
            const int32_t A1   = QuadStripEdgeVerts1[NumEdges - k];
            const int32_t B1   = QuadStripEdgeVerts1[NumEdges - ( k + 1 )];
            const int32_t Tid0 = AppendOrRefuse( Mesh, B0, A0, A1, UseGroupID, Where );
            if ( Mesh.IsTriangle( Tid0 ) )
                Vertex0.NewTriangles.push_back( Tid0 );
            const int32_t Tid1 = AppendOrRefuse( Mesh, A1, B1, B0, UseGroupID, Where );
            if ( Mesh.IsTriangle( Tid1 ) )
                Vertex0.NewTriangles.push_back( Tid1 );
        }
    }

    void MeshBevel::CreateBevelMeshing_Multi( DynamicMesh3& Mesh )
    {
        // The round profile's arcs are tangent to the faces on either side of the strip: take those normals now,
        // while each unlinked side vertex still touches only its own faces.
        const bool bRound = HasRoundProfile();
        if ( bRound )
        {
            auto FillNormals = [&Mesh]( const std::vector<int32_t>& MeshVertices,
                                        const std::vector<int32_t>& NewMeshVertices,
                                        std::vector<glm::dvec3>& NormalsA, std::vector<glm::dvec3>& NormalsB )
            {
                NormalsA.resize( static_cast<int32_t>( MeshVertices.size() ) );
                NormalsB.resize( static_cast<int32_t>( MeshVertices.size() ) );
                for ( int32_t k = 0; k < static_cast<int32_t>( MeshVertices.size() ); ++k )
                {
                    NormalsA[k] = MeshNormals::ComputeVertexNormal( Mesh, MeshVertices[k] );
                    NormalsB[k] = MeshNormals::ComputeVertexNormal( Mesh, NewMeshVertices[k] );
                }
            };
            for ( BevelEdge& Edge : m_Edges )
                FillNormals( Edge.MeshVertices, Edge.NewMeshVertices, Edge.NormalsA, Edge.NormalsB );
            for ( BevelLoop& Loop : m_Loops )
                FillNormals( Loop.MeshVertices, Loop.NewMeshVertices, Loop.NormalsA, Loop.NormalsB );
        }

        // Strips first: the junction polygons take their sides from the strips' end columns.
        for ( BevelEdge& Edge : m_Edges )
            AppendEdgeQuads_Multi( Mesh, Edge );
        for ( BevelLoop& Loop : m_Loops )
            AppendLoopQuads_Multi( Mesh, Loop );
        if ( !m_FailureReason.empty() )
            return;
        for ( BevelVertex& Vertex : m_Vertices )
        {
            if ( Vertex.VertexType == BevelVertexType::JunctionVertex &&
                 static_cast<int32_t>( Vertex.Wedges.size() ) > 2 )
                AppendJunctionVertexPolygon_Multi( Mesh, Vertex );
        }
        // Terminators last: the strip's end column now exists and orients their triangles.
        std::unordered_set<Index2i> HandledQuadVtxPairs;
        for ( BevelVertex& Vertex : m_Vertices )
        {
            if ( Vertex.VertexType != BevelVertexType::TerminatorVertex )
                continue;
            if ( Vertex.ConnectedBevelVertex >= 0 )
            {
                BevelVertex& OtherVertex = m_Vertices[Vertex.ConnectedBevelVertex];
                Index2i      VtxPair( Vertex.VertexID, OtherVertex.VertexID );
                VtxPair.Sort();
                if ( !HandledQuadVtxPairs.contains( VtxPair ) )
                {
                    AppendTerminatorVertexPairQuad_Multi( Mesh, Vertex, OtherVertex );
                    HandledQuadVtxPairs.insert( VtxPair );
                }
            }
            else
            {
                AppendTerminatorVertexTriangles_Multi( Mesh, Vertex );
            }
        }
        // All topology is final; the round profile only moves vertices.
        if ( bRound && m_FailureReason.empty() )
            ApplyProfileShape_Round( Mesh );
    }

    glm::dvec3 MeshBevel::ArcSplineCurve::Eval( double T ) const
    {
        // FMath::CubicInterp, the CIM_CurveUser segment between parameters 0 and 1
        const double T2 = T * T;
        const double T3 = T2 * T;
        return ( 2.0 * T3 - 3.0 * T2 + 1.0 ) * Pos0 + ( T3 - 2.0 * T2 + T ) * Tangent0 + ( T3 - T2 ) * Tangent1 +
               ( -2.0 * T3 + 3.0 * T2 ) * Pos1;
    }

    MeshBevel::ArcSplineCurve MeshBevel::MakeArcSplineCurve( const glm::dvec3& PosA, const glm::dvec3& NormalA,
                                                             const glm::dvec3& PosB,
                                                             const glm::dvec3& NormalB ) const
    {
        // A and B with their surface normals: B projected onto A's tangent plane gives the direction of the
        // tangent at A, and the other way round. For a planar right angle these tangents are the sides of the
        // square spanned by the arc; a cubic Hermite with them is too flat, so they are scaled by sqrt(2) (UE's
        // default; the arc midpoint then lies at 0.957 of the radius). A curve rather than an exact arc keeps
        // non-planar inputs reasonable.
        const glm::dvec3 AB       = PosB - PosA;
        const glm::dvec3 TangentA = AB - glm::dot( AB, NormalA ) * NormalA;
        const glm::dvec3 BA       = PosA - PosB;
        const glm::dvec3 TangentB = BA - glm::dot( BA, NormalB ) * NormalB;

        ArcSplineCurve Curve;
        Curve.Pos0                = PosA;
        Curve.Pos1                = PosB;
        const double TangentScale = std::abs( m_RoundWeight ) * std::numbers::sqrt2;
        if ( m_RoundWeight >= 0 )
        {
            Curve.Tangent0 = TangentScale * TangentA;
            Curve.Tangent1 = -TangentScale * TangentB;
        }
        else
        {
            Curve.Tangent0 = -TangentScale * TangentB;
            Curve.Tangent1 = TangentScale * TangentA;
        }
        return Curve;
    }

    void MeshBevel::ApplyProfileShape_Round( DynamicMesh3& Mesh )
    {
        // Each strip column (a vertex pair split by the unlink, joined by the subdivided column) is bent onto the
        // arc between its end vertices; the 4-sided junction patches then blend their four curved borders.
        // A column's normals are taken by its end vertices: UE indexes them by column, which assumes the patch
        // columns run in MeshVertices order (it detects only a reversed edge patch).
        struct ColumnSide
        {
            int32_t Index    = -1;
            bool    bSwapped = false; // column starts on the NewMeshVertices side
        };
        auto FindColumnSide = []( const std::vector<int32_t>& MeshVertices,
                                  const std::vector<int32_t>& NewMeshVertices, int32_t A, int32_t B ) -> ColumnSide
        {
            for ( int32_t k = 0; k < static_cast<int32_t>( MeshVertices.size() ); ++k )
            {
                if ( MeshVertices[k] == A && NewMeshVertices[k] == B )
                    return { k, false };
                if ( NewMeshVertices[k] == A && MeshVertices[k] == B )
                    return { k, true };
            }
            return {};
        };
        auto ProjectToPlane = []( const glm::dvec3& V, const glm::dvec3& PlaneNormal )
        { return Normalized( V - glm::dot( V, PlaneNormal ) * PlaneNormal ); };
        auto BendColumn = [&Mesh]( const std::vector<int32_t>& ColVerts, const ArcSplineCurve& Curve )
        {
            const int32_t NV = static_cast<int32_t>( ColVerts.size() );
            for ( int32_t k = 1; k < NV - 1; ++k )
                Mesh.SetVertex( ColVerts[k],
                                Curve.Eval( static_cast<double>( k ) / static_cast<double>( NV - 1 ) ) );
        };

        // Loops have no junctions: every column gets its own arc (the last column repeats the first).
        for ( BevelLoop& Loop : m_Loops )
        {
            const QuadGridPatch& Patch = Loop.StripQuadPatch;
            for ( int32_t Col = 0; Col < Patch.NumVertexCols() - 1; ++Col )
            {
                std::vector<int32_t> ColVerts;
                Patch.GetVertexColumn( Col, ColVerts );
                const int32_t     A    = ColVerts[0];
                const int32_t     B    = ColVerts.back();
                const ColumnSide  Side = FindColumnSide( Loop.MeshVertices, Loop.NewMeshVertices, A, B );
                if ( Side.Index < 0 )
                {
                    Refuse( "bevel loop: strip column " + std::to_string( Col ) + " from vertex " +
                            std::to_string( A ) + " to " + std::to_string( B ) + " is no unlinked vertex pair" );
                    return;
                }
                const glm::dvec3 PosA = Mesh.GetVertex( A );
                const glm::dvec3 PosB = Mesh.GetVertex( B );
                // the section plane through the original vertex and its two inset positions
                const glm::dvec3 InitialPosition = Loop.InitialPositions[Side.Index];
                const glm::dvec3 PlaneNormal     = Normalized(
                     glm::cross( Normalized( PosA - InitialPosition ), Normalized( PosB - InitialPosition ) ) );
                const glm::dvec3 NormalA = ProjectToPlane(
                     Side.bSwapped ? Loop.NormalsB[Side.Index] : Loop.NormalsA[Side.Index], PlaneNormal );
                const glm::dvec3 NormalB = ProjectToPlane(
                     Side.bSwapped ? Loop.NormalsA[Side.Index] : Loop.NormalsB[Side.Index], PlaneNormal );
                BendColumn( ColVerts, MakeArcSplineCurve( PosA, NormalA, PosB, NormalB ) );
            }
        }

        // Edges likewise, keeping each column's curve by its end-vertex pair for the junction patches, and summing
        // the surface normal at every column vertex (UE's DeformNormals, B:3377-3518) for the 5+-sided patches.
        std::unordered_map<Index2i, ArcSplineCurve>   BorderCurves;
        std::vector<glm::dvec3>                       DeformNormals;
        DeformNormals.assign( Mesh.MaxVertexID(), glm::dvec3( 0 ) );
        for ( BevelEdge& Edge : m_Edges )
        {
            const QuadGridPatch&  Patch  = Edge.StripQuadPatch;
            const int32_t         NumVtx = static_cast<int32_t>( Edge.MeshVertices.size() );
            for ( int32_t Col = 0; Col < Patch.NumVertexCols(); ++Col )
            {
                std::vector<int32_t> ColVerts;
                Patch.GetVertexColumn( Col, ColVerts );
                const int32_t     A    = ColVerts[0];
                const int32_t     B    = ColVerts.back();
                const ColumnSide  Side = FindColumnSide( Edge.MeshVertices, Edge.NewMeshVertices, A, B );
                if ( Side.Index < 0 )
                {
                    Refuse( "bevel edge " + std::to_string( Edge.EdgeIndex ) + ": strip column " +
                            std::to_string( Col ) + " from vertex " + std::to_string( A ) + " to " +
                            std::to_string( B ) + " is no unlinked vertex pair" );
                    return;
                }
                const glm::dvec3 PosA               = Mesh.GetVertex( A );
                const glm::dvec3 PosB               = Mesh.GetVertex( B );
                const glm::dvec3 InitialPosition    = Edge.InitialPositions[Side.Index];
                glm::dvec3       SectionPlaneNormal = Normalized(
                     glm::cross( Normalized( PosA - InitialPosition ), Normalized( PosB - InitialPosition ) ) );
                // At a junction of 3+ edges the corner was inset along every edge, so the original vertex and the
                // two inset positions no longer span the section: it is the plane through A and B that contains
                // the original edge direction.
                const bool bIsEndpoint = Side.Index == 0 || Side.Index == NumVtx - 1;
                if ( bIsEndpoint )
                {
                    const BevelVertex& BevelVtx =
                         m_Vertices[Side.Index == 0 ? Edge.BevelVertices.A : Edge.BevelVertices.B];
                    if ( BevelVtx.VertexType == BevelVertexType::JunctionVertex &&
                         static_cast<int32_t>( BevelVtx.IncomingBevelEdgeIndices.size() ) > 2 )
                    {
                        glm::dvec3 InitialEdgeDirection =
                             Side.Index == 0 ? Edge.InitialPositions[1] - InitialPosition
                                             : InitialPosition - Edge.InitialPositions[NumVtx - 2];
                        if ( Normalize( InitialEdgeDirection ) > 0.0 )
                        {
                            Frame3d TempFrame( PosA, Normalized( PosB - PosA ) );
                            TempFrame.ConstrainedAlignAxis( 1, InitialEdgeDirection, TempFrame.Z() );
                            SectionPlaneNormal = TempFrame.Y();
                        }
                    }
                }
                const glm::dvec3 NormalA = ProjectToPlane(
                     Side.bSwapped ? Edge.NormalsB[Side.Index] : Edge.NormalsA[Side.Index], SectionPlaneNormal );
                const glm::dvec3 NormalB = ProjectToPlane(
                     Side.bSwapped ? Edge.NormalsA[Side.Index] : Edge.NormalsB[Side.Index], SectionPlaneNormal );
                const ArcSplineCurve Curve = MakeArcSplineCurve( PosA, NormalA, PosB, NormalB );
                DeformNormals[A] += NormalA;
                DeformNormals[B] += NormalB;
                BorderCurves.insert_or_assign( Index2i( A, B ), Curve );
                BendColumn( ColVerts, Curve );
                // the bent column's normals within its own strip
                for ( int32_t k = 1; k < static_cast<int32_t>( ColVerts.size() ) - 1; ++k )
                    DeformNormals[ColVerts[k]] += MeshNormals::ComputeVertexNormal(
                         Mesh, ColVerts[k], [&Mesh, &Edge]( int32_t TriangleID )
                         { return Mesh.GetTriangleGroup( TriangleID ) == Edge.NewGroupID; }, true, true );
            }
        }
        for ( glm::dvec3& Normal : DeformNormals )
            Normalize( Normal );

        // a border curve keyed (P, Q) or, reversed, (Q, P)
        auto BorderCurve = [&BorderCurves]( int32_t P, int32_t Q, bool& bReversed ) -> const ArcSplineCurve*
        {
            bReversed                    = false;
            const ArcSplineCurve* Found  = FindValue( BorderCurves, Index2i( P, Q ) );
            if ( Found == nullptr )
            {
                Found     = FindValue( BorderCurves, Index2i( Q, P ) );
                bReversed = true;
            }
            return Found;
        };

        // Junction patches: 3 corners (PN triangle), 4 (blended curves), 5+ (mean-value blend of border frames).
        for ( const BevelVertex& Vertex : m_Vertices )
        {
            if ( Vertex.InteriorVertices.empty() )
                continue;
            const std::string Where = "junction vertex " + std::to_string( Vertex.VertexID );
            if ( static_cast<int32_t>( Vertex.InteriorBorderLoop.size() ) == 3 )
            {
                // PN triangle (UE B:3527-3585): the cubic Bezier triangle whose edge control points come from
                // the three border curves' end tangents (a Hermite tangent is 3x the Bezier leg, hence 1/3) and
                // whose centre point b111 is pushed out of the corner plane by RoundWeight.
                const int32_t          i300       = Vertex.InteriorBorderLoop[0];
                const int32_t          i030       = Vertex.InteriorBorderLoop[1];
                const int32_t          i003       = Vertex.InteriorBorderLoop[2];
                const glm::dvec3       b300       = Mesh.GetVertex( i300 );
                const glm::dvec3       b030       = Mesh.GetVertex( i030 );
                const glm::dvec3       b003       = Mesh.GetVertex( i003 );
                bool                   bReversedA = false;
                bool                   bReversedB = false;
                bool                   bReversedC = false;
                const ArcSplineCurve*  CurveA     = BorderCurve( i300, i030, bReversedA );
                const ArcSplineCurve*  CurveB     = BorderCurve( i030, i003, bReversedB );
                const ArcSplineCurve*  CurveC     = BorderCurve( i003, i300, bReversedC );
                if ( CurveA == nullptr || CurveB == nullptr || CurveC == nullptr )
                {
                    Refuse( Where + ": a side of its 3-sided patch is no bevel strip end column" );
                    return;
                }
                // the control point next to the curve's start P and the one next to its end Q
                auto NearStart = []( const ArcSplineCurve& Curve, bool bReversed )
                { return ( bReversed ? -Curve.Tangent1 : Curve.Tangent0 ) / 3.0; };
                auto NearEnd = []( const ArcSplineCurve& Curve, bool bReversed )
                { return ( bReversed ? Curve.Tangent0 : -Curve.Tangent1 ) / 3.0; };
                const glm::dvec3 b210 = b300 + NearStart( *CurveA, bReversedA );
                const glm::dvec3 b120 = b030 + NearEnd( *CurveA, bReversedA );
                const glm::dvec3 b021 = b030 + NearStart( *CurveB, bReversedB );
                const glm::dvec3 b012 = b003 + NearEnd( *CurveB, bReversedB );
                const glm::dvec3 b102 = b003 + NearStart( *CurveC, bReversedC );
                const glm::dvec3 b201 = b300 + NearEnd( *CurveC, bReversedC );
                const glm::dvec3 E    = ( b210 + b120 + b021 + b012 + b102 + b201 ) / 6.0;
                const glm::dvec3 V    = ( b300 + b030 + b003 ) / 3.0;
                const glm::dvec3 b111 = E + m_RoundWeight * ( E - V ) / 2.0;
                for ( const BevelVertex_InteriorVertex& InteriorVtx : Vertex.InteriorVertices )
                {
                    const double w = InteriorVtx.BorderFrameWeight[0].x;
                    const double u = InteriorVtx.BorderFrameWeight[0].y;
                    const double v = InteriorVtx.BorderFrameWeight[0].z;
                    Mesh.SetVertex( InteriorVtx.VertexID,
                                    b300 * ( w * w * w ) + b030 * ( u * u * u ) + b003 * ( v * v * v ) +
                                         b210 * ( 3 * w * w * u ) + b120 * ( 3 * w * u * u ) +
                                         b201 * ( 3 * w * w * v ) + b021 * ( 3 * u * u * v ) +
                                         b102 * ( 3 * w * v * v ) + b012 * ( 3 * u * v * v ) +
                                         b111 * ( 6 * w * u * v ) );
                }
                continue;
            }
            const int32_t LoopN = static_cast<int32_t>( Vertex.InteriorBorderLoop.size() );
            if ( LoopN >= 5 )
            {
                // UE B:3697-3735: rebuild the vertex from its flattened offset in every border vertex's frame (X
                // along the border, Y = N x X across the rounded surface) and blend those by its mean-value
                // weights. Smooth inside, not tangent-continuous with the border (UE's own caveat).
                for ( const BevelVertex_InteriorVertex& InteriorVtx : Vertex.InteriorVertices )
                {
                    if ( static_cast<int32_t>( InteriorVtx.BorderFrameWeight.size() ) != LoopN )
                    {
                        Refuse( Where + ": interior vertex " + std::to_string( InteriorVtx.VertexID ) + " has " +
                                std::to_string( static_cast<int32_t>( InteriorVtx.BorderFrameWeight.size() ) ) +
                                " border frame weights for a border loop of " + std::to_string( LoopN ) );
                        return;
                    }
                    glm::dvec3 BlendedPos = glm::dvec3( 0 );
                    double    WeightSum  = 0.0;
                    for ( int32_t k = 0; k < LoopN; ++k )
                    {
                        const int32_t   BorderVID = Vertex.InteriorBorderLoop[k];
                        const glm::dvec3 BorderFrameX =
                             Normalized( Mesh.GetVertex( Vertex.InteriorBorderLoop[( k + 1 ) % LoopN] ) -
                                         Mesh.GetVertex( Vertex.InteriorBorderLoop[( k - 1 + LoopN ) % LoopN] ) );
                        glm::dvec3 BorderFrameN = DeformNormals[BorderVID];
                        if ( m_RoundWeight < 0 )
                        {
                            // Quaterniond(BorderFrameX, -45, true) * N, as Rodrigues' rotation
                            const double Angle = -0.78539816339744830962;
                            BorderFrameN       = BorderFrameN * std::cos( Angle ) +
                                           glm::cross( BorderFrameX, BorderFrameN ) * std::sin( Angle ) +
                                           BorderFrameX * ( glm::dot( BorderFrameX, BorderFrameN ) *
                                                            ( 1.0 - std::cos( Angle ) ) );
                        }
                        const glm::dvec3 BorderFrameY     = glm::cross( BorderFrameN, BorderFrameX );
                        const glm::dvec3 FrameDeltaWeight = InteriorVtx.BorderFrameWeight[k];
                        const glm::dvec3 ReconstructedPos = Mesh.GetVertex( BorderVID ) +
                                                            FrameDeltaWeight.x * BorderFrameX +
                                                            FrameDeltaWeight.y * BorderFrameY;
                        BlendedPos += FrameDeltaWeight.z * ReconstructedPos;
                        WeightSum += FrameDeltaWeight.z;
                    }
                    Mesh.SetVertex( InteriorVtx.VertexID, BlendedPos * ( 1.0 / WeightSum ) );
                }
                continue;
            }
            if ( LoopN != 4 )
            {
                Refuse( Where + ": the round profile has no patch for its " + std::to_string( LoopN ) +
                        "-corner polygon" );
                return;
            }
            // 4-sided: blend the two "X" border curves along the two "Y" ones.
            //   c01  X2   c11      ty = 1
            //    Y1 |  XInterp | Y2
            //   c00  X1   c10      ty = 0;   tx = 0 at Y1, 1 at Y2
            const int32_t          c00         = Vertex.InteriorBorderLoop[0];
            const int32_t          c10         = Vertex.InteriorBorderLoop[1];
            const int32_t          c01         = Vertex.InteriorBorderLoop[2];
            const int32_t          c11         = Vertex.InteriorBorderLoop[3];
            bool                   bReversedY1 = false;
            bool                   bReversedY2 = false;
            bool                   bReversedX1 = false;
            bool                   bReversedX2 = false;
            const ArcSplineCurve*  CurveY1     = BorderCurve( c00, c01, bReversedY1 );
            const ArcSplineCurve*  CurveY2     = BorderCurve( c10, c11, bReversedY2 );
            const ArcSplineCurve*  CurveX1     = BorderCurve( c00, c10, bReversedX1 );
            const ArcSplineCurve*  CurveX2     = BorderCurve( c01, c11, bReversedX2 );
            if ( CurveY1 == nullptr || CurveY2 == nullptr || CurveX1 == nullptr || CurveX2 == nullptr )
            {
                Refuse( Where + ": a side of its 4-sided patch is no bevel strip end column" );
                return;
            }
            // the X curves' end tangents, pointing out of the patch at tx = 0 and along +tx at tx = 1
            const glm::dvec3 Tangent00 = bReversedX1 ? CurveX1->Tangent1 : -CurveX1->Tangent0;
            const glm::dvec3 Tangent10 = bReversedX1 ? -CurveX1->Tangent0 : CurveX1->Tangent1;
            const glm::dvec3 Tangent01 = bReversedX2 ? CurveX2->Tangent1 : -CurveX2->Tangent0;
            const glm::dvec3 Tangent11 = bReversedX2 ? -CurveX2->Tangent0 : CurveX2->Tangent1;
            for ( const BevelVertex_InteriorVertex& InteriorVtx : Vertex.InteriorVertices )
            {
                const double    tx = InteriorVtx.BorderFrameWeight[0].x;
                const double    ty = InteriorVtx.BorderFrameWeight[0].y;
                ArcSplineCurve  InterpolatedXCurve;
                InterpolatedXCurve.Pos0     = CurveY1->Eval( bReversedY1 ? 1.0 - ty : ty );
                InterpolatedXCurve.Pos1     = CurveY2->Eval( bReversedY2 ? 1.0 - ty : ty );
                InterpolatedXCurve.Tangent0 = -Lerp( Tangent00, Tangent01, ty );
                InterpolatedXCurve.Tangent1 = Lerp( Tangent10, Tangent11, ty );
                Mesh.SetVertex( InteriorVtx.VertexID, InterpolatedXCurve.Eval( tx ) );
            }
        }
    }

    bool MeshBevel::Apply( DynamicMesh3& Mesh )
    {
        // UE's FixBowties is RefuseBowties at initialization; each phase below may Refuse.
        UnlinkEdges( Mesh );
        if ( !m_FailureReason.empty() )
            return false;
        UnlinkLoops( Mesh );
        if ( !m_FailureReason.empty() )
            return false;
        UnlinkVertices( Mesh );
        if ( !m_FailureReason.empty() )
            return false;
        FixUpUnlinkedBevelEdges( Mesh );
        if ( !m_FailureReason.empty() )
            return false;
        DisplaceVertices( Mesh );
        if ( !m_FailureReason.empty() )
            return false;
        if ( m_NumSubdivisions <= 0 )
            CreateBevelMeshing( Mesh );
        else
            CreateBevelMeshing_Multi( Mesh );
        if ( !m_FailureReason.empty() )
            return false;

        m_NewTriangles.clear();
        for ( const BevelVertex& Vertex : m_Vertices )
            m_NewTriangles.insert( m_NewTriangles.end(), Vertex.NewTriangles.begin(), Vertex.NewTriangles.end() );
        for ( const BevelEdge& Edge : m_Edges )
            QuadsToTris( Mesh, Edge.StripQuads, m_NewTriangles, false );
        for ( const BevelLoop& Loop : m_Loops )
            QuadsToTris( Mesh, Loop.StripQuads, m_NewTriangles, false );

        ComputeNormals( Mesh );
        ComputeUVs( Mesh );
        if ( !m_FailureReason.empty() )
            return false;
        ComputeMaterialIDs( Mesh );
        return true;
    }

    // UE ignores the ExpMap result; a region it could not parameterize is a refusal here.
    void MeshBevel::ComputeUVs( DynamicMesh3& Mesh )
    {
        if ( !Mesh.HasAttributes() || Mesh.Attributes()->NumUVLayers() == 0 )
            return;
        DynamicMeshUVOverlay* UVOverlay = Mesh.Attributes()->PrimaryUV();

        auto SetUVsOnTriRegion = [this, &Mesh, UVOverlay]( const std::vector<int32_t>& Triangles )
        {
            if ( !Triangles.empty() && !ComputeArbitraryTrianglePatchUVs( Mesh, *UVOverlay, Triangles ) )
                Refuse( "ComputeUVs: the ExpMap failed on a bevel region of " +
                        std::to_string( static_cast<int32_t>( Triangles.size() ) ) +
                        " triangles starting at triangle " + std::to_string( Triangles[0] ) );
        };
        std::vector<int32_t> TriList;
        for ( const BevelEdge& Edge : m_Edges )
        {
            QuadsToTris( Mesh, Edge.StripQuads, TriList, true );
            SetUVsOnTriRegion( TriList );
        }
        for ( const BevelLoop& Loop : m_Loops )
        {
            QuadsToTris( Mesh, Loop.StripQuads, TriList, true );
            SetUVsOnTriRegion( TriList );
        }
        // vertices last: until the edges have UVs, the vertex polygons have no neighbour UV islands to scale by
        for ( const BevelVertex& Vertex : m_Vertices )
            SetUVsOnTriRegion( Vertex.NewTriangles );
    }

    void MeshBevel::ComputeNormals( DynamicMesh3& Mesh )
    {
        if ( !Mesh.HasAttributes() )
            return;
        DynamicMeshNormalOverlay* NormalOverlay = Mesh.Attributes()->PrimaryNormals();

        auto SetNormalsOnTriRegion = [NormalOverlay]( const std::vector<int32_t>& Triangles )
        {
            if ( !Triangles.empty() )
                MeshNormals::InitializeOverlayRegionToPerVertexNormals( NormalOverlay, Triangles );
        };
        for ( const BevelVertex& Vertex : m_Vertices )
            SetNormalsOnTriRegion( Vertex.NewTriangles );
        std::vector<int32_t> TriList;
        for ( const BevelEdge& Edge : m_Edges )
        {
            QuadsToTris( Mesh, Edge.StripQuads, TriList, true );
            SetNormalsOnTriRegion( TriList );
        }
        for ( const BevelLoop& Loop : m_Loops )
        {
            QuadsToTris( Mesh, Loop.StripQuads, TriList, true );
            SetNormalsOnTriRegion( TriList );
        }
    }

    // Three UE slips are fixed: the second strip neighbour was guarded by the first one's index (B:3861, reading
    // triangle -1 on an open edge); a neighbour material was only counted the first time it was seen (B:3923),
    // and the minimum count was picked where the comment says "most frequent" (B:3949). A terminator cap thus
    // takes the material of the face it closes, not whichever neighbour came first.
    void MeshBevel::ComputeMaterialIDs( DynamicMesh3& Mesh )
    {
        if ( !Mesh.HasAttributes() || !Mesh.Attributes()->HasMaterialID() )
            return;
        DynamicMeshMaterialAttribute* MaterialIDs = Mesh.Attributes()->GetMaterialID();

        if ( m_MaterialIDMode == MaterialIDMode::ConstantMaterialID )
        {
            for ( const int32_t tid : m_NewTriangles )
                MaterialIDs->SetValue( tid, m_SetConstantMaterialID );
            return;
        }

        auto SetQuadMaterial = [MaterialIDs]( const Index2i& Quad, int32_t MaterialID )
        {
            if ( Quad.A >= 0 )
                MaterialIDs->SetValue( Quad.A, MaterialID );
            if ( Quad.B >= 0 )
                MaterialIDs->SetValue( Quad.B, MaterialID );
        };

        // Materials of the new triangles along a beveled edge follow the adjacent pre-bevel triangles; an edge
        // between two materials takes the lowest material seen along the strip (InferMaterialID) or the constant.
        auto SetEdgeMaterials = [&]( const std::vector<Index2i>& StripQuads, const std::vector<Index2i>& EdgeTris )
        {
            const int32_t NumEdges = static_cast<int32_t>( EdgeTris.size() );
            if ( static_cast<int32_t>( StripQuads.size() ) != NumEdges )
            {
                for ( const Index2i& Quad : StripQuads )
                    SetQuadMaterial( Quad, m_SetConstantMaterialID );
                return;
            }
            std::vector<int32_t> SawMaterialIDs;
            std::vector<int32_t> AmbiguousEdges;
            for ( int32_t k = 0; k < NumEdges; ++k )
            {
                const Index2i  NbrTris  = EdgeTris[k];
                const int32_t  MatIDA   = MaterialIDs->GetValue( NbrTris.A );
                const int32_t  MatIDB   = ( NbrTris.B >= 0 ) ? MaterialIDs->GetValue( NbrTris.B ) : MatIDA;
                int32_t        SetMatID = MatIDA;
                if ( std::find( SawMaterialIDs.begin(), SawMaterialIDs.end(), MatIDA ) == SawMaterialIDs.end() )
                {
                    SawMaterialIDs.push_back( MatIDA );
                }
                if ( std::find( SawMaterialIDs.begin(), SawMaterialIDs.end(), MatIDB ) == SawMaterialIDs.end() )
                {
                    SawMaterialIDs.push_back( MatIDB );
                }
                if ( MatIDA != MatIDB )
                {
                    SetMatID = m_SetConstantMaterialID;
                    if ( m_MaterialIDMode == MaterialIDMode::InferMaterialID )
                        AmbiguousEdges.push_back( k );
                }
                SetQuadMaterial( StripQuads[k], SetMatID );
            }
            if ( !AmbiguousEdges.empty() )
            {
                const int32_t LowestMatID = *std::min_element( SawMaterialIDs.begin(), SawMaterialIDs.end() );
                for ( const int32_t k : AmbiguousEdges )
                    SetQuadMaterial( StripQuads[k], LowestMatID );
            }
        };
        for ( const BevelEdge& Edge : m_Edges )
            SetEdgeMaterials( Edge.StripQuads, Edge.MeshEdgeTris );
        for ( const BevelLoop& Loop : m_Loops )
            SetEdgeMaterials( Loop.StripQuads, Loop.MeshEdgeTris );

        // Each vertex polygon takes the most frequent material across its border (the strips and faces around it).
        for ( const BevelVertex& Vertex : m_Vertices )
        {
            std::vector<int32_t> NbrMaterialIDs;
            std::vector<int32_t> NbrMaterialIDCounts;
            for ( const int32_t tid : Vertex.NewTriangles )
            {
                const Index3i TriNbrs = Mesh.GetTriNeighbourTris( tid );
                for ( int32_t j = 0; j < 3; ++j )
                {
                    const int32_t NbrTriangleID = TriNbrs[j];
                    if ( !Mesh.IsTriangle( NbrTriangleID ) ||
                         ( std::find( Vertex.NewTriangles.begin(), Vertex.NewTriangles.end(), NbrTriangleID ) !=
                           Vertex.NewTriangles.end() ) )
                        continue;
                    const int32_t MaterialID = MaterialIDs->GetValue( NbrTriangleID );
                    const auto    Found = std::find( NbrMaterialIDs.begin(), NbrMaterialIDs.end(), MaterialID );
                    const int32_t Index = static_cast<int32_t>( Found - NbrMaterialIDs.begin() );
                    if ( Found == NbrMaterialIDs.end() )
                        NbrMaterialIDs.push_back( MaterialID );
                    if ( NbrMaterialIDCounts.size() != NbrMaterialIDs.size() )
                        NbrMaterialIDCounts.push_back( 0 );
                    NbrMaterialIDCounts[Index]++;
                }
            }
            int32_t SetMaterialID = m_SetConstantMaterialID;
            if ( !NbrMaterialIDs.empty() )
            {
                int32_t MaxIndex = 0;
                for ( int32_t k = 1; k < static_cast<int32_t>( NbrMaterialIDs.size() ); ++k )
                {
                    if ( NbrMaterialIDCounts[k] > NbrMaterialIDCounts[MaxIndex] )
                        MaxIndex = k;
                }
                SetMaterialID = NbrMaterialIDs[MaxIndex];
            }
            for ( const int32_t tid : Vertex.NewTriangles )
                MaterialIDs->SetValue( tid, SetMaterialID );
        }
    }
} // namespace Desert::Geometry
