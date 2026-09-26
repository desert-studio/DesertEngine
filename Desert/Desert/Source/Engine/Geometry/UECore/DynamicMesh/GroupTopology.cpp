// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/GroupTopology.cpp:12-770, 1019-1177,
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry, TBitArray is TArray<bool>; the boundary walk's
// failure is kept as FailureReason; the extra-corner hook is not ported (see the header).
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"

#include "Engine/Geometry/UECore/VectorTypes.hpp"

using namespace Desert::Geometry;

bool FGroupTopology::FGroupEdge::IsConnectedToVertices( const TSet<int>& Vertices ) const
{
    for ( int VertexID : Span.Vertices )
    {
        if ( Vertices.Contains( VertexID ) )
        {
            return true;
        }
    }
    return false;
}

FGroupTopology::FGroupTopology( const FDynamicMesh3* MeshIn, bool bAutoBuild ) : Mesh( MeshIn )
{
    if ( bAutoBuild )
    {
        RebuildTopology();
    }
}

bool FGroupTopology::RebuildTopology()
{
    Groups.Reset();
    Edges.Reset();
    Corners.Reset();
    FailureReason.clear();

    int32_t MaxGroupID = 0;
    for ( int32_t const Tid : Mesh->TriangleIndicesItr() )
    {
        MaxGroupID = std::max( GetGroupID( Tid ), MaxGroupID );
    }
    MaxGroupID++;
    GroupIDToGroupIndexMap.Reset();
    GroupIDToGroupIndexMap.Init( -1, MaxGroupID );
    for ( int Tid : Mesh->TriangleIndicesItr() )
    {
        const int GroupID = GetGroupID( Tid );
        if ( GroupIDToGroupIndexMap[GroupID] == -1 )
        {
            FGroup NewGroup;
            NewGroup.GroupID                = GroupID;
            GroupIDToGroupIndexMap[GroupID] = Groups.Add( NewGroup );
        }
        Groups[GroupIDToGroupIndexMap[GroupID]].Triangles.Add( Tid );
    }

    VertexIDToCornerIDMap = TMap<int32_t, int32_t>();
    TMap<int32_t, int32_t> GroupEdgeMinEidToGroupEdgeID;
    TArray<bool>       VertCheckedForCorner;
    VertCheckedForCorner.Init( false, Mesh->MaxVertexID() );
    for ( FGroup& Group : Groups )
    {
        if ( !GenerateBoundaryAndGroupEdges( Group, GroupEdgeMinEidToGroupEdgeID, VertCheckedForCorner ) )
        {
            return false;
        }
        for ( FGroupBoundary& Boundary : Group.Boundaries )
        {
            Boundary.bIsOnBoundary = false;
            for ( int EdgeIndex : Boundary.GroupEdges )
            {
                const FGroupEdge& Edge = Edges[EdgeIndex];
                const int OtherGroupID = ( Edge.Groups.A == Group.GroupID ) ? Edge.Groups.B : Edge.Groups.A;
                if ( OtherGroupID != FDynamicMesh3::InvalidID )
                {
                    Boundary.NeighbourGroupIDs.AddUnique( OtherGroupID );
                }
                else
                {
                    Boundary.bIsOnBoundary = true;
                }
            }
        }
        for ( const FGroupBoundary& Boundary : Group.Boundaries )
        {
            for ( int NbrGroupID : Boundary.NeighbourGroupIDs )
            {
                Group.NeighbourGroupIDs.AddUnique( NbrGroupID );
            }
        }
    }
    return true;
}

bool FGroupTopology::ShouldVertBeCorner( int VertexID ) const
{
    int NumGroupEdges = 0;
    for ( int32_t const Eid : Mesh->VtxEdgesItr( VertexID ) )
    {
        const FIndex2i EdgeTris = Mesh->GetEdgeT( Eid );
        if ( EdgeTris.B == IndexConstants::InvalidID || GetGroupID( EdgeTris.A ) != GetGroupID( EdgeTris.B ) )
        {
            if ( ++NumGroupEdges > 2 )
            {
                return true;
            }
        }
    }
    return false;
}

bool FGroupTopology::GenerateBoundaryAndGroupEdges( FGroup&                 Group,
                                                    TMap<int32_t, int32_t>& GroupEdgeMinEidToGroupEdgeID,
                                                    TArray<bool>&           VertCheckedForCorner )
{
    FMeshRegionBoundaryLoops BdryLoops( Mesh, Group.Triangles, true );
    if ( BdryLoops.bFailed )
    {
        FailureReason = "group " + std::to_string( Group.GroupID ) + ": " + BdryLoops.FailureReason;
        return false;
    }

    auto CheckForCornerAndCreateIfNeeded = [this, &VertCheckedForCorner]( int32_t Vid )
    {
        if ( VertCheckedForCorner[Vid] )
        {
            return VertexIDToCornerIDMap.Contains( Vid );
        }
        VertCheckedForCorner[Vid] = true;
        if ( !ShouldVertBeCorner( Vid ) )
        {
            return false;
        }
        const int32_t CornerID     = Corners.Emplace();
        Corners[CornerID].VertexID = Vid;
        GetAllVertexGroups( Vid, Corners[CornerID].NeighbourGroupIDs );
        VertexIDToCornerIDMap.Add( Vid, CornerID );
        return true;
    };

    const int NumLoops = BdryLoops.Loops.Num();
    Group.Boundaries.SetNum( NumLoops );
    for ( int Li = 0; Li < NumLoops; ++Li )
    {
        const FEdgeLoop& Loop     = BdryLoops.Loops[Li];
        FGroupBoundary&  Boundary = Group.Boundaries[Li];

        TArray<int> CornerIndices;
        const int   NumV = Loop.Vertices.Num();
        for ( int i = 0; i < NumV; ++i )
        {
            if ( CheckForCornerAndCreateIfNeeded( Loop.Vertices[i] ) )
            {
                CornerIndices.Add( i );
            }
        }

        // A loop with no corners is one closed group edge, identified by its smallest mesh edge ID.
        if ( CornerIndices.Num() == 0 )
        {
            const int32_t  MinEid    = *std::min_element( Loop.Edges.begin(), Loop.Edges.end() );
            const int32_t* Existing  = GroupEdgeMinEidToGroupEdgeID.Find( MinEid );
            int          EdgeIndex = Existing ? *Existing : IndexConstants::InvalidID;
            if ( EdgeIndex == IndexConstants::InvalidID )
            {
                FGroupEdge Edge;
                Edge.Groups = MakeEdgeGroupsPair( Loop.Edges[0] );
                Edge.Span.InitializeFromEdges( *Mesh, Loop.Edges );
                Edge.EndpointCorners = FIndex2i( IndexConstants::InvalidID, IndexConstants::InvalidID );
                EdgeIndex            = Edges.Add( Edge );
                GroupEdgeMinEidToGroupEdgeID.Add( MinEid, EdgeIndex );
            }
            Boundary.GroupEdges.Add( EdgeIndex );
            continue;
        }

        const int NumSpans = CornerIndices.Num();
        CornerIndices.Add( CornerIndices[0] );
        for ( int k = 0; k < NumSpans; ++k )
        {
            const int32_t StartIndex   = CornerIndices[k];
            const int32_t EndIndex     = CornerIndices[k + 1]; // equal on a loop with one corner
            int32_t       NumSpanEdges = ( EndIndex + NumV - StartIndex ) % NumV;
            if ( NumSpanEdges == 0 )
            {
                NumSpanEdges = NumV;
            }
            int32_t MinEid = Loop.Edges[StartIndex];
            for ( int32_t i = 1; i < NumSpanEdges; ++i )
            {
                MinEid = std::min( MinEid, Loop.Edges[( StartIndex + i ) % NumV] );
            }
            if ( const int32_t* Existing = GroupEdgeMinEidToGroupEdgeID.Find( MinEid ) )
            {
                Boundary.GroupEdges.Add( *Existing );
                continue;
            }
            FGroupEdge Edge;
            Edge.Groups = MakeEdgeGroupsPair( Loop.Edges[StartIndex] );
            TArray<int> SpanVertices;
            for ( int32_t i = 0; i < NumSpanEdges + 1; ++i )
            {
                SpanVertices.Add( Loop.Vertices[( StartIndex + i ) % NumV] );
            }
            Edge.Span.InitializeFromVertices( *Mesh, SpanVertices );
            Edge.EndpointCorners = FIndex2i( GetCornerIDFromVertexID( SpanVertices[0] ),
                                             GetCornerIDFromVertexID( SpanVertices.Last() ) );
            UE_CHECK( Edge.EndpointCorners.A != IndexConstants::InvalidID &&
                      Edge.EndpointCorners.B != IndexConstants::InvalidID );
            const int EdgeIndex = Edges.Add( Edge );
            Boundary.GroupEdges.Add( EdgeIndex );
            GroupEdgeMinEidToGroupEdgeID.Add( MinEid, EdgeIndex );
        }
    }
    return true;
}

FIndex2i FGroupTopology::MakeEdgeGroupsPair( int MeshEdgeID ) const
{
    const FIndex2i EdgeTris = Mesh->GetEdgeT( MeshEdgeID );
    const int      G0       = GetGroupID( EdgeTris.A );
    if ( EdgeTris.B == IndexConstants::InvalidID )
    {
        return FIndex2i( G0, IndexConstants::InvalidID );
    }
    const int G1 = GetGroupID( EdgeTris.B );
    return ( G0 < G1 ) ? FIndex2i( G0, G1 ) : FIndex2i( G1, G0 );
}

int FGroupTopology::GetCornerVertexID( int CornerID ) const
{
    UE_CHECK( CornerID >= 0 && CornerID < Corners.Num() );
    return Corners[CornerID].VertexID;
}

int32_t FGroupTopology::GetCornerIDFromVertexID( int32_t VertexID ) const
{
    const int32_t* Found = VertexIDToCornerIDMap.Find( VertexID );
    return ( Found == nullptr ) ? IndexConstants::InvalidID : *Found;
}

const FGroupTopology::FGroup* FGroupTopology::FindGroupByID( int GroupID ) const
{
    if ( GroupID < 0 || GroupID >= GroupIDToGroupIndexMap.Num() || GroupIDToGroupIndexMap[GroupID] == -1 )
    {
        return nullptr;
    }
    return &Groups[GroupIDToGroupIndexMap[GroupID]];
}

const TArray<int>& FGroupTopology::GetGroupTriangles( int GroupID ) const
{
    const FGroup* Found = FindGroupByID( GroupID );
    return ( Found != nullptr ) ? Found->Triangles : EmptyArray;
}

const TArray<int>& FGroupTopology::GetGroupNbrGroups( int GroupID ) const
{
    const FGroup* Found = FindGroupByID( GroupID );
    return ( Found != nullptr ) ? Found->NeighbourGroupIDs : EmptyArray;
}

int FGroupTopology::FindGroupEdgeID( int MeshEdgeID ) const
{
    const FGroup* Group = FindGroupByID( GetGroupID( Mesh->GetEdgeT( MeshEdgeID ).A ) );
    if ( Group != nullptr )
    {
        for ( const FGroupBoundary& Boundary : Group->Boundaries )
        {
            for ( int EdgeID : Boundary.GroupEdges )
            {
                if ( Edges[EdgeID].Span.Edges.Contains( MeshEdgeID ) )
                {
                    return EdgeID;
                }
            }
        }
    }
    return -1;
}

const TArray<int>& FGroupTopology::GetGroupEdgeVertices( int GroupEdgeID ) const
{
    UE_CHECK( GroupEdgeID >= 0 && GroupEdgeID < Edges.Num() );
    return Edges[GroupEdgeID].Span.Vertices;
}

const TArray<int>& FGroupTopology::GetGroupEdgeEdges( int GroupEdgeID ) const
{
    UE_CHECK( GroupEdgeID >= 0 && GroupEdgeID < Edges.Num() );
    return Edges[GroupEdgeID].Span.Edges;
}

void FGroupTopology::FindEdgeNbrGroups( int GroupEdgeID, TArray<int>& GroupsOut ) const
{
    const TArray<int>& Vertices = GetGroupEdgeVertices( GroupEdgeID );
    FindVertexNbrGroups( Vertices[0], GroupsOut );
    FindVertexNbrGroups( Vertices[Vertices.Num() - 1], GroupsOut );
}

void FGroupTopology::FindEdgeNbrEdges( int GroupEdgeID, TArray<int>& EdgesOut ) const
{
    UE_CHECK( GroupEdgeID >= 0 && GroupEdgeID < Edges.Num() );
    const FGroupEdge& Edge = Edges[GroupEdgeID];
    if ( Edge.EndpointCorners.A != IndexConstants::InvalidID )
    {
        FindCornerNbrEdges( Edge.EndpointCorners.A, EdgesOut );
    }
    if ( Edge.EndpointCorners.B != IndexConstants::InvalidID )
    {
        FindCornerNbrEdges( Edge.EndpointCorners.B, EdgesOut );
    }
}

// UE GroupTopology.cpp:357-377.
double FGroupTopology::GetEdgeArcLength( int32_t GroupEdgeID, TArray<double>* PerVertexLengthsOut ) const
{
    UE_CHECK( GroupEdgeID >= 0 && GroupEdgeID < Edges.Num() );
    const TArray<int>& Vertices = GetGroupEdgeVertices( GroupEdgeID );
    const int32_t      NumV     = Vertices.Num();
    if ( PerVertexLengthsOut != nullptr )
    {
        PerVertexLengthsOut->SetNum( NumV );
        ( *PerVertexLengthsOut )[0] = 0.0;
    }
    double AccumLength = 0;
    for ( int32_t k = 1; k < NumV; ++k )
    {
        AccumLength += Distance( Mesh->GetVertex( Vertices[k] ), Mesh->GetVertex( Vertices[k - 1] ) );
        if ( PerVertexLengthsOut != nullptr )
        {
            ( *PerVertexLengthsOut )[k] = AccumLength;
        }
    }
    return AccumLength;
}

bool FGroupTopology::IsBoundaryEdge( int32_t GroupEdgeID ) const
{
    return Mesh->IsBoundaryEdge( Edges[GroupEdgeID].Span.Edges[0] );
}

bool FGroupTopology::IsSimpleGroupEdge( int32_t GroupEdgeID ) const
{
    return Edges[GroupEdgeID].Span.Edges.Num() == 1;
}

bool FGroupTopology::IsIsolatedLoop( int32_t GroupEdgeID ) const
{
    return Edges[GroupEdgeID].EndpointCorners.A == IndexConstants::InvalidID;
}

void FGroupTopology::FindCornerNbrGroups( int CornerID, TArray<int>& GroupsOut ) const
{
    UE_CHECK( CornerID >= 0 && CornerID < Corners.Num() );
    for ( int GroupID : Corners[CornerID].NeighbourGroupIDs )
    {
        GroupsOut.AddUnique( GroupID );
    }
}

void FGroupTopology::ForCornerNbrEdges( int                                          CornerID,
                                        const std::function<bool( int32_t EdgeID )>& ReturnTrueToContinue ) const
{
    UE_CHECK( CornerID >= 0 && CornerID < Corners.Num() );
    TSet<int32_t> ProcessedEdges;
    for ( int GroupID : Corners[CornerID].NeighbourGroupIDs )
    {
        for ( const FGroupBoundary& Boundary : FindGroupByID( GroupID )->Boundaries )
        {
            for ( int32_t const EdgeID : Boundary.GroupEdges )
            {
                const FGroupEdge& Edge = Edges[EdgeID];
                if ( ( Edge.EndpointCorners.A != CornerID && Edge.EndpointCorners.B != CornerID ) ||
                     ProcessedEdges.Contains( EdgeID ) )
                {
                    continue;
                }
                ProcessedEdges.Add( EdgeID );
                if ( !ReturnTrueToContinue( EdgeID ) )
                {
                    return;
                }
            }
        }
    }
}

void FGroupTopology::FindCornerNbrEdges( int CornerID, TArray<int>& EdgesOut ) const
{
    ForCornerNbrEdges( CornerID, [&EdgesOut]( int32_t EdgeID ) { return EdgesOut.Add( EdgeID ) >= 0; } );
}

void FGroupTopology::FindCornerNbrCorners( int CornerID, TArray<int>& CornersOut ) const
{
    ForCornerNbrEdges( CornerID,
                       [&]( int32_t EdgeID )
                       {
                           CornersOut.AddUnique( Edges[EdgeID].EndpointCorners.OtherElement( CornerID ) );
                           return true;
                       } );
}

void FGroupTopology::FindVertexNbrGroups( int VertexID, TArray<int>& GroupsOut ) const
{
    for ( int Tid : Mesh->VtxTrianglesItr( VertexID ) )
    {
        GroupsOut.AddUnique( GetGroupID( Tid ) );
    }
}

void FGroupTopology::CollectGroupVertices( int GroupID, TSet<int>& Vertices ) const
{
    for ( int TriID : GetGroupTriangles( GroupID ) )
    {
        const FIndex3i TriVerts = Mesh->GetTriangle( TriID );
        Vertices.Add( TriVerts.A );
        Vertices.Add( TriVerts.B );
        Vertices.Add( TriVerts.C );
    }
}

void FGroupTopology::CollectGroupBoundaryVertices( int GroupID, TSet<int>& Vertices ) const
{
    const FGroup* Group = FindGroupByID( GroupID );
    if ( Group == nullptr )
    {
        return;
    }
    for ( const FGroupBoundary& Boundary : Group->Boundaries )
    {
        for ( int EdgeIndex : Boundary.GroupEdges )
        {
            for ( int Vid : Edges[EdgeIndex].Span.Vertices )
            {
                Vertices.Add( Vid );
            }
        }
    }
}

void FGroupTopology::GetSelectedTriangles( const FGroupTopologySelection& Selection,
                                           TArray<int32_t>&               Triangles ) const
{
    for ( int32_t const GroupID : Selection.SelectedGroupIDs )
    {
        for ( int32_t const TriangleID : GetGroupTriangles( GroupID ) )
        {
            Triangles.Add( TriangleID );
        }
    }
}

void FGroupTopology::GetAllVertexGroups( int32_t VertexID, TArray<int32_t>& GroupsOut ) const
{
    for ( int32_t const EdgeID : Mesh->VtxEdgesItr( VertexID ) )
    {
        const FIndex2i EdgeTris = Mesh->GetEdgeT( EdgeID );
        GroupsOut.AddUnique( GetGroupID( EdgeTris.A ) );
        if ( EdgeTris.B != FDynamicMesh3::InvalidID )
        {
            GroupsOut.AddUnique( GetGroupID( EdgeTris.B ) );
        }
    }
}

FTriangleGroupTopology::FTriangleGroupTopology( const FDynamicMesh3* MeshIn, bool bAutoBuild )
     : FGroupTopology( MeshIn, false )
{
    if ( bAutoBuild )
    {
        RebuildTopology();
    }
}

bool FTriangleGroupTopology::RebuildTopology()
{
    Groups.Reset();
    Edges.Reset();
    Corners.Reset();
    FailureReason.clear();
    GroupIDToGroupIndexMap.Reset();
    GroupIDToGroupIndexMap.Init( -1, Mesh->MaxTriangleID() );
    for ( int Tid : Mesh->TriangleIndicesItr() )
    {
        FGroup NewGroup;
        NewGroup.GroupID = Tid;
        NewGroup.Triangles.Add( Tid );
        GroupIDToGroupIndexMap[Tid] = Groups.Add( NewGroup );
    }
    VertexIDToCornerIDMap = TMap<int32_t, int32_t>();
    for ( int Vid : Mesh->VertexIndicesItr() )
    {
        FCorner Corner;
        Corner.VertexID = Vid;
        VertexIDToCornerIDMap.Add( Vid, Corners.Add( Corner ) );
    }
    for ( FCorner& Corner : Corners )
    {
        GetAllVertexGroups( Corner.VertexID, Corner.NeighbourGroupIDs );
    }
    TArray<int32_t> MeshEdgeToGroupEdge;
    MeshEdgeToGroupEdge.Init( INDEX_NONE, Mesh->MaxEdgeID() );
    for ( FGroup& Group : Groups )
    {
        Group.Boundaries.SetNum( 1 );
        FGroupBoundary& Boundary0 = Group.Boundaries[0];
        const FIndex3i  TriEdges  = Mesh->GetTriEdges( Group.GroupID );
        for ( int j = 0; j < 3; ++j )
        {
            int& GroupEdgeIndex = MeshEdgeToGroupEdge[TriEdges[j]];
            if ( GroupEdgeIndex == INDEX_NONE )
            {
                FGroupEdge NewGroupEdge;
                NewGroupEdge.Groups      = MakeEdgeGroupsPair( TriEdges[j] );
                const FIndex2i EdgeVerts = Mesh->GetEdgeV( TriEdges[j] );
                TArray<int>    SpanVertices;
                SpanVertices.Add( EdgeVerts.A );
                SpanVertices.Add( EdgeVerts.B );
                NewGroupEdge.Span.InitializeFromVertices( *Mesh, SpanVertices );
                NewGroupEdge.EndpointCorners =
                     FIndex2i( GetCornerIDFromVertexID( EdgeVerts.A ), GetCornerIDFromVertexID( EdgeVerts.B ) );
                GroupEdgeIndex = Edges.Add( NewGroupEdge );
            }
            Boundary0.GroupEdges.Add( GroupEdgeIndex );
        }
        const FIndex3i TriNbrTris = Mesh->GetTriNeighbourTris( Group.GroupID );
        for ( int j = 0; j < 3; ++j )
        {
            if ( TriNbrTris[j] != FDynamicMesh3::InvalidID )
            {
                Group.NeighbourGroupIDs.Add( TriNbrTris[j] );
                Boundary0.NeighbourGroupIDs.Add( TriNbrTris[j] );
            }
            else
            {
                Boundary0.bIsOnBoundary = true;
            }
        }
    }
    return true;
}
