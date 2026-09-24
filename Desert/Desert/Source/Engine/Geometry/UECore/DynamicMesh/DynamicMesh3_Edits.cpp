// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/DynamicMesh3_Edits.cpp:1-878, adapted:
// UE Core via UECore.hpp; AttributeSet hooks removed (task P4 restores them); the topology edit operators
// (879-2311) are task P3.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

using namespace Desert::Geometry;

int FDynamicMesh3::AppendVertex( const FVertexInfo& VtxInfo )
{
    int vid = VertexRefCounts.Allocate();
    Vertices.InsertAt( VtxInfo.Position, vid );

    if ( HasVertexNormals() )
    {
        FVector3f n = ( VtxInfo.bHaveN ) ? VtxInfo.Normal : FVector3f::UnitY();
        VertexNormals->InsertAt( n, vid );
    }

    if ( HasVertexColors() )
    {
        FVector3f c = ( VtxInfo.bHaveC ) ? VtxInfo.Color : FVector3f::One();
        VertexColors->InsertAt( c, vid );
    }

    if ( HasVertexUVs() )
    {
        FVector2f u = ( VtxInfo.bHaveUV ) ? VtxInfo.UV : FVector2f::Zero();
        VertexUVs->InsertAt( u, vid );
    }

    AllocateEdgesList( vid );

    UpdateChangeStamps( true, true );
    return vid;
}

int FDynamicMesh3::AppendVertex( const FDynamicMesh3& from, int fromVID )
{
    const int vid = VertexRefCounts.Allocate();
    Vertices.InsertAt( from.Vertices[fromVID], vid );

    if ( HasVertexNormals() )
    {
        if ( from.HasVertexNormals() )
        {
            const TDynamicVector<FVector3f>& FromNormals = from.VertexNormals.GetValue();
            VertexNormals->InsertAt( FromNormals[fromVID], vid );
        }
        else
        {
            VertexNormals->InsertAt( { 0, 1, 0 }, vid ); // y-up
        }
    }

    if ( HasVertexColors() )
    {
        if ( from.HasVertexColors() )
        {
            const TDynamicVector<FVector3f>& FromColors = from.VertexColors.GetValue();
            VertexColors->InsertAt( FromColors[fromVID], vid );
        }
        else
        {
            VertexColors->InsertAt( { 1, 1, 1 }, vid ); // white
        }
    }

    if ( HasVertexUVs() )
    {
        if ( from.HasVertexUVs() )
        {
            const TDynamicVector<FVector2f>& FromUVs = from.VertexUVs.GetValue();
            VertexUVs->InsertAt( FromUVs[fromVID], vid );
        }
        else
        {
            VertexUVs->InsertAt( { 0, 0 }, vid );
        }
    }

    AllocateEdgesList( vid );

    UpdateChangeStamps( true, true );
    return vid;
}

EMeshResult FDynamicMesh3::InsertVertex( int vid, const FVertexInfo& info, bool bUnsafe )
{
    if ( VertexRefCounts.IsValid( vid ) )
    {
        return EMeshResult::Failed_VertexAlreadyExists;
    }

    bool bOK = ( bUnsafe ) ? VertexRefCounts.AllocateAtUnsafe( vid ) : VertexRefCounts.AllocateAt( vid );
    if ( bOK == false )
    {
        return EMeshResult::Failed_CannotAllocateVertex;
    }

    Vertices.InsertAt( info.Position, vid );

    if ( HasVertexNormals() )
    {
        FVector3f n = ( info.bHaveN ) ? info.Normal : FVector3f::UnitY();
        VertexNormals->InsertAt( n, vid );
    }

    if ( HasVertexColors() )
    {
        FVector3f c = ( info.bHaveC ) ? info.Color : FVector3f::One();
        VertexColors->InsertAt( c, vid );
    }

    if ( HasVertexUVs() )
    {
        FVector2f u = ( info.bHaveUV ) ? info.UV : FVector2f::Zero();
        VertexUVs->InsertAt( u, vid );
    }

    AllocateEdgesList( vid );

    UpdateChangeStamps( true, true );
    return EMeshResult::Ok;
}

int FDynamicMesh3::AppendTriangle( const FIndex3i& tv, int gid )
{
    if ( IsVertex( tv[0] ) == false || IsVertex( tv[1] ) == false || IsVertex( tv[2] ) == false )
    {
        UE_CHECK_SLOW( false );
        return InvalidID;
    }
    if ( tv[0] == tv[1] || tv[0] == tv[2] || tv[1] == tv[2] )
    {
        UE_CHECK_SLOW( false );
        return InvalidID;
    }

    // look up edges. if any already have two triangles, this would
    // create non-manifold geometry and so we do not allow it
    bool boundary0, boundary1, boundary2;
    int  e0 = FindEdgeInternal( tv[0], tv[1], boundary0 );
    int  e1 = FindEdgeInternal( tv[1], tv[2], boundary1 );
    int  e2 = FindEdgeInternal( tv[2], tv[0], boundary2 );
    if ( ( e0 != InvalidID && boundary0 == false ) || ( e1 != InvalidID && boundary1 == false ) ||
         ( e2 != InvalidID && boundary2 == false ) )
    {
        return NonManifoldID;
    }

    // if all edges were already present, check for duplicate triangle
    if ( e0 != InvalidID && e1 != InvalidID && e2 != InvalidID )
    {
        // check if the triangle attached to edge e0 (tv[0] - tv[1]) also contains tv[2]
        int ti = Edges[e0].Tri[0];
        if ( Triangles[ti][0] == tv[2] || Triangles[ti][1] == tv[2] || Triangles[ti][2] == tv[2] )
        {
            return DuplicateTriangleID;
        }
        // there's no other triangle on the edge to check -- if there were, we would have already returned
        // NonManifoldID
        UE_CHECK_SLOW( Edges[e0].Tri[1] == InvalidID );
    }

    bool bHasGroups = HasTriangleGroups(); // have to check before changing .triangles

    // now safe to insert triangle
    int tid = TriangleRefCounts.Allocate();
    Triangles.InsertAt( tv, tid );
    if ( bHasGroups )
    {
        TriangleGroups->InsertAt( gid, tid );
        GroupIDCounter = FMath::Max( GroupIDCounter, gid + 1 );
    }

    // increment ref counts and update/create edges
    VertexRefCounts.Increment( tv[0] );
    VertexRefCounts.Increment( tv[1] );
    VertexRefCounts.Increment( tv[2] );

    AddTriangleEdge( tid, tv[0], tv[1], 0, e0 );
    AddTriangleEdge( tid, tv[1], tv[2], 1, e1 );
    AddTriangleEdge( tid, tv[2], tv[0], 2, e2 );

    UpdateChangeStamps( true, true );
    return tid;
}

EMeshResult FDynamicMesh3::InsertTriangle( int tid, const FIndex3i& tv, int gid, bool bUnsafe )
{
    if ( TriangleRefCounts.IsValid( tid ) )
    {
        return EMeshResult::Failed_TriangleAlreadyExists;
    }

    if ( IsVertex( tv[0] ) == false || IsVertex( tv[1] ) == false || IsVertex( tv[2] ) == false )
    {
        UE_CHECK_SLOW( false );
        return EMeshResult::Failed_NotAVertex;
    }
    if ( tv[0] == tv[1] || tv[0] == tv[2] || tv[1] == tv[2] )
    {
        UE_CHECK_SLOW( false );
        return EMeshResult::Failed_InvalidNeighbourhood;
    }

    // look up edges. if any already have two triangles, this would
    // create non-manifold geometry and so we do not allow it
    int e0 = FindEdge( tv[0], tv[1] );
    int e1 = FindEdge( tv[1], tv[2] );
    int e2 = FindEdge( tv[2], tv[0] );
    if ( ( e0 != InvalidID && IsBoundaryEdge( e0 ) == false ) ||
         ( e1 != InvalidID && IsBoundaryEdge( e1 ) == false ) ||
         ( e2 != InvalidID && IsBoundaryEdge( e2 ) == false ) )
    {
        return EMeshResult::Failed_WouldCreateNonmanifoldEdge;
    }

    bool bOK = ( bUnsafe ) ? TriangleRefCounts.AllocateAtUnsafe( tid ) : TriangleRefCounts.AllocateAt( tid );
    if ( bOK == false )
    {
        return EMeshResult::Failed_CannotAllocateTriangle;
    }

    // now safe to insert triangle
    Triangles.InsertAt( tv, tid );
    if ( HasTriangleGroups() )
    {
        TriangleGroups->InsertAt( gid, tid );
        GroupIDCounter = FMath::Max( GroupIDCounter, gid + 1 );
    }

    // increment ref counts and update/create edges
    VertexRefCounts.Increment( tv[0] );
    VertexRefCounts.Increment( tv[1] );
    VertexRefCounts.Increment( tv[2] );

    AddTriangleEdge( tid, tv[0], tv[1], 0, e0 );
    AddTriangleEdge( tid, tv[1], tv[2], 1, e1 );
    AddTriangleEdge( tid, tv[2], tv[0], 2, e2 );

    UpdateChangeStamps( true, true );
    return EMeshResult::Ok;
}

int32 FDynamicMesh3::RemoveUnusedVertices()
{
    int32 NumRemoved = 0;
    for ( int32 VID = 0; VID < MaxVertexID(); ++VID )
    {
        // If vertex exists but is not referenced by any triangles
        if ( VertexRefCounts.GetRefCount( VID ) == 1 )
        {
            NumRemoved++;
            VertexRefCounts.Decrement( VID );

            UE_CHECK_SLOW( VertexRefCounts.IsValid( VID ) == false ); // vertex should now not be valid
            UE_CHECK_SLOW( VertexEdgeLists.GetCount( VID ) == 0 ); // vertex should not have had any edges attached
        }
    }

    if ( NumRemoved > 0 )
    {
        UpdateChangeStamps( true, true );
    }
    return NumRemoved;
}

bool FDynamicMesh3::HasUnusedVertices() const
{
    for ( int32 VID = 0; VID < MaxVertexID(); ++VID )
    {
        // If vertex exists but is not referenced by any triangles
        if ( VertexRefCounts.GetRefCount( VID ) == 1 )
        {
            return true;
        }
    }

    return false;
}

void FDynamicMesh3::CompactInPlace( FCompactMaps* CompactInfo )
{
    // Initialize CompactInfo

    if ( CompactInfo )
    {
        // starts as identity (except at gaps); sparsely remapped below
        CompactInfo->Reset( MaxVertexID(), MaxTriangleID(), false );
        for ( int VID = 0, NumVID = MaxVertexID(); VID < NumVID; VID++ )
        {
            CompactInfo->SetVertexMapping( VID, IsVertex( VID ) ? VID : -1 );
        }
        for ( int TID = 0, NumTID = MaxTriangleID(); TID < NumTID; TID++ )
        {
            CompactInfo->SetTriangleMapping( TID, IsTriangle( TID ) ? TID : -1 );
        }
    }

    // find first free vertex, and last used vertex
    int iLastV = MaxVertexID() - 1, iCurV = 0;
    while ( iLastV >= 0 && VertexRefCounts.IsValidUnsafe( iLastV ) == false )
    {
        iLastV--;
    }
    while ( iCurV < iLastV && VertexRefCounts.IsValidUnsafe( iCurV ) )
    {
        iCurV++;
    }

    TDynamicVector<unsigned short>& vref = VertexRefCounts.GetRawRefCountsUnsafe();

    while ( iCurV < iLastV )
    {
        Vertices[iCurV] = Vertices[iLastV];

        // const int kc = iCurV * 3;
        // const int kl = iLastV * 3;
        if ( HasVertexNormals() )
        {
            TDynamicVector<FVector3f>& Normals = VertexNormals.GetValue();
            Normals[iCurV]                     = Normals[iLastV];
        }
        if ( HasVertexColors() )
        {
            TDynamicVector<FVector3f>& Colors = VertexColors.GetValue();
            Colors[iCurV]                     = Colors[iLastV];
        }
        if ( HasVertexUVs() )
        {
            TDynamicVector<FVector2f>& UVs = VertexUVs.GetValue();
            UVs[iCurV]                     = UVs[iLastV];
        }

        for ( int eid : VertexEdgeLists.Values( iLastV ) )
        {
            // replace vertex in edges
            ReplaceEdgeVertex( eid, iLastV, iCurV );

            // replace vertex in triangles
            const FIndex2i Tris = Edges[eid].Tri;
            ReplaceTriangleVertex( Tris[0], iLastV, iCurV );
            if ( Tris[1] != InvalidID )
            {
                ReplaceTriangleVertex( Tris[1], iLastV, iCurV );
            }
        }

        // shift vertex refcount to position
        vref[iCurV]  = vref[iLastV];
        vref[iLastV] = FRefCountVector::INVALID_REF_COUNT;

        // move edge list
        VertexEdgeLists.Move( iLastV, iCurV );

        if ( CompactInfo != nullptr )
        {
            CompactInfo->SetVertexMapping( iLastV, iCurV );
        }

        // move cur forward one, last back one, and  then search for next valid
        iLastV--;
        iCurV++;
        while ( iLastV >= 0 && VertexRefCounts.IsValidUnsafe( iLastV ) == false )
        {
            iLastV--;
        }
        while ( iCurV < iLastV && VertexRefCounts.IsValidUnsafe( iCurV ) )
        {
            iCurV++;
        }
    }

    // trim vertices data structures
    VertexRefCounts.Trim( VertexCount() );
    Vertices.Resize( VertexCount() );
    if ( HasVertexNormals() )
    {
        VertexNormals->Resize( VertexCount() * 3 );
    }
    if ( HasVertexColors() )
    {
        VertexColors->Resize( VertexCount() * 3 );
    }
    if ( HasVertexUVs() )
    {
        VertexUVs->Resize( VertexCount() * 2 );
    }

    VertexEdgeLists.Compact( VertexCount() );

    /** shift triangles **/

    // find first free triangle, and last valid triangle
    int iLastT = MaxTriangleID() - 1, iCurT = 0;
    while ( iLastT >= 0 && TriangleRefCounts.IsValidUnsafe( iLastT ) == false )
    {
        iLastT--;
    }
    while ( iCurT < iLastT && TriangleRefCounts.IsValidUnsafe( iCurT ) )
    {
        iCurT++;
    }

    TDynamicVector<unsigned short>& tref = TriangleRefCounts.GetRawRefCountsUnsafe();

    while ( iCurT < iLastT )
    {
        // shift triangle
        Triangles[iCurT]     = Triangles[iLastT];
        TriangleEdges[iCurT] = TriangleEdges[iLastT];

        if ( HasTriangleGroups() )
        {
            TriangleGroups.GetValue()[iCurT] = TriangleGroups.GetValue()[iLastT];
        }

        // update edges
        for ( int j = 0; j < 3; ++j )
        {
            int eid = TriangleEdges[iCurT][j];
            ReplaceEdgeTriangle( eid, iLastT, iCurT );
        }

        // shift triangle refcount to position
        tref[iCurT]  = tref[iLastT];
        tref[iLastT] = FRefCountVector::INVALID_REF_COUNT;

        if ( CompactInfo != nullptr )
        {
            CompactInfo->SetTriangleMapping( iLastT, iCurT );
        }

        // move cur forward one, last back one, and  then search for next valid
        iLastT--;
        iCurT++;
        while ( iLastT >= 0 && TriangleRefCounts.IsValidUnsafe( iLastT ) == false )
        {
            iLastT--;
        }
        while ( iCurT < iLastT && TriangleRefCounts.IsValidUnsafe( iCurT ) )
        {
            iCurT++;
        }
    }

    // trim triangles data structures
    TriangleRefCounts.Trim( TriangleCount() );
    Triangles.Resize( TriangleCount() );
    TriangleEdges.Resize( TriangleCount() );
    if ( HasTriangleGroups() )
    {
        TriangleGroups->Resize( TriangleCount() );
    }

    /** shift edges **/

    // find first free edge, and last used edge
    int iLastE = MaxEdgeID() - 1, iCurE = 0;
    while ( iLastE >= 0 && EdgeRefCounts.IsValidUnsafe( iLastE ) == false )
    {
        iLastE--;
    }
    while ( iCurE < iLastE && EdgeRefCounts.IsValidUnsafe( iCurE ) )
    {
        iCurE++;
    }

    TDynamicVector<unsigned short>& eref = EdgeRefCounts.GetRawRefCountsUnsafe();

    while ( iCurE < iLastE )
    {
        Edges[iCurE] = Edges[iLastE];

        // replace edge in vertex edges lists
        int v0 = Edges[iCurE].Vert[0], v1 = Edges[iCurE].Vert[1];
        VertexEdgeLists.Replace( v0, [iLastE]( int eid ) { return eid == iLastE; }, iCurE );
        VertexEdgeLists.Replace( v1, [iLastE]( int eid ) { return eid == iLastE; }, iCurE );

        // replace edge in triangles
        ReplaceTriangleEdge( Edges[iCurE].Tri[0], iLastE, iCurE );
        if ( Edges[iCurE].Tri[1] != InvalidID )
        {
            ReplaceTriangleEdge( Edges[iCurE].Tri[1], iLastE, iCurE );
        }

        // shift triangle refcount to position
        eref[iCurE]  = eref[iLastE];
        eref[iLastE] = FRefCountVector::INVALID_REF_COUNT;

        // move cur forward one, last back one, and  then search for next valid
        iLastE--;
        iCurE++;
        while ( iLastE >= 0 && EdgeRefCounts.IsValidUnsafe( iLastE ) == false )
        {
            iLastE--;
        }
        while ( iCurE < iLastE && EdgeRefCounts.IsValidUnsafe( iCurE ) )
        {
            iCurE++;
        }
    }

    // trim edge data structures
    EdgeRefCounts.Trim( EdgeCount() );
    Edges.Resize( EdgeCount() );
}

EMeshResult FDynamicMesh3::ReverseTriOrientation( int tID )
{
    if ( !IsTriangle( tID ) )
    {
        return EMeshResult::Failed_NotATriangle;
    }
    ReverseTriOrientationInternal( tID );
    UpdateChangeStamps( true, true );
    return EMeshResult::Ok;
}

void FDynamicMesh3::ReverseTriOrientationInternal( int tID )
{
    FIndex3i t = GetTriangle( tID );
    SetTriangleInternal( tID, t[1], t[0], t[2] );
    FIndex3i te = GetTriEdges( tID );
    SetTriangleEdgesInternal( tID, te[0], te[2], te[1] );
}

void FDynamicMesh3::ReverseOrientation( bool bFlipNormals )
{
    for ( int tid : TriangleIndicesItr() )
    {
        ReverseTriOrientationInternal( tid );
    }
    if ( bFlipNormals && HasVertexNormals() )
    {
        for ( int vid : VertexIndicesItr() )
        {
            TDynamicVector<FVector3f>& Normals = VertexNormals.GetValue();
            Normals[vid]                       = -Normals[vid];
        }
    }
    UpdateChangeStamps( true, true );
}

EMeshResult FDynamicMesh3::RemoveVertex( int vID, bool bPreserveManifold )
{
    if ( VertexRefCounts.IsValid( vID ) == false )
    {
        return EMeshResult::Failed_NotAVertex;
    }

    // if any one-ring vtx is a boundary vtx and one of its outer-ring edges is an
    // interior edge then we will create a bowtie if we remove that triangle
    if ( bPreserveManifold )
    {
        for ( int tid : VtxTrianglesItr( vID ) )
        {
            FIndex3i tri = GetTriangle( tid );
            int      j   = IndexUtil::FindTriIndex( vID, tri );
            int      oa = tri[( j + 1 ) % 3], ob = tri[( j + 2 ) % 3];
            int      eid = FindEdge( oa, ob );
            if ( IsBoundaryEdge( eid ) )
            {
                continue;
            }
            if ( IsBoundaryVertex( oa ) || IsBoundaryVertex( ob ) )
            {
                return EMeshResult::Failed_WouldCreateBowtie;
            }
        }
    }

    // Remove incident triangles
    FDynamicMesh3::FLocalIntArray tris;
    GetVtxTriangles( vID, tris );
    for ( int tID : tris )
    {
        EMeshResult result = RemoveTriangle( tID, false, bPreserveManifold );
        if ( result != EMeshResult::Ok )
        {
            return result;
        }
    }

    if ( VertexRefCounts.GetRefCount( vID ) != 1 )
    {
        return EMeshResult::Failed_VertexStillReferenced;
    }

    VertexRefCounts.Decrement( vID );
    UE_ENSURE( VertexRefCounts.IsValid( vID ) == false );
    VertexEdgeLists.Clear( vID );

    UpdateChangeStamps( true, true );
    return EMeshResult::Ok;
}

EMeshResult FDynamicMesh3::RemoveTriangle( int tID, bool bRemoveIsolatedVertices, bool bPreserveManifold )
{
    if ( !TriangleRefCounts.IsValid( tID ) )
    {
        UE_ENSURE( false );
        return EMeshResult::Failed_NotATriangle;
    }

    FIndex3i tv = GetTriangle( tID );
    FIndex3i te = GetTriEdges( tID );

    // if any tri vtx is a boundary vtx connected to two interior edges, then
    // we cannot remove this triangle because it would create a bowtie vertex!
    // (that vtx already has 2 boundary edges, and we would add two more)
    if ( bPreserveManifold )
    {
        for ( int j = 0; j < 3; ++j )
        {
            if ( IsBoundaryVertex( tv[j] ) )
            {
                if ( IsBoundaryEdge( te[j] ) == false && IsBoundaryEdge( te[( j + 2 ) % 3] ) == false )
                {
                    return EMeshResult::Failed_WouldCreateBowtie;
                }
            }
        }
    }

    // Remove triangle from its edges. if edge has no triangles left,
    // then it is removed.
    for ( int j = 0; j < 3; ++j )
    {
        int eid = te[j];
        ReplaceEdgeTriangle( eid, tID, InvalidID );
        const FEdge Edge = Edges[eid];
        if ( Edge.Tri[0] == InvalidID )
        {
            int a = Edge.Vert[0];
            VertexEdgeLists.Remove( a, eid );

            int b = Edge.Vert[1];
            VertexEdgeLists.Remove( b, eid );

            EdgeRefCounts.Decrement( eid );
        }
    }

    // free this triangle
    TriangleRefCounts.Decrement( tID );
    UE_CHECK_SLOW( TriangleRefCounts.IsValid( tID ) == false );

    // Decrement vertex refcounts. If any hit 1 and we got remove-isolated flag,
    // we need to remove that vertex
    for ( int j = 0; j < 3; ++j )
    {
        int vid = tv[j];
        VertexRefCounts.Decrement( vid );
        if ( bRemoveIsolatedVertices && VertexRefCounts.GetRefCount( vid ) == 1 )
        {
            VertexRefCounts.Decrement( vid );
            UE_CHECK_SLOW( VertexRefCounts.IsValid( vid ) == false );
            VertexEdgeLists.Clear( vid );
        }
    }

    UpdateChangeStamps( true, true );
    return EMeshResult::Ok;
}

EMeshResult FDynamicMesh3::SetTriangle( int tID, const FIndex3i& newv, bool bRemoveIsolatedVertices )
{
    // UE 5.8 opens with `if (ensure(HasAttributes()) == false) return Failed_Unsupported;`, which rejects
    // exactly the meshes it can handle (no attributes). Without an AttributeSet that guard has nothing to
    // test; task P4 restores it with the intended polarity (fail when attributes ARE present).

    FIndex3i tv = GetTriangle( tID );
    FIndex3i te = GetTriEdges( tID );
    if ( tv[0] == newv[0] && tv[1] == newv[1] )
    {
        te[0] = -1;
    }
    if ( tv[1] == newv[1] && tv[2] == newv[2] )
    {
        te[1] = -1;
    }
    if ( tv[2] == newv[2] && tv[0] == newv[0] )
    {
        te[2] = -1;
    }

    if ( !TriangleRefCounts.IsValid( tID ) )
    {
        UE_CHECK_SLOW( false );
        return EMeshResult::Failed_NotATriangle;
    }
    if ( IsVertex( newv[0] ) == false || IsVertex( newv[1] ) == false || IsVertex( newv[2] ) == false )
    {
        UE_CHECK_SLOW( false );
        return EMeshResult::Failed_NotAVertex;
    }
    if ( newv[0] == newv[1] || newv[0] == newv[2] || newv[1] == newv[2] )
    {
        UE_CHECK_SLOW( false );
        return EMeshResult::Failed_BrokenTopology;
    }
    // look up edges. if any already have two triangles, this would
    // create non-manifold geometry and so we do not allow it
    int e0 = FindEdge( newv[0], newv[1] );
    int e1 = FindEdge( newv[1], newv[2] );
    int e2 = FindEdge( newv[2], newv[0] );
    if ( ( te[0] != -1 && e0 != InvalidID && IsBoundaryEdge( e0 ) == false ) ||
         ( te[1] != -1 && e1 != InvalidID && IsBoundaryEdge( e1 ) == false ) ||
         ( te[2] != -1 && e2 != InvalidID && IsBoundaryEdge( e2 ) == false ) )
    {
        return EMeshResult::Failed_BrokenTopology;
    }

    // [TODO] check that we are not going to create invalid stuff...

    // Remove triangle from its edges. if edge has no triangles left, then it is removed.
    for ( int j = 0; j < 3; ++j )
    {
        int eid = te[j];
        if ( eid == -1 ) // we don't need to modify this edge
        {
            continue;
        }
        ReplaceEdgeTriangle( eid, tID, InvalidID );
        const FEdge Edge = GetEdge( eid );
        if ( Edge.Tri[0] == InvalidID )
        {
            int a = Edge.Vert[0];
            VertexEdgeLists.Remove( a, eid );

            int b = Edge.Vert[1];
            VertexEdgeLists.Remove( b, eid );

            EdgeRefCounts.Decrement( eid );
        }
    }

    // Decrement vertex refcounts. If any hit 1 and we got remove-isolated flag,
    // we need to remove that vertex
    for ( int j = 0; j < 3; ++j )
    {
        int vid = tv[j];
        if ( vid == newv[j] ) // we don't need to modify this vertex
        {
            continue;
        }
        VertexRefCounts.Decrement( vid );
        if ( bRemoveIsolatedVertices && VertexRefCounts.GetRefCount( vid ) == 1 )
        {
            VertexRefCounts.Decrement( vid );
            UE_CHECK_SLOW( VertexRefCounts.IsValid( vid ) == false );
            VertexEdgeLists.Clear( vid );
        }
    }

    // ok now re-insert with vertices
    for ( int j = 0; j < 3; ++j )
    {
        if ( newv[j] != tv[j] )
        {
            Triangles[tID][j] = newv[j];
            VertexRefCounts.Increment( newv[j] );
        }
    }

    if ( te[0] != -1 )
    {
        AddTriangleEdge( tID, newv[0], newv[1], 0, e0 );
    }
    if ( te[1] != -1 )
    {
        AddTriangleEdge( tID, newv[1], newv[2], 1, e1 );
    }
    if ( te[2] != -1 )
    {
        AddTriangleEdge( tID, newv[2], newv[0], 2, e2 );
    }

    UpdateChangeStamps( true, true );
    return EMeshResult::Ok;
}
