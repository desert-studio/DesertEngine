// Ported from UE 5.8
// Engine/Plugins/Runtime/GeometryProcessing/Source/DynamicMesh/Private/GroupTopology.cpp:12-770, 1019-1177,
// adapted: UE Core via UECore.hpp, namespace Desert::Geometry, TBitArray is TArray<bool>; the boundary walk's
// failure is kept as FailureReason; the extra-corner hook is not ported (see the header).
#include "Engine/Geometry/UECore/DynamicMesh/GroupTopology.hpp"
#include "Engine/Geometry/UECore/MapLookup.hpp"

#include "Engine/Geometry/UECore/VectorTypes.hpp"

using namespace Desert::Geometry;

bool GroupTopology::GroupEdge::IsConnectedToVertices( const std::unordered_set<int>& Vertices ) const
{
    for ( int VertexID : Span.Vertices )
    {
        if ( Vertices.contains( VertexID ) )
        {
            return true;
        }
    }
    return false;
}

GroupTopology::GroupTopology( const DynamicMesh3* MeshIn, bool bAutoBuild ) : Mesh( MeshIn )
{
    if ( bAutoBuild )
    {
        RebuildTopology();
    }
}

bool GroupTopology::RebuildTopology()
{
    Groups.clear();
    Edges.clear();
    Corners.clear();
    FailureReason.clear();

    int32_t MaxGroupID = 0;
    for ( int32_t const Tid : Mesh->TriangleIndicesItr() )
    {
        MaxGroupID = std::max( GetGroupID( Tid ), MaxGroupID );
    }
    MaxGroupID++;
    GroupIDToGroupIndexMap.clear();
    GroupIDToGroupIndexMap.assign( MaxGroupID, -1 );
    for ( int Tid : Mesh->TriangleIndicesItr() )
    {
        const int GroupID = GetGroupID( Tid );
        if ( GroupIDToGroupIndexMap[GroupID] == -1 )
        {
            Group NewGroup;
            NewGroup.GroupID                = GroupID;
            Groups.push_back( NewGroup );
            GroupIDToGroupIndexMap[GroupID] = static_cast<int32_t>( Groups.size() ) - 1;
        }
        Groups[GroupIDToGroupIndexMap[GroupID]].Triangles.push_back( Tid );
    }

    VertexIDToCornerIDMap = std::unordered_map<int32_t, int32_t>();
    std::unordered_map<int32_t, int32_t> GroupEdgeMinEidToGroupEdgeID;
    std::vector<bool>                    VertCheckedForCorner;
    VertCheckedForCorner.assign( Mesh->MaxVertexID(), false );
    for ( Group& Group : Groups )
    {
        if ( !GenerateBoundaryAndGroupEdges( Group, GroupEdgeMinEidToGroupEdgeID, VertCheckedForCorner ) )
        {
            return false;
        }
        for ( GroupBoundary& Boundary : Group.Boundaries )
        {
            Boundary.bIsOnBoundary = false;
            for ( int EdgeIndex : Boundary.GroupEdges )
            {
                const GroupEdge& Edge         = Edges[EdgeIndex];
                const int OtherGroupID = ( Edge.Groups.A == Group.GroupID ) ? Edge.Groups.B : Edge.Groups.A;
                if ( OtherGroupID != DynamicMesh3::InvalidID )
                {
                    if ( std::find( Boundary.NeighbourGroupIDs.begin(), Boundary.NeighbourGroupIDs.end(),
                                    OtherGroupID ) == Boundary.NeighbourGroupIDs.end() )
                    {
                        Boundary.NeighbourGroupIDs.push_back( OtherGroupID );
                    }
                }
                else
                {
                    Boundary.bIsOnBoundary = true;
                }
            }
        }
        for ( const GroupBoundary& Boundary : Group.Boundaries )
        {
            for ( int NbrGroupID : Boundary.NeighbourGroupIDs )
            {
                if ( std::find( Group.NeighbourGroupIDs.begin(), Group.NeighbourGroupIDs.end(), NbrGroupID ) ==
                     Group.NeighbourGroupIDs.end() )
                {
                    Group.NeighbourGroupIDs.push_back( NbrGroupID );
                }
            }
        }
    }
    return true;
}

bool GroupTopology::ShouldVertBeCorner( int VertexID ) const
{
    int NumGroupEdges = 0;
    for ( int32_t const Eid : Mesh->VtxEdgesItr( VertexID ) )
    {
        const Index2i EdgeTris = Mesh->GetEdgeT( Eid );
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

bool GroupTopology::GenerateBoundaryAndGroupEdges(
     Group& Group, std::unordered_map<int32_t, int32_t>& GroupEdgeMinEidToGroupEdgeID,
     std::vector<bool>& VertCheckedForCorner )
{
    MeshRegionBoundaryLoops BdryLoops( Mesh, Group.Triangles, true );
    if ( BdryLoops.bFailed )
    {
        FailureReason = "group " + std::to_string( Group.GroupID ) + ": " + BdryLoops.FailureReason;
        return false;
    }

    auto CheckForCornerAndCreateIfNeeded = [this, &VertCheckedForCorner]( int32_t Vid )
    {
        if ( VertCheckedForCorner[Vid] )
        {
            return VertexIDToCornerIDMap.contains( Vid );
        }
        VertCheckedForCorner[Vid] = true;
        if ( !ShouldVertBeCorner( Vid ) )
        {
            return false;
        }
        Corners.emplace_back();
        const int32_t CornerID     = static_cast<int32_t>( Corners.size() ) - 1;
        Corners[CornerID].VertexID = Vid;
        GetAllVertexGroups( Vid, Corners[CornerID].NeighbourGroupIDs );
        VertexIDToCornerIDMap.insert_or_assign( Vid, CornerID );
        return true;
    };

    const int NumLoops = static_cast<int32_t>( BdryLoops.Loops.size() );
    Group.Boundaries.resize( NumLoops );
    for ( int Li = 0; Li < NumLoops; ++Li )
    {
        const EdgeLoop& Loop     = BdryLoops.Loops[Li];
        GroupBoundary&  Boundary = Group.Boundaries[Li];

        std::vector<int> CornerIndices;
        const int        NumV = static_cast<int32_t>( Loop.Vertices.size() );
        for ( int i = 0; i < NumV; ++i )
        {
            if ( CheckForCornerAndCreateIfNeeded( Loop.Vertices[i] ) )
            {
                CornerIndices.push_back( i );
            }
        }

        // A loop with no corners is one closed group edge, identified by its smallest mesh edge ID.
        if ( CornerIndices.empty() )
        {
            const int32_t  MinEid    = *std::min_element( Loop.Edges.begin(), Loop.Edges.end() );
            const int32_t* Existing  = FindValue( GroupEdgeMinEidToGroupEdgeID, MinEid );
            int          EdgeIndex = Existing ? *Existing : IndexConstants::InvalidID;
            if ( EdgeIndex == IndexConstants::InvalidID )
            {
                GroupEdge Edge;
                Edge.Groups = MakeEdgeGroupsPair( Loop.Edges[0] );
                Edge.Span.InitializeFromEdges( *Mesh, Loop.Edges );
                Edge.EndpointCorners = Index2i( IndexConstants::InvalidID, IndexConstants::InvalidID );
                Edges.push_back( Edge );
                EdgeIndex = static_cast<int32_t>( Edges.size() ) - 1;
                GroupEdgeMinEidToGroupEdgeID.insert_or_assign( MinEid, EdgeIndex );
            }
            Boundary.GroupEdges.push_back( EdgeIndex );
            continue;
        }

        const int NumSpans = static_cast<int32_t>( CornerIndices.size() );
        CornerIndices.push_back( CornerIndices[0] );
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
            if ( const int32_t* Existing = FindValue( GroupEdgeMinEidToGroupEdgeID, MinEid ) )
            {
                Boundary.GroupEdges.push_back( *Existing );
                continue;
            }
            GroupEdge Edge;
            Edge.Groups = MakeEdgeGroupsPair( Loop.Edges[StartIndex] );
            std::vector<int> SpanVertices;
            for ( int32_t i = 0; i < NumSpanEdges + 1; ++i )
            {
                SpanVertices.push_back( Loop.Vertices[( StartIndex + i ) % NumV] );
            }
            Edge.Span.InitializeFromVertices( *Mesh, SpanVertices );
            Edge.EndpointCorners = Index2i( GetCornerIDFromVertexID( SpanVertices[0] ),
                                            GetCornerIDFromVertexID( SpanVertices.back() ) );
            UE_CHECK( Edge.EndpointCorners.A != IndexConstants::InvalidID &&
                      Edge.EndpointCorners.B != IndexConstants::InvalidID );
            Edges.push_back( Edge );
            const int EdgeIndex = static_cast<int32_t>( Edges.size() ) - 1;
            Boundary.GroupEdges.push_back( EdgeIndex );
            GroupEdgeMinEidToGroupEdgeID.insert_or_assign( MinEid, EdgeIndex );
        }
    }
    return true;
}

Index2i GroupTopology::MakeEdgeGroupsPair( int MeshEdgeID ) const
{
    const Index2i  EdgeTris = Mesh->GetEdgeT( MeshEdgeID );
    const int      G0       = GetGroupID( EdgeTris.A );
    if ( EdgeTris.B == IndexConstants::InvalidID )
    {
        return Index2i( G0, IndexConstants::InvalidID );
    }
    const int G1 = GetGroupID( EdgeTris.B );
    return ( G0 < G1 ) ? Index2i( G0, G1 ) : Index2i( G1, G0 );
}

int GroupTopology::GetCornerVertexID( int CornerID ) const
{
    UE_CHECK( CornerID >= 0 && CornerID < static_cast<int32_t>( Corners.size() ) );
    return Corners[CornerID].VertexID;
}

int32_t GroupTopology::GetCornerIDFromVertexID( int32_t VertexID ) const
{
    const int32_t* Found = FindValue( VertexIDToCornerIDMap, VertexID );
    return ( Found == nullptr ) ? IndexConstants::InvalidID : *Found;
}

const GroupTopology::Group* GroupTopology::FindGroupByID( int GroupID ) const
{
    if ( GroupID < 0 || GroupID >= static_cast<int32_t>( GroupIDToGroupIndexMap.size() ) ||
         GroupIDToGroupIndexMap[GroupID] == -1 )
    {
        return nullptr;
    }
    return &Groups[GroupIDToGroupIndexMap[GroupID]];
}

const std::vector<int>& GroupTopology::GetGroupTriangles( int GroupID ) const
{
    const Group* Found = FindGroupByID( GroupID );
    return ( Found != nullptr ) ? Found->Triangles : EmptyArray;
}

const std::vector<int>& GroupTopology::GetGroupNbrGroups( int GroupID ) const
{
    const Group* Found = FindGroupByID( GroupID );
    return ( Found != nullptr ) ? Found->NeighbourGroupIDs : EmptyArray;
}

int GroupTopology::FindGroupEdgeID( int MeshEdgeID ) const
{
    const Group* Group = FindGroupByID( GetGroupID( Mesh->GetEdgeT( MeshEdgeID ).A ) );
    if ( Group != nullptr )
    {
        for ( const GroupBoundary& Boundary : Group->Boundaries )
        {
            for ( int EdgeID : Boundary.GroupEdges )
            {
                if ( ( std::find( Edges[EdgeID].Span.Edges.begin(), Edges[EdgeID].Span.Edges.end(), MeshEdgeID ) !=
                       Edges[EdgeID].Span.Edges.end() ) )
                {
                    return EdgeID;
                }
            }
        }
    }
    return -1;
}

const std::vector<int>& GroupTopology::GetGroupEdgeVertices( int GroupEdgeID ) const
{
    UE_CHECK( GroupEdgeID >= 0 && GroupEdgeID < static_cast<int32_t>( Edges.size() ) );
    return Edges[GroupEdgeID].Span.Vertices;
}

const std::vector<int>& GroupTopology::GetGroupEdgeEdges( int GroupEdgeID ) const
{
    UE_CHECK( GroupEdgeID >= 0 && GroupEdgeID < static_cast<int32_t>( Edges.size() ) );
    return Edges[GroupEdgeID].Span.Edges;
}

void GroupTopology::FindEdgeNbrGroups( int GroupEdgeID, std::vector<int>& GroupsOut ) const
{
    const std::vector<int>& Vertices = GetGroupEdgeVertices( GroupEdgeID );
    FindVertexNbrGroups( Vertices[0], GroupsOut );
    FindVertexNbrGroups( Vertices[static_cast<int32_t>( Vertices.size() ) - 1], GroupsOut );
}

void GroupTopology::FindEdgeNbrEdges( int GroupEdgeID, std::vector<int>& EdgesOut ) const
{
    UE_CHECK( GroupEdgeID >= 0 && GroupEdgeID < static_cast<int32_t>( Edges.size() ) );
    const GroupEdge& Edge = Edges[GroupEdgeID];
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
double GroupTopology::GetEdgeArcLength( int32_t GroupEdgeID, std::vector<double>* PerVertexLengthsOut ) const
{
    UE_CHECK( GroupEdgeID >= 0 && GroupEdgeID < static_cast<int32_t>( Edges.size() ) );
    const std::vector<int>& Vertices = GetGroupEdgeVertices( GroupEdgeID );
    const int32_t           NumV     = static_cast<int32_t>( Vertices.size() );
    if ( PerVertexLengthsOut != nullptr )
    {
        PerVertexLengthsOut->resize( NumV );
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

bool GroupTopology::IsBoundaryEdge( int32_t GroupEdgeID ) const
{
    return Mesh->IsBoundaryEdge( Edges[GroupEdgeID].Span.Edges[0] );
}

bool GroupTopology::IsSimpleGroupEdge( int32_t GroupEdgeID ) const
{
    return static_cast<int32_t>( Edges[GroupEdgeID].Span.Edges.size() ) == 1;
}

bool GroupTopology::IsIsolatedLoop( int32_t GroupEdgeID ) const
{
    return Edges[GroupEdgeID].EndpointCorners.A == IndexConstants::InvalidID;
}

void GroupTopology::FindCornerNbrGroups( int CornerID, std::vector<int>& GroupsOut ) const
{
    UE_CHECK( CornerID >= 0 && CornerID < static_cast<int32_t>( Corners.size() ) );
    for ( int GroupID : Corners[CornerID].NeighbourGroupIDs )
    {
        if ( std::find( GroupsOut.begin(), GroupsOut.end(), GroupID ) == GroupsOut.end() )
        {
            GroupsOut.push_back( GroupID );
        }
    }
}

void GroupTopology::ForCornerNbrEdges( int                                          CornerID,
                                       const std::function<bool( int32_t EdgeID )>& ReturnTrueToContinue ) const
{
    UE_CHECK( CornerID >= 0 && CornerID < static_cast<int32_t>( Corners.size() ) );
    std::unordered_set<int32_t> ProcessedEdges;
    for ( int GroupID : Corners[CornerID].NeighbourGroupIDs )
    {
        for ( const GroupBoundary& Boundary : FindGroupByID( GroupID )->Boundaries )
        {
            for ( int32_t const EdgeID : Boundary.GroupEdges )
            {
                const GroupEdge& Edge = Edges[EdgeID];
                if ( ( Edge.EndpointCorners.A != CornerID && Edge.EndpointCorners.B != CornerID ) ||
                     ProcessedEdges.contains( EdgeID ) )
                {
                    continue;
                }
                ProcessedEdges.insert( EdgeID );
                if ( !ReturnTrueToContinue( EdgeID ) )
                {
                    return;
                }
            }
        }
    }
}

void GroupTopology::FindCornerNbrEdges( int CornerID, std::vector<int>& EdgesOut ) const
{
    ForCornerNbrEdges( CornerID,
                       [&EdgesOut]( int32_t EdgeID )
                       {
                           EdgesOut.push_back( EdgeID );
                           return true;
                       } );
}

void GroupTopology::FindCornerNbrCorners( int CornerID, std::vector<int>& CornersOut ) const
{
    ForCornerNbrEdges( CornerID,
                       [&]( int32_t EdgeID )
                       {
                           if ( std::find( CornersOut.begin(), CornersOut.end(),
                                           Edges[EdgeID].EndpointCorners.OtherElement( CornerID ) ) ==
                                CornersOut.end() )
                           {
                               CornersOut.push_back( Edges[EdgeID].EndpointCorners.OtherElement( CornerID ) );
                           }
                           return true;
                       } );
}

void GroupTopology::FindVertexNbrGroups( int VertexID, std::vector<int>& GroupsOut ) const
{
    for ( int Tid : Mesh->VtxTrianglesItr( VertexID ) )
    {
        if ( std::find( GroupsOut.begin(), GroupsOut.end(), GetGroupID( Tid ) ) == GroupsOut.end() )
        {
            GroupsOut.push_back( GetGroupID( Tid ) );
        }
    }
}

void GroupTopology::CollectGroupVertices( int GroupID, std::unordered_set<int>& Vertices ) const
{
    for ( int TriID : GetGroupTriangles( GroupID ) )
    {
        const Index3i TriVerts = Mesh->GetTriangle( TriID );
        Vertices.insert( TriVerts.A );
        Vertices.insert( TriVerts.B );
        Vertices.insert( TriVerts.C );
    }
}

void GroupTopology::CollectGroupBoundaryVertices( int GroupID, std::unordered_set<int>& Vertices ) const
{
    const Group* Group = FindGroupByID( GroupID );
    if ( Group == nullptr )
    {
        return;
    }
    for ( const GroupBoundary& Boundary : Group->Boundaries )
    {
        for ( int EdgeIndex : Boundary.GroupEdges )
        {
            for ( int Vid : Edges[EdgeIndex].Span.Vertices )
            {
                Vertices.insert( Vid );
            }
        }
    }
}

void GroupTopology::GetSelectedTriangles( const GroupTopologySelection& Selection,
                                          std::vector<int32_t>&         Triangles ) const
{
    for ( int32_t const GroupID : Selection.SelectedGroupIDs )
    {
        for ( int32_t const TriangleID : GetGroupTriangles( GroupID ) )
        {
            Triangles.push_back( TriangleID );
        }
    }
}

void GroupTopology::GetAllVertexGroups( int32_t VertexID, std::vector<int32_t>& GroupsOut ) const
{
    for ( int32_t const EdgeID : Mesh->VtxEdgesItr( VertexID ) )
    {
        const Index2i EdgeTris = Mesh->GetEdgeT( EdgeID );
        if ( std::find( GroupsOut.begin(), GroupsOut.end(), GetGroupID( EdgeTris.A ) ) == GroupsOut.end() )
        {
            GroupsOut.push_back( GetGroupID( EdgeTris.A ) );
        }
        if ( EdgeTris.B != DynamicMesh3::InvalidID )
        {
            if ( std::find( GroupsOut.begin(), GroupsOut.end(), GetGroupID( EdgeTris.B ) ) == GroupsOut.end() )
            {
                GroupsOut.push_back( GetGroupID( EdgeTris.B ) );
            }
        }
    }
}

TriangleGroupTopology::TriangleGroupTopology( const DynamicMesh3* MeshIn, bool bAutoBuild )
     : GroupTopology( MeshIn, false )
{
    if ( bAutoBuild )
    {
        RebuildTopology();
    }
}

bool TriangleGroupTopology::RebuildTopology()
{
    Groups.clear();
    Edges.clear();
    Corners.clear();
    FailureReason.clear();
    GroupIDToGroupIndexMap.clear();
    GroupIDToGroupIndexMap.assign( Mesh->MaxTriangleID(), -1 );
    for ( int Tid : Mesh->TriangleIndicesItr() )
    {
        Group NewGroup;
        NewGroup.GroupID = Tid;
        NewGroup.Triangles.push_back( Tid );
        Groups.push_back( NewGroup );
        GroupIDToGroupIndexMap[Tid] = static_cast<int32_t>( Groups.size() ) - 1;
    }
    VertexIDToCornerIDMap = std::unordered_map<int32_t, int32_t>();
    for ( int Vid : Mesh->VertexIndicesItr() )
    {
        Corner Corner;
        Corner.VertexID = Vid;
        Corners.push_back( Corner );
        VertexIDToCornerIDMap.insert_or_assign( Vid, static_cast<int32_t>( Corners.size() ) - 1 );
    }
    for ( Corner& Corner : Corners )
    {
        GetAllVertexGroups( Corner.VertexID, Corner.NeighbourGroupIDs );
    }
    std::vector<int32_t> MeshEdgeToGroupEdge;
    MeshEdgeToGroupEdge.assign( Mesh->MaxEdgeID(), INDEX_NONE );
    for ( Group& Group : Groups )
    {
        Group.Boundaries.resize( 1 );
        GroupBoundary& Boundary0 = Group.Boundaries[0];
        const Index3i  TriEdges  = Mesh->GetTriEdges( Group.GroupID );
        for ( int j = 0; j < 3; ++j )
        {
            int& GroupEdgeIndex = MeshEdgeToGroupEdge[TriEdges[j]];
            if ( GroupEdgeIndex == INDEX_NONE )
            {
                GroupEdge NewGroupEdge;
                NewGroupEdge.Groups      = MakeEdgeGroupsPair( TriEdges[j] );
                const Index2i    EdgeVerts = Mesh->GetEdgeV( TriEdges[j] );
                std::vector<int> SpanVertices;
                SpanVertices.push_back( EdgeVerts.A );
                SpanVertices.push_back( EdgeVerts.B );
                NewGroupEdge.Span.InitializeFromVertices( *Mesh, SpanVertices );
                NewGroupEdge.EndpointCorners =
                     Index2i( GetCornerIDFromVertexID( EdgeVerts.A ), GetCornerIDFromVertexID( EdgeVerts.B ) );
                Edges.push_back( NewGroupEdge );
                GroupEdgeIndex = static_cast<int32_t>( Edges.size() ) - 1;
            }
            Boundary0.GroupEdges.push_back( GroupEdgeIndex );
        }
        const Index3i TriNbrTris = Mesh->GetTriNeighbourTris( Group.GroupID );
        for ( int j = 0; j < 3; ++j )
        {
            if ( TriNbrTris[j] != DynamicMesh3::InvalidID )
            {
                Group.NeighbourGroupIDs.push_back( TriNbrTris[j] );
                Boundary0.NeighbourGroupIDs.push_back( TriNbrTris[j] );
            }
            else
            {
                Boundary0.bIsOnBoundary = true;
            }
        }
    }
    return true;
}
