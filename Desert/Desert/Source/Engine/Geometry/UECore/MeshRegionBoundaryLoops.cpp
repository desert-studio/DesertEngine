// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/MeshRegionBoundaryLoops.cpp:14-306, 483-537 and
// Engine/Source/Runtime/GeometryCore/Private/EdgeSpan.cpp (InitializeFromVertices/InitializeFromEdges), adapted:
// UE Core via UECore.hpp, namespace Desert::Geometry, FIndexFlagSet is TArray<bool>; a bowtie vertex fails Compute
// with FailureReason (see the header) instead of entering FindLeftTurnEdge/TryExtractSubloops.
// GetLoopOverlayMap returns false where UE checks that the loop edge has an inside triangle.
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include "Engine/Geometry/UECore/IndexUtil.hpp"

using namespace Desert::Geometry;

void FEdgeSpan::InitializeFromVertices( const FDynamicMesh3& Mesh, const std::vector<int>& VerticesIn )
{
    Vertices = VerticesIn;
    Edges.clear();
    for ( int i = 0; i + 1 < static_cast<int32_t>( Vertices.size() ); ++i )
    {
        const int Eid = Mesh.FindEdge( Vertices[i], Vertices[i + 1] );
        UE_CHECK( Eid != IndexConstants::InvalidID );
        Edges.push_back( Eid );
    }
}

void FEdgeSpan::InitializeFromEdges( const FDynamicMesh3& Mesh, const std::vector<int>& EdgesIn )
{
    Edges              = EdgesIn;
    const int NumEdges = static_cast<int32_t>( Edges.size() );
    Vertices.resize( NumEdges + 1 );
    const FIndex2i StartEv = Mesh.GetEdgeV( Edges[0] );
    FIndex2i       PrevEv  = StartEv;
    for ( int i = 1; i < NumEdges; ++i )
    {
        const FIndex2i NextEv = Mesh.GetEdgeV( Edges[i] );
        Vertices[i]           = IndexUtil::FindSharedEdgeVertex( PrevEv, NextEv );
        PrevEv                = NextEv;
    }
    Vertices[0]        = IndexUtil::FindEdgeOtherVertex( StartEv, Vertices[1] );
    Vertices[NumEdges] = IndexUtil::FindEdgeOtherVertex( PrevEv, Vertices[NumEdges - 1] );
}

void FEdgeLoop::Initialize( const std::vector<int>& VerticesIn, const std::vector<int>& EdgesIn,
                            const std::vector<int>* BowtieVerticesIn )
{
    Vertices = VerticesIn;
    Edges    = EdgesIn;
    if ( BowtieVerticesIn != nullptr )
        BowtieVertices = *BowtieVerticesIn;
}

void FEdgeLoop::InitializeFromEdges( const FDynamicMesh3& Mesh, const std::vector<int>& EdgesIn )
{
    Edges              = EdgesIn;
    const int NumEdges = static_cast<int32_t>( Edges.size() );
    Vertices.resize( NumEdges );

    const FIndex2i StartEV = Mesh.GetEdgeV( Edges[0] );
    FIndex2i       PrevEV  = StartEV;
    for ( int i = 1; i < NumEdges; ++i )
    {
        const FIndex2i NextEV = Mesh.GetEdgeV( Edges[i] );
        Vertices[i]           = IndexUtil::FindSharedEdgeVertex( PrevEV, NextEV );
        PrevEV                = NextEV;
    }
    Vertices[0] = IndexUtil::FindEdgeOtherVertex( StartEV, Vertices[1] );
}

bool FEdgeLoop::InitializeFromVertices( const FDynamicMesh3& Mesh, const std::vector<int>& VerticesIn )
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

bool FEdgeLoop::IsBoundaryLoop( const FDynamicMesh3& Mesh ) const
{
    for ( int Eid : Edges )
    {
        if ( !Mesh.IsBoundaryEdge( Eid ) )
            return false;
    }
    return true;
}

FMeshRegionBoundaryLoops::FMeshRegionBoundaryLoops( const FDynamicMesh3*    MeshIn,
                                                    const std::vector<int>& RegionTris, bool bAutoCompute )
     : Mesh( MeshIn )
{
    Triangles.assign( Mesh->MaxTriangleID(), false );
    for ( int Tid : RegionTris )
    {
        Triangles[Tid] = true;
    }
    Edges.assign( Mesh->MaxEdgeID(), false );
    for ( int Tid : RegionTris )
    {
        const FIndex3i Te = Mesh->GetTriEdges( Tid );
        for ( int j = 0; j < 3; ++j )
        {
            const int Eid = Te[j];
            if ( !Edges[Eid] )
            {
                const FIndex2i Et = Mesh->GetEdgeT( Eid );
                if ( Et.B == IndexConstants::InvalidID || Triangles[Et.A] != Triangles[Et.B] )
                {
                    EdgesRoi.push_back( Eid );
                    Edges[Eid] = true;
                }
            }
        }
    }
    if ( bAutoCompute )
    {
        Compute();
    }
}

bool FMeshRegionBoundaryLoops::Compute()
{
    bFailed = false;
    FailureReason.clear();
    Loops.resize( 0 );

    std::vector<bool> UsedEdge;
    UsedEdge.assign( Mesh->MaxEdgeID(), false );
    for ( int Eid : EdgesRoi )
    {
        if ( UsedEdge[Eid] || !IsEdgeOnBoundary( Eid ) )
        {
            continue;
        }
        FEdgeLoop Loop;
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
                const FIndex2i Ev = GetOrientedEdgeVerts( ECur, TidIn );
                CurA              = Ev.A;
                CurB              = Ev.B;
            }
            else
            {
                const FIndex2i Ev = Mesh->GetEdgeV( ECur );
                CurA              = EFirstVert;
                CurB              = Ev.A == CurA ? Ev.B : Ev.A;
            }
            Loop.Vertices.push_back( CurA );
            int       E0       = -1;
            int       E1       = -1;
            const int BdryNbrs = GetVertexBoundaryEdges( CurB, E0, E1 );
            if ( BdryNbrs != 2 )
            {
                bFailed       = true;
                FailureReason = "region boundary vertex " + std::to_string( CurB ) + " has " +
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
                UE_CHECK( !UsedEdge[ENext] );
                Loop.Edges.push_back( ENext );
                ECur           = ENext;
                UsedEdge[ECur] = true;
            }
            EFirstVert = CurB;
        }
        Loops.push_back( std::move( Loop ) );
    }
    return !bFailed;
}

bool FMeshRegionBoundaryLoops::IsEdgeOnBoundary( int Eid, int& TidIn, int& TidOut ) const
{
    if ( !Edges[Eid] )
    {
        return false;
    }
    TidIn             = IndexConstants::InvalidID;
    TidOut            = IndexConstants::InvalidID;
    const FIndex2i Et = Mesh->GetEdgeT( Eid );
    if ( Et.B == IndexConstants::InvalidID )
    {
        TidIn = Et.A;
        return true;
    }
    const bool In0 = Triangles[Et.A];
    const bool In1 = Triangles[Et.B];
    if ( In0 != In1 )
    {
        TidIn  = In0 ? Et.A : Et.B;
        TidOut = In0 ? Et.B : Et.A;
        return true;
    }
    return false;
}

FIndex2i FMeshRegionBoundaryLoops::GetOrientedEdgeVerts( int Eid, int TidIn ) const
{
    const FIndex2i Ev  = Mesh->GetEdgeV( Eid );
    const FIndex3i Tri = Mesh->GetTriangle( TidIn );
    const int      Ai  = IndexUtil::FindEdgeIndexInTri( Ev.A, Ev.B, Tri );
    return FIndex2i( Tri[Ai], Tri[( Ai + 1 ) % 3] );
}

int FMeshRegionBoundaryLoops::GetVertexBoundaryEdges( int Vid, int& E0, int& E1 ) const
{
    int Count = 0;
    for ( int Eid : Mesh->VtxEdgesItr( Vid ) )
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
bool FMeshRegionBoundaryLoops::GetLoopOverlayMap( const FEdgeLoop&                                     LoopIn,
                                                  const TDynamicMeshOverlay<StorageType, ElementSize>& Overlay,
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

        const FIndex3i TriangleVerts = Mesh->GetTriangle( TidInside );
        const int32_t  VidTriIndex   = TriangleVerts.IndexOf( Vid );
        if ( VidTriIndex < 0 )
        {
            return false;
        }

        const FIndex3i TriangleElements = Overlay.GetTriangle( TidInside );
        const int32_t  UVElementID      = TriangleElements[VidTriIndex];
        if ( !Overlay.IsElement( UVElementID ) )
        {
            return false;
        }

        ElementType Element;
        Overlay.GetElement( UVElementID, Element );
        LoopVidsToOverlayElementsOut.insert_or_assign( Vid,
                                                       ElementIDAndValue<ElementType>( UVElementID, Element ) );
    }
    return true;
}

template <typename StorageType, int ElementSize, typename ElementType>
void FMeshRegionBoundaryLoops::UpdateLoopOverlayMapValidity(
     VidOverlayMap<ElementType>&                          LoopVidsToOverlayElements,
     const TDynamicMeshOverlay<StorageType, ElementSize>& Overlay )
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
template bool FMeshRegionBoundaryLoops::GetLoopOverlayMap<float, 2, glm::vec2>(
     const FEdgeLoop& LoopIn, const TDynamicMeshOverlay<float, 2>& Overlay,
     VidOverlayMap<glm::vec2>& LoopVidsToOverlayElementsOut ) const;
template void FMeshRegionBoundaryLoops::UpdateLoopOverlayMapValidity<float, 2, glm::vec2>(
     VidOverlayMap<glm::vec2>& LoopVidsToOverlayElements, const TDynamicMeshOverlay<float, 2>& Overlay );
