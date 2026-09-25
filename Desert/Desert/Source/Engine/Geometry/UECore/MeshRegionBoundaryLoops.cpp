// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/MeshRegionBoundaryLoops.cpp:14-306, 483-537 and
// Engine/Source/Runtime/GeometryCore/Private/EdgeSpan.cpp (InitializeFromVertices/InitializeFromEdges), adapted:
// UE Core via UECore.hpp, namespace Desert::Geometry, FIndexFlagSet is TArray<bool>; a bowtie vertex fails Compute
// with FailureReason (see the header) instead of entering FindLeftTurnEdge/TryExtractSubloops.
// GetLoopOverlayMap returns false where UE checks that the loop edge has an inside triangle.
#include "Engine/Geometry/UECore/MeshRegionBoundaryLoops.hpp"

#include "Engine/Geometry/UECore/IndexUtil.hpp"

using namespace Desert::Geometry;

void FEdgeSpan::InitializeFromVertices( const FDynamicMesh3& Mesh, const TArray<int>& VerticesIn )
{
    Vertices = VerticesIn;
    Edges.Reset();
    for ( int i = 0; i + 1 < Vertices.Num(); ++i )
    {
        const int Eid = Mesh.FindEdge( Vertices[i], Vertices[i + 1] );
        UE_CHECK( Eid != IndexConstants::InvalidID );
        Edges.Add( Eid );
    }
}

void FEdgeSpan::InitializeFromEdges( const FDynamicMesh3& Mesh, const TArray<int>& EdgesIn )
{
    Edges              = EdgesIn;
    const int NumEdges = Edges.Num();
    Vertices.SetNum( NumEdges + 1 );
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

void FEdgeLoop::Initialize( const TArray<int>& VerticesIn, const TArray<int>& EdgesIn,
                            const TArray<int>* BowtieVerticesIn )
{
    Vertices = VerticesIn;
    Edges    = EdgesIn;
    if ( BowtieVerticesIn != nullptr )
        BowtieVertices = *BowtieVerticesIn;
}

void FEdgeLoop::InitializeFromEdges( const FDynamicMesh3& Mesh, const TArray<int>& EdgesIn )
{
    Edges              = EdgesIn;
    const int NumEdges = Edges.Num();
    Vertices.SetNum( NumEdges );

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

bool FEdgeLoop::InitializeFromVertices( const FDynamicMesh3& Mesh, const TArray<int>& VerticesIn )
{
    Vertices              = VerticesIn;
    const int NumVertices = Vertices.Num();
    Edges.SetNum( NumVertices );
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

FMeshRegionBoundaryLoops::FMeshRegionBoundaryLoops( const FDynamicMesh3* MeshIn, const TArray<int>& RegionTris,
                                                    bool bAutoCompute )
     : Mesh( MeshIn )
{
    Triangles.Init( false, Mesh->MaxTriangleID() );
    for ( int Tid : RegionTris )
    {
        Triangles[Tid] = true;
    }
    Edges.Init( false, Mesh->MaxEdgeID() );
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
                    EdgesRoi.Add( Eid );
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
    Loops.SetNum( 0 );

    TArray<bool> UsedEdge;
    UsedEdge.Init( false, Mesh->MaxEdgeID() );
    for ( int Eid : EdgesRoi )
    {
        if ( UsedEdge[Eid] || !IsEdgeOnBoundary( Eid ) )
        {
            continue;
        }
        FEdgeLoop Loop;
        const int EStart = Eid;
        UsedEdge[EStart] = true;
        Loop.Edges.Add( EStart );
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
            Loop.Vertices.Add( CurA );
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
                Loop.Edges.Add( ENext );
                ECur           = ENext;
                UsedEdge[ECur] = true;
            }
            EFirstVert = CurB;
        }
        Loops.Add( std::move( Loop ) );
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
    for ( int32 i = 0; i < LoopIn.Vertices.Num(); ++i )
    {
        const int32 Vid = LoopIn.Vertices[i];

        // the inner triangle of the edge going forward from this vertex
        int32 TidInside  = IndexConstants::InvalidID;
        int32 TidOutside = IndexConstants::InvalidID;
        if ( !IsEdgeOnBoundary( LoopIn.Edges[i], TidInside, TidOutside ) ||
             TidInside == IndexConstants::InvalidID )
        {
            return false;
        }

        const FIndex3i TriangleVerts = Mesh->GetTriangle( TidInside );
        const int32    VidTriIndex   = TriangleVerts.IndexOf( Vid );
        if ( VidTriIndex < 0 )
        {
            return false;
        }

        const FIndex3i TriangleElements = Overlay.GetTriangle( TidInside );
        const int32    UVElementID      = TriangleElements[VidTriIndex];
        if ( !Overlay.IsElement( UVElementID ) )
        {
            return false;
        }

        ElementType Element;
        Overlay.GetElement( UVElementID, Element );
        LoopVidsToOverlayElementsOut.Add( Vid, ElementIDAndValue<ElementType>( UVElementID, Element ) );
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
        if ( !Overlay.IsElement( Entry.second.Key ) )
        {
            Entry.second.Key = IndexConstants::InvalidID;
        }
    }
}

// UV layers are the only overlays these are used for, as in UE; another layer type needs its instantiation here.
template bool FMeshRegionBoundaryLoops::GetLoopOverlayMap<float, 2, FVector2f>(
     const FEdgeLoop& LoopIn, const TDynamicMeshOverlay<float, 2>& Overlay,
     VidOverlayMap<FVector2f>& LoopVidsToOverlayElementsOut ) const;
template void FMeshRegionBoundaryLoops::UpdateLoopOverlayMapValidity<float, 2, FVector2f>(
     VidOverlayMap<FVector2f>& LoopVidsToOverlayElements, const TDynamicMeshOverlay<float, 2>& Overlay );
