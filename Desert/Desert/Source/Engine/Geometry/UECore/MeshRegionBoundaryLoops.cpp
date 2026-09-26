// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/MeshRegionBoundaryLoops.cpp:14-306, 483-537 and
// Engine/Source/Runtime/GeometryCore/Private/EdgeSpan.cpp (InitializeFromVertices/InitializeFromEdges), adapted:
// UE Core via UECore.hpp, namespace Desert::Geometry, FIndexFlagSet is TArray<bool>; a bowtie vertex fails Compute
// with FailureReason (see the header) instead of entering FindLeftTurnEdge/TryExtractSubloops.
// GetLoopOverlayMap returns false where UE checks that the loop edge has an inside triangle.
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include "Engine/Geometry/UECore/IndexUtil.hpp"

using namespace Desert::Geometry;

void EdgeSpan::InitializeFromVertices( const DynamicMesh3& Mesh, const std::vector<int>& VerticesIn )
{
    Vertices = VerticesIn;
    Edges.clear();
    for ( int i = 0; i + 1 < static_cast<int32_t>( Vertices.size() ); ++i )
    {
        const int Eid = Mesh.FindEdge( Vertices[i], Vertices[i + 1] );
        assert( Eid != IndexConstants::InvalidID );
        Edges.push_back( Eid );
    }
}

void EdgeSpan::InitializeFromEdges( const DynamicMesh3& Mesh, const std::vector<int>& EdgesIn )
{
    Edges              = EdgesIn;
    const int NumEdges = static_cast<int32_t>( Edges.size() );
    Vertices.resize( NumEdges + 1 );
    const Index2i StartEv = Mesh.GetEdgeV( Edges[0] );
    Index2i       PrevEv  = StartEv;
    for ( int i = 1; i < NumEdges; ++i )
    {
        const Index2i NextEv  = Mesh.GetEdgeV( Edges[i] );
        Vertices[i]           = IndexUtil::FindSharedEdgeVertex( PrevEv, NextEv );
        PrevEv                = NextEv;
    }
    Vertices[0]        = IndexUtil::FindEdgeOtherVertex( StartEv, Vertices[1] );
    Vertices[NumEdges] = IndexUtil::FindEdgeOtherVertex( PrevEv, Vertices[NumEdges - 1] );
}

void EdgeLoop::Initialize( const std::vector<int>& VerticesIn, const std::vector<int>& EdgesIn,
                           const std::vector<int>* BowtieVerticesIn )
{
    Vertices = VerticesIn;
    Edges    = EdgesIn;
    if ( BowtieVerticesIn != nullptr )
        BowtieVertices = *BowtieVerticesIn;
}

void EdgeLoop::InitializeFromEdges( const DynamicMesh3& Mesh, const std::vector<int>& EdgesIn )
{
    Edges              = EdgesIn;
    const int NumEdges = static_cast<int32_t>( Edges.size() );
    Vertices.resize( NumEdges );

    const Index2i StartEV = Mesh.GetEdgeV( Edges[0] );
    Index2i       PrevEV  = StartEV;
    for ( int i = 1; i < NumEdges; ++i )
    {
        const Index2i NextEV  = Mesh.GetEdgeV( Edges[i] );
        Vertices[i]           = IndexUtil::FindSharedEdgeVertex( PrevEV, NextEV );
        PrevEV                = NextEV;
    }
    Vertices[0] = IndexUtil::FindEdgeOtherVertex( StartEV, Vertices[1] );
}

bool EdgeLoop::InitializeFromVertices( const DynamicMesh3& Mesh, const std::vector<int>& VerticesIn )
{
    Vertices              = VerticesIn;
    const int NumVertices = static_cast<int32_t>( Vertices.size() );
    Edges.resize( NumVertices );
    for ( int i = 0; i < NumVertices; ++i )
    {
        Edges[i] = Mesh.FindEdge( Vertices[i], Vertices[( i + 1 ) % NumVertices] );
        if ( Edges[i] == IndexConstants::InvalidID )
            return false;
    }
    return true;
}

bool EdgeLoop::IsBoundaryLoop( const DynamicMesh3& Mesh ) const
{
    for ( const int Eid : Edges )
    {
        if ( !Mesh.IsBoundaryEdge( Eid ) )
            return false;
    }
    return true;
}

MeshRegionBoundaryLoops::MeshRegionBoundaryLoops( const DynamicMesh3* MeshIn, const std::vector<int>& RegionTris,
                                                  bool bAutoCompute )
     : m_Mesh( MeshIn )
{
    m_Triangles.assign( m_Mesh->MaxTriangleID(), false );
    for ( const int Tid : RegionTris )
    {
        m_Triangles[Tid] = true;
    }
    m_Edges.assign( m_Mesh->MaxEdgeID(), false );
    for ( const int Tid : RegionTris )
    {
        const Index3i Te = m_Mesh->GetTriEdges( Tid );
        for ( int j = 0; j < 3; ++j )
        {
            const int Eid = Te[j];
            if ( !m_Edges[Eid] )
            {
                const Index2i Et = m_Mesh->GetEdgeT( Eid );
                if ( Et.B == IndexConstants::InvalidID || m_Triangles[Et.A] != m_Triangles[Et.B] )
                {
                    m_EdgesRoi.push_back( Eid );
                    m_Edges[Eid] = true;
                }
            }
        }
    }
    if ( bAutoCompute )
    {
        Compute();
    }
}

bool MeshRegionBoundaryLoops::Compute()
{
    m_bFailed = false;
    m_FailureReason.clear();
    m_Loops.resize( 0 );

    std::vector<bool> UsedEdge;
    UsedEdge.assign( m_Mesh->MaxEdgeID(), false );
    for ( const int Eid : m_EdgesRoi )
    {
        if ( UsedEdge[Eid] || !IsEdgeOnBoundary( Eid ) )
        {
            continue;
        }
        EdgeLoop  Loop;
        const int EStart = Eid;
        UsedEdge[EStart] = true;
        Loop.Edges.push_back( EStart );
        int  ECur       = Eid;
        int  EFirstVert = -1; // the first vertex on ECur, in walking order
        bool bClosed    = false;
        while ( !bClosed )
        {
            int TidIn  = IndexConstants::InvalidID;
            int TidOut = IndexConstants::InvalidID;
            IsEdgeOnBoundary( ECur, TidIn, TidOut );
            int CurA = 0;
            int CurB = 0;
            if ( EFirstVert == -1 )
            {
                const Index2i Ev  = GetOrientedEdgeVerts( ECur, TidIn );
                CurA              = Ev.A;
                CurB              = Ev.B;
            }
            else
            {
                const Index2i Ev  = m_Mesh->GetEdgeV( ECur );
                CurA              = EFirstVert;
                CurB              = Ev.A == CurA ? Ev.B : Ev.A;
            }
            Loop.Vertices.push_back( CurA );
            int       E0       = -1;
            int       E1       = -1;
            const int BdryNbrs = GetVertexBoundaryEdges( CurB, E0, E1 );
            if ( BdryNbrs != 2 )
            {
                m_bFailed       = true;
                m_FailureReason = "region boundary vertex " + std::to_string( CurB ) + " has " +
                                  std::to_string( BdryNbrs ) +
                                  " region-boundary edges (2 expected; bowties are refused)";
                return false;
            }
            const int ENext = ( E0 == ECur ) ? E1 : E0;
            if ( ENext == EStart )
            {
                bClosed = true;
            }
            else
            {
                assert( !UsedEdge[ENext] );
                Loop.Edges.push_back( ENext );
                ECur           = ENext;
                UsedEdge[ECur] = true;
            }
            EFirstVert = CurB;
        }
        m_Loops.push_back( std::move( Loop ) );
    }
    return !m_bFailed;
}

bool MeshRegionBoundaryLoops::IsEdgeOnBoundary( int Eid, int& TidIn, int& TidOut ) const
{
    if ( !m_Edges[Eid] )
    {
        return false;
    }
    TidIn             = IndexConstants::InvalidID;
    TidOut            = IndexConstants::InvalidID;
    const Index2i Et  = m_Mesh->GetEdgeT( Eid );
    if ( Et.B == IndexConstants::InvalidID )
    {
        TidIn = Et.A;
        return true;
    }
    const bool In0 = m_Triangles[Et.A];
    const bool In1 = m_Triangles[Et.B];
    if ( In0 != In1 )
    {
        TidIn  = In0 ? Et.A : Et.B;
        TidOut = In0 ? Et.B : Et.A;
        return true;
    }
    return false;
}

Index2i MeshRegionBoundaryLoops::GetOrientedEdgeVerts( int Eid, int TidIn ) const
{
    const Index2i  Ev  = m_Mesh->GetEdgeV( Eid );
    const Index3i  Tri = m_Mesh->GetTriangle( TidIn );
    const int      Ai  = IndexUtil::FindEdgeIndexInTri( Ev.A, Ev.B, Tri );
    return { Tri[Ai], Tri[( Ai + 1 ) % 3] };
}

int MeshRegionBoundaryLoops::GetVertexBoundaryEdges( int Vid, int& E0, int& E1 ) const
{
    int Count = 0;
    for ( const int Eid : m_Mesh->VtxEdgesItr( Vid ) )
    {
        if ( IsEdgeOnBoundary( Eid ) )
        {
            if ( Count == 0 )
            {
                E0 = Eid;
            }
            else if ( Count == 1 )
            {
                E1 = Eid;
            }
            Count++;
        }
    }
    return Count;
}

template <typename StorageType, int ElementSize, typename ElementType>
bool MeshRegionBoundaryLoops::GetLoopOverlayMap( const EdgeLoop&                                     LoopIn,
                                                 const DynamicMeshOverlay<StorageType, ElementSize>& Overlay,
                                                 VidOverlayMap<ElementType>& LoopVidsToOverlayElementsOut ) const
{
    for ( int32_t i = 0; i < static_cast<int32_t>( LoopIn.Vertices.size() ); ++i )
    {
        const int32_t Vid = LoopIn.Vertices[i];

        // the inner triangle of the edge going forward from this vertex
        int32_t TidInside  = IndexConstants::InvalidID;
        int32_t TidOutside = IndexConstants::InvalidID;
        if ( !IsEdgeOnBoundary( LoopIn.Edges[i], TidInside, TidOutside ) ||
             TidInside == IndexConstants::InvalidID )
        {
            return false;
        }

        const Index3i  TriangleVerts = m_Mesh->GetTriangle( TidInside );
        const int32_t  VidTriIndex   = TriangleVerts.IndexOf( Vid );
        if ( VidTriIndex < 0 )
        {
            return false;
        }

        const Index3i  TriangleElements = Overlay.GetTriangle( TidInside );
        const int32_t  UVElementID      = TriangleElements[VidTriIndex];
        if ( !Overlay.IsElement( UVElementID ) )
        {
            return false;
        }

        ElementType Element{};
        Overlay.GetElement( UVElementID, Element );
        LoopVidsToOverlayElementsOut.insert_or_assign( Vid,
                                                       ElementIDAndValue<ElementType>( UVElementID, Element ) );
    }
    return true;
}

template <typename StorageType, int ElementSize, typename ElementType>
void MeshRegionBoundaryLoops::UpdateLoopOverlayMapValidity(
     VidOverlayMap<ElementType>&                         LoopVidsToOverlayElements,
     const DynamicMeshOverlay<StorageType, ElementSize>& Overlay )
{
    for ( auto& Entry : LoopVidsToOverlayElements )
    {
        if ( !Overlay.IsElement( Entry.second.first ) )
        {
            Entry.second.first = IndexConstants::InvalidID;
        }
    }
}

// UV layers are the only overlays these are used for, as in UE; another layer type needs its instantiation here.
template bool MeshRegionBoundaryLoops::GetLoopOverlayMap<float, 2, glm::vec2>(
     const EdgeLoop& LoopIn, const DynamicMeshOverlay<float, 2>& Overlay,
     VidOverlayMap<glm::vec2>& LoopVidsToOverlayElementsOut ) const;
template void MeshRegionBoundaryLoops::UpdateLoopOverlayMapValidity<float, 2, glm::vec2>(
     VidOverlayMap<glm::vec2>& LoopVidsToOverlayElements, const DynamicMeshOverlay<float, 2>& Overlay );
