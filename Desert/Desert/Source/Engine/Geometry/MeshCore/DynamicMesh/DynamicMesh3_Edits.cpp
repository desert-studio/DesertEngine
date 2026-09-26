// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/DynamicMesh3_Edits.cpp:1-2311,
// adapted: UE Core as std/glm; check/checkSlow/checkfSlow are assert, ensure is DESERT_VERIFY_WARN /
// Common::EnsureOrWarn; SetTriangle's attribute guard has the polarity UE intended (fails when attributes ARE
// present).
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMesh3.hpp"
#include "Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp"
#include <Common/Core/Core.hpp>
#include <algorithm>

using namespace Desert::Geometry;

int DynamicMesh3::AppendVertex( const VertexInfo& VertInfo )
{
    int const vid = m_VertexRefCounts.Allocate();
    m_Vertices.InsertAt( VertInfo.Position, vid );

    if ( m_VertexNormals.has_value() )
    {
        glm::vec3 const n = ( VertInfo.bHaveN ) ? VertInfo.Normal : glm::vec3( 0, 1, 0 );
        m_VertexNormals->InsertAt( n, vid );
    }

    if ( m_VertexColors.has_value() )
    {
        glm::vec3 const c = ( VertInfo.bHaveC ) ? VertInfo.Color : glm::vec3( 1 );
        m_VertexColors->InsertAt( c, vid );
    }

    if ( m_VertexUVs.has_value() )
    {
        glm::vec2 const u = ( VertInfo.bHaveUV ) ? VertInfo.UV : glm::vec2( 0 );
        m_VertexUVs->InsertAt( u, vid );
    }

    AllocateEdgesList( vid );
    if ( HasAttributes() )
    {
        Attributes()->OnNewVertex( vid, false );
    }
    UpdateChangeStamps( true, true );
    return vid;
}

int DynamicMesh3::AppendVertex( const DynamicMesh3& SourceMesh, int SourceVertexID )
{
    const int vid = m_VertexRefCounts.Allocate();
    m_Vertices.InsertAt( SourceMesh.m_Vertices[SourceVertexID], vid );

    if ( m_VertexNormals.has_value() )
    {
        if ( SourceMesh.m_VertexNormals.has_value() )
        {
            const DynamicVector<glm::vec3>& FromNormals = SourceMesh.m_VertexNormals.value();
            m_VertexNormals->InsertAt( FromNormals[SourceVertexID], vid );
        }
        else
        {
            m_VertexNormals->InsertAt( { 0, 1, 0 }, vid ); // y-up
        }
    }

    if ( m_VertexColors.has_value() )
    {
        if ( SourceMesh.m_VertexColors.has_value() )
        {
            const DynamicVector<glm::vec3>& FromColors = SourceMesh.m_VertexColors.value();
            m_VertexColors->InsertAt( FromColors[SourceVertexID], vid );
        }
        else
        {
            m_VertexColors->InsertAt( { 1, 1, 1 }, vid ); // white
        }
    }

    if ( m_VertexUVs.has_value() )
    {
        if ( SourceMesh.m_VertexUVs.has_value() )
        {
            const DynamicVector<glm::vec2>& FromUVs = SourceMesh.m_VertexUVs.value();
            m_VertexUVs->InsertAt( FromUVs[SourceVertexID], vid );
        }
        else
        {
            m_VertexUVs->InsertAt( { 0, 0 }, vid );
        }
    }

    AllocateEdgesList( vid );
    if ( HasAttributes() )
    {
        Attributes()->OnNewVertex( vid, false );
    }
    UpdateChangeStamps( true, true );
    return vid;
}

MeshResult DynamicMesh3::InsertVertex( int VertexID, const VertexInfo& info, bool bUnsafe )
{
    if ( m_VertexRefCounts.IsValid( VertexID ) )
    {
        return MeshResult::Failed_VertexAlreadyExists;
    }

    bool const bOK =
         ( bUnsafe ) ? m_VertexRefCounts.AllocateAtUnsafe( VertexID ) : m_VertexRefCounts.AllocateAt( VertexID );
    if ( !bOK )
    {
        return MeshResult::Failed_CannotAllocateVertex;
    }

    m_Vertices.InsertAt( info.Position, VertexID );

    if ( m_VertexNormals.has_value() )
    {
        glm::vec3 const n = ( info.bHaveN ) ? info.Normal : glm::vec3( 0, 1, 0 );
        m_VertexNormals->InsertAt( n, VertexID );
    }

    if ( m_VertexColors.has_value() )
    {
        glm::vec3 const c = ( info.bHaveC ) ? info.Color : glm::vec3( 1 );
        m_VertexColors->InsertAt( c, VertexID );
    }

    if ( m_VertexUVs.has_value() )
    {
        glm::vec2 const u = ( info.bHaveUV ) ? info.UV : glm::vec2( 0 );
        m_VertexUVs->InsertAt( u, VertexID );
    }

    AllocateEdgesList( VertexID );
    if ( HasAttributes() )
    {
        Attributes()->OnNewVertex( VertexID, true );
    }
    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

int DynamicMesh3::AppendTriangle( const Index3i& TriVertices, int GroupID )
{
    if ( !IsVertex( TriVertices[0] ) || !IsVertex( TriVertices[1] ) || !IsVertex( TriVertices[2] ) )
    {
        assert( false );
        return InvalidID;
    }
    if ( TriVertices[0] == TriVertices[1] || TriVertices[0] == TriVertices[2] || TriVertices[1] == TriVertices[2] )
    {
        assert( false );
        return InvalidID;
    }

    // look up edges. if any already have two triangles, this would
    // create non-manifold geometry and so we do not allow it
    bool      boundary0 = false;
    bool      boundary1 = false;
    bool      boundary2 = false;
    int const e0        = FindEdgeInternal( TriVertices[0], TriVertices[1], boundary0 );
    int const e1        = FindEdgeInternal( TriVertices[1], TriVertices[2], boundary1 );
    int const e2        = FindEdgeInternal( TriVertices[2], TriVertices[0], boundary2 );
    if ( ( e0 != InvalidID && !boundary0 ) || ( e1 != InvalidID && !boundary1 ) ||
         ( e2 != InvalidID && !boundary2 ) )
    {
        return NonManifoldID;
    }

    // if all edges were already present, check for duplicate triangle
    if ( e0 != InvalidID && e1 != InvalidID && e2 != InvalidID )
    {
        // check if the triangle attached to edge e0 (TriVertices[0] - TriVertices[1]) also contains TriVertices[2]
        int const ti = m_Edges[e0].Tri[0];
        if ( m_Triangles[ti][0] == TriVertices[2] || m_Triangles[ti][1] == TriVertices[2] ||
             m_Triangles[ti][2] == TriVertices[2] )
        {
            return DuplicateTriangleID;
        }
        // there's no other triangle on the edge to check -- if there were, we would have already returned
        // NonManifoldID
        assert( m_Edges[e0].Tri[1] == InvalidID );
    }

    bool const bHasGroups = HasTriangleGroups(); // have to check before changing .triangles

    // now safe to insert triangle
    int const tid = m_TriangleRefCounts.Allocate();
    m_Triangles.InsertAt( TriVertices, tid );
    if ( bHasGroups )
    {
        m_TriangleGroups->InsertAt( GroupID, tid );
        m_GroupIDCounter = std::max( m_GroupIDCounter, GroupID + 1 );
    }

    // increment ref counts and update/create edges
    m_VertexRefCounts.Increment( TriVertices[0] );
    m_VertexRefCounts.Increment( TriVertices[1] );
    m_VertexRefCounts.Increment( TriVertices[2] );

    AddTriangleEdge( tid, TriVertices[0], TriVertices[1], 0, e0 );
    AddTriangleEdge( tid, TriVertices[1], TriVertices[2], 1, e1 );
    AddTriangleEdge( tid, TriVertices[2], TriVertices[0], 2, e2 );
    if ( HasAttributes() )
    {
        Attributes()->OnNewTriangle( tid, false );
    }
    UpdateChangeStamps( true, true );
    return tid;
}

MeshResult DynamicMesh3::InsertTriangle( int TriangleID, const Index3i& TriVertices, int GroupID, bool bUnsafe )
{
    if ( m_TriangleRefCounts.IsValid( TriangleID ) )
    {
        return MeshResult::Failed_TriangleAlreadyExists;
    }

    if ( !IsVertex( TriVertices[0] ) || !IsVertex( TriVertices[1] ) || !IsVertex( TriVertices[2] ) )
    {
        assert( false );
        return MeshResult::Failed_NotAVertex;
    }
    if ( TriVertices[0] == TriVertices[1] || TriVertices[0] == TriVertices[2] || TriVertices[1] == TriVertices[2] )
    {
        assert( false );
        return MeshResult::Failed_InvalidNeighbourhood;
    }

    // look up edges. if any already have two triangles, this would
    // create non-manifold geometry and so we do not allow it
    int const e0 = FindEdge( TriVertices[0], TriVertices[1] );
    int const e1 = FindEdge( TriVertices[1], TriVertices[2] );
    int const e2 = FindEdge( TriVertices[2], TriVertices[0] );
    if ( ( e0 != InvalidID && !IsBoundaryEdge( e0 ) ) || ( e1 != InvalidID && !IsBoundaryEdge( e1 ) ) ||
         ( e2 != InvalidID && !IsBoundaryEdge( e2 ) ) )
    {
        return MeshResult::Failed_WouldCreateNonmanifoldEdge;
    }

    bool const bOK = ( bUnsafe ) ? m_TriangleRefCounts.AllocateAtUnsafe( TriangleID )
                                 : m_TriangleRefCounts.AllocateAt( TriangleID );
    if ( !bOK )
    {
        return MeshResult::Failed_CannotAllocateTriangle;
    }

    // now safe to insert triangle
    m_Triangles.InsertAt( TriVertices, TriangleID );
    if ( HasTriangleGroups() )
    {
        m_TriangleGroups->InsertAt( GroupID, TriangleID );
        m_GroupIDCounter = std::max( m_GroupIDCounter, GroupID + 1 );
    }

    // increment ref counts and update/create edges
    m_VertexRefCounts.Increment( TriVertices[0] );
    m_VertexRefCounts.Increment( TriVertices[1] );
    m_VertexRefCounts.Increment( TriVertices[2] );

    AddTriangleEdge( TriangleID, TriVertices[0], TriVertices[1], 0, e0 );
    AddTriangleEdge( TriangleID, TriVertices[1], TriVertices[2], 1, e1 );
    AddTriangleEdge( TriangleID, TriVertices[2], TriVertices[0], 2, e2 );
    if ( HasAttributes() )
    {
        Attributes()->OnNewTriangle( TriangleID, true );
    }
    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

int32_t DynamicMesh3::RemoveUnusedVertices()
{
    int32_t NumRemoved = 0;
    for ( int32_t VID = 0; VID < MaxVertexID(); ++VID )
    {
        // If vertex exists but is not referenced by any triangles
        if ( m_VertexRefCounts.GetRefCount( VID ) == 1 )
        {
            NumRemoved++;
            m_VertexRefCounts.Decrement( VID );
            if ( HasAttributes() )
            {
                Attributes()->OnRemoveVertex( VID );
            }
            assert( m_VertexRefCounts.IsValid( VID ) == false ); // vertex should now not be valid
            assert( m_VertexEdgeLists.GetCount( VID ) == 0 );    // vertex should not have had any edges attached
        }
    }

    if ( NumRemoved > 0 )
    {
        UpdateChangeStamps( true, true );
    }
    return NumRemoved;
}

bool DynamicMesh3::HasUnusedVertices() const
{
    for ( int32_t VID = 0; VID < MaxVertexID(); ++VID )
    {
        // If vertex exists but is not referenced by any triangles
        if ( m_VertexRefCounts.GetRefCount( VID ) == 1 )
        {
            return true;
        }
    }

    return false;
}

void DynamicMesh3::CompactInPlace( DynamicMeshCompactMaps* CompactInfo )
{
    // Initialize CompactInfo
    // If we need a CompactInfo for compacting attributes but we don't have one, we'll make it refer to a local
    // one.
    DynamicMeshCompactMaps LocalCompactInfo;
    if ( HasAttributes() && ( CompactInfo == nullptr ) )
    {
        CompactInfo = &LocalCompactInfo;
    }
    if ( CompactInfo != nullptr )
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
    int iLastV = MaxVertexID() - 1;
    int iCurV  = 0;
    while ( iLastV >= 0 && !m_VertexRefCounts.IsValidUnsafe( iLastV ) )
    {
        iLastV--;
    }
    while ( iCurV < iLastV && m_VertexRefCounts.IsValidUnsafe( iCurV ) )
    {
        iCurV++;
    }

    DynamicVector<unsigned short>& vref = m_VertexRefCounts.GetRawRefCountsUnsafe();

    while ( iCurV < iLastV )
    {
        m_Vertices[iCurV] = m_Vertices[iLastV];

        // const int kc = iCurV * 3;
        // const int kl = iLastV * 3;
        if ( HasVertexNormals() )
        {
            DynamicVector<glm::vec3>& Normals = m_VertexNormals.value();
            Normals[iCurV]                    = Normals[iLastV];
        }
        if ( HasVertexColors() )
        {
            DynamicVector<glm::vec3>& Colors = m_VertexColors.value();
            Colors[iCurV]                    = Colors[iLastV];
        }
        if ( HasVertexUVs() )
        {
            DynamicVector<glm::vec2>& UVs = m_VertexUVs.value();
            UVs[iCurV]                    = UVs[iLastV];
        }

        for ( int const eid : m_VertexEdgeLists.Values( iLastV ) )
        {
            // replace vertex in edges
            ReplaceEdgeVertex( eid, iLastV, iCurV );

            // replace vertex in triangles
            const Index2i Tris = m_Edges[eid].Tri;
            ReplaceTriangleVertex( Tris[0], iLastV, iCurV );
            if ( Tris[1] != InvalidID )
            {
                ReplaceTriangleVertex( Tris[1], iLastV, iCurV );
            }
        }

        // shift vertex refcount to position
        vref[iCurV]  = vref[iLastV];
        vref[iLastV] = RefCountVector::INVALID_REF_COUNT;

        // move edge list
        m_VertexEdgeLists.Move( iLastV, iCurV );

        if ( CompactInfo != nullptr )
        {
            CompactInfo->SetVertexMapping( iLastV, iCurV );
        }

        // move cur forward one, last back one, and  then search for next valid
        iLastV--;
        iCurV++;
        while ( iLastV >= 0 && !m_VertexRefCounts.IsValidUnsafe( iLastV ) )
        {
            iLastV--;
        }
        while ( iCurV < iLastV && m_VertexRefCounts.IsValidUnsafe( iCurV ) )
        {
            iCurV++;
        }
    }

    // trim vertices data structures
    m_VertexRefCounts.Trim( VertexCount() );
    m_Vertices.Resize( VertexCount() );
    if ( HasVertexNormals() )
    {
        m_VertexNormals->Resize( VertexCount() * 3 );
    }
    if ( HasVertexColors() )
    {
        m_VertexColors->Resize( VertexCount() * 3 );
    }
    if ( HasVertexUVs() )
    {
        m_VertexUVs->Resize( VertexCount() * 2 );
    }

    m_VertexEdgeLists.Compact( VertexCount() );

    /** shift triangles **/

    // find first free triangle, and last valid triangle
    int iLastT = MaxTriangleID() - 1;
    int iCurT  = 0;
    while ( iLastT >= 0 && !m_TriangleRefCounts.IsValidUnsafe( iLastT ) )
    {
        iLastT--;
    }
    while ( iCurT < iLastT && m_TriangleRefCounts.IsValidUnsafe( iCurT ) )
    {
        iCurT++;
    }

    DynamicVector<unsigned short>& tref = m_TriangleRefCounts.GetRawRefCountsUnsafe();

    while ( iCurT < iLastT )
    {
        // shift triangle
        m_Triangles[iCurT]     = m_Triangles[iLastT];
        m_TriangleEdges[iCurT] = m_TriangleEdges[iLastT];

        if ( HasTriangleGroups() )
        {
            m_TriangleGroups.value()[iCurT] = m_TriangleGroups.value()[iLastT];
        }

        // update edges
        for ( int j = 0; j < 3; ++j )
        {
            int const eid = m_TriangleEdges[iCurT][j];
            ReplaceEdgeTriangle( eid, iLastT, iCurT );
        }

        // shift triangle refcount to position
        tref[iCurT]  = tref[iLastT];
        tref[iLastT] = RefCountVector::INVALID_REF_COUNT;

        if ( CompactInfo != nullptr )
        {
            CompactInfo->SetTriangleMapping( iLastT, iCurT );
        }

        // move cur forward one, last back one, and  then search for next valid
        iLastT--;
        iCurT++;
        while ( iLastT >= 0 && !m_TriangleRefCounts.IsValidUnsafe( iLastT ) )
        {
            iLastT--;
        }
        while ( iCurT < iLastT && m_TriangleRefCounts.IsValidUnsafe( iCurT ) )
        {
            iCurT++;
        }
    }

    // trim triangles data structures
    m_TriangleRefCounts.Trim( TriangleCount() );
    m_Triangles.Resize( TriangleCount() );
    m_TriangleEdges.Resize( TriangleCount() );
    if ( HasTriangleGroups() )
    {
        m_TriangleGroups->Resize( TriangleCount() );
    }

    /** shift edges **/

    // find first free edge, and last used edge
    int iLastE = MaxEdgeID() - 1;
    int iCurE  = 0;
    while ( iLastE >= 0 && !m_EdgeRefCounts.IsValidUnsafe( iLastE ) )
    {
        iLastE--;
    }
    while ( iCurE < iLastE && m_EdgeRefCounts.IsValidUnsafe( iCurE ) )
    {
        iCurE++;
    }

    DynamicVector<unsigned short>& eref = m_EdgeRefCounts.GetRawRefCountsUnsafe();

    while ( iCurE < iLastE )
    {
        m_Edges[iCurE] = m_Edges[iLastE];

        // replace edge in vertex edges lists
        int const v0 = m_Edges[iCurE].Vert[0];
        int const v1 = m_Edges[iCurE].Vert[1];
        m_VertexEdgeLists.Replace( v0, [iLastE]( int eid ) { return eid == iLastE; }, iCurE );
        m_VertexEdgeLists.Replace( v1, [iLastE]( int eid ) { return eid == iLastE; }, iCurE );

        // replace edge in triangles
        ReplaceTriangleEdge( m_Edges[iCurE].Tri[0], iLastE, iCurE );
        if ( m_Edges[iCurE].Tri[1] != InvalidID )
        {
            ReplaceTriangleEdge( m_Edges[iCurE].Tri[1], iLastE, iCurE );
        }

        // shift triangle refcount to position
        eref[iCurE]  = eref[iLastE];
        eref[iLastE] = RefCountVector::INVALID_REF_COUNT;

        // move cur forward one, last back one, and  then search for next valid
        iLastE--;
        iCurE++;
        while ( iLastE >= 0 && !m_EdgeRefCounts.IsValidUnsafe( iLastE ) )
        {
            iLastE--;
        }
        while ( iCurE < iLastE && m_EdgeRefCounts.IsValidUnsafe( iCurE ) )
        {
            iCurE++;
        }
    }

    // trim edge data structures
    m_EdgeRefCounts.Trim( EdgeCount() );
    m_Edges.Resize( EdgeCount() );

    if ( HasAttributes() )
    {
        assert( CompactInfo ); // can this ever fail?
        m_AttributeSet->CompactInPlace( *CompactInfo );
    }
}
MeshResult DynamicMesh3::ReverseTriOrientation( int TriangleID )
{
    if ( !IsTriangle( TriangleID ) )
    {
        return MeshResult::Failed_NotATriangle;
    }
    ReverseTriOrientationInternal( TriangleID );
    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

void DynamicMesh3::ReverseTriOrientationInternal( int TriangleID )
{
    Index3i t = GetTriangle( TriangleID );
    SetTriangleInternal( TriangleID, t[1], t[0], t[2] );
    Index3i te = GetTriEdges( TriangleID );
    SetTriangleEdgesInternal( TriangleID, te[0], te[2], te[1] );
    if ( HasAttributes() )
    {
        Attributes()->OnReverseTriOrientation( TriangleID );
    }
}
void DynamicMesh3::ReverseOrientation( bool bFlipNormals )
{
    for ( int const tid : TriangleIndicesItr() )
    {
        ReverseTriOrientationInternal( tid );
    }
    if ( bFlipNormals && m_VertexNormals.has_value() )
    {
        for ( int const vid : VertexIndicesItr() )
        {
            DynamicVector<glm::vec3>& Normals = m_VertexNormals.value();
            Normals[vid]                      = -Normals[vid];
        }
    }
    UpdateChangeStamps( true, true );
}

MeshResult DynamicMesh3::RemoveVertex( int VertexID, bool bPreserveManifold )
{
    if ( !m_VertexRefCounts.IsValid( VertexID ) )
    {
        return MeshResult::Failed_NotAVertex;
    }

    // if any one-ring vtx is a boundary vtx and one of its outer-ring edges is an
    // interior edge then we will create a bowtie if we remove that triangle
    if ( bPreserveManifold )
    {
        for ( int const tid : VtxTrianglesItr( VertexID ) )
        {
            Index3i   tri = GetTriangle( tid );
            int const j   = IndexUtil::FindTriIndex( VertexID, tri );
            int const oa  = tri[( j + 1 ) % 3];
            int const ob  = tri[( j + 2 ) % 3];
            int const eid = FindEdge( oa, ob );
            if ( IsBoundaryEdge( eid ) )
            {
                continue;
            }
            if ( IsBoundaryVertex( oa ) || IsBoundaryVertex( ob ) )
            {
                return MeshResult::Failed_WouldCreateBowtie;
            }
        }
    }

    // Remove incident triangles
    DynamicMesh3::LocalIntArray tris;
    GetVtxTriangles( VertexID, tris );
    for ( int const tID : tris )
    {
        MeshResult const result = RemoveTriangle( tID, false, bPreserveManifold );
        if ( result != MeshResult::Ok )
        {
            return result;
        }
    }

    if ( m_VertexRefCounts.GetRefCount( VertexID ) != 1 )
    {
        return MeshResult::Failed_VertexStillReferenced;
    }

    m_VertexRefCounts.Decrement( VertexID );
    DESERT_VERIFY_WARN( m_VertexRefCounts.IsValid( VertexID ) == false );
    m_VertexEdgeLists.Clear( VertexID );
    if ( HasAttributes() )
    {
        Attributes()->OnRemoveVertex( VertexID );
    }
    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

MeshResult DynamicMesh3::RemoveTriangle( int TriangleID, bool bRemoveIsolatedVertices, bool bPreserveManifold )
{
    if ( !m_TriangleRefCounts.IsValid( TriangleID ) )
    {
        DESERT_VERIFY_WARN( false );
        return MeshResult::Failed_NotATriangle;
    }

    Index3i tv = GetTriangle( TriangleID );
    Index3i te = GetTriEdges( TriangleID );

    // if any tri vtx is a boundary vtx connected to two interior edges, then
    // we cannot remove this triangle because it would create a bowtie vertex!
    // (that vtx already has 2 boundary edges, and we would add two more)
    if ( bPreserveManifold )
    {
        for ( int j = 0; j < 3; ++j )
        {
            if ( IsBoundaryVertex( tv[j] ) )
            {
                if ( !IsBoundaryEdge( te[j] ) && !IsBoundaryEdge( te[( j + 2 ) % 3] ) )
                {
                    return MeshResult::Failed_WouldCreateBowtie;
                }
            }
        }
    }

    // Remove triangle from its edges. if edge has no triangles left,
    // then it is removed.
    for ( int j = 0; j < 3; ++j )
    {
        int const eid = te[j];
        ReplaceEdgeTriangle( eid, TriangleID, InvalidID );
        const Edge Edge = m_Edges[eid];
        if ( Edge.Tri[0] == InvalidID )
        {
            int const a = Edge.Vert[0];
            m_VertexEdgeLists.Remove( a, eid );

            int const b = Edge.Vert[1];
            m_VertexEdgeLists.Remove( b, eid );

            m_EdgeRefCounts.Decrement( eid );
        }
    }

    // free this triangle
    m_TriangleRefCounts.Decrement( TriangleID );
    assert( m_TriangleRefCounts.IsValid( TriangleID ) == false );

    // Decrement vertex refcounts. If any hit 1 and we got remove-isolated flag,
    // we need to remove that vertex
    for ( int j = 0; j < 3; ++j )
    {
        int const vid = tv[j];
        m_VertexRefCounts.Decrement( vid );
        if ( bRemoveIsolatedVertices && m_VertexRefCounts.GetRefCount( vid ) == 1 )
        {
            m_VertexRefCounts.Decrement( vid );
            assert( m_VertexRefCounts.IsValid( vid ) == false );
            m_VertexEdgeLists.Clear( vid );
        }
    }
    if ( HasAttributes() )
    {
        Attributes()->OnRemoveTriangle( TriangleID );
    }
    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

MeshResult DynamicMesh3::SetTriangle( int TriangleID, const Index3i& newv, bool bRemoveIsolatedVertices )
{
    // UE 5.8 writes `if (ensure(HasAttributes()) == false)`, which rejects exactly the meshes it can handle:
    // SetTriangle does not update overlays, so it is meshes WITH attributes that must be refused.
    if ( !Common::EnsureOrWarn( !HasAttributes(), "!HasAttributes()" ) )
    {
        return MeshResult::Failed_Unsupported;
    }
    Index3i tv = GetTriangle( TriangleID );
    Index3i te = GetTriEdges( TriangleID );
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

    if ( !m_TriangleRefCounts.IsValid( TriangleID ) )
    {
        assert( false );
        return MeshResult::Failed_NotATriangle;
    }
    if ( !IsVertex( newv[0] ) || !IsVertex( newv[1] ) || !IsVertex( newv[2] ) )
    {
        assert( false );
        return MeshResult::Failed_NotAVertex;
    }
    if ( newv[0] == newv[1] || newv[0] == newv[2] || newv[1] == newv[2] )
    {
        assert( false );
        return MeshResult::Failed_BrokenTopology;
    }
    // look up edges. if any already have two triangles, this would
    // create non-manifold geometry and so we do not allow it
    int const e0 = FindEdge( newv[0], newv[1] );
    int const e1 = FindEdge( newv[1], newv[2] );
    int const e2 = FindEdge( newv[2], newv[0] );
    if ( ( te[0] != -1 && e0 != InvalidID && !IsBoundaryEdge( e0 ) ) ||
         ( te[1] != -1 && e1 != InvalidID && !IsBoundaryEdge( e1 ) ) ||
         ( te[2] != -1 && e2 != InvalidID && !IsBoundaryEdge( e2 ) ) )
    {
        return MeshResult::Failed_BrokenTopology;
    }

    // [TODO] check that we are not going to create invalid stuff...

    // Remove triangle from its edges. if edge has no triangles left, then it is removed.
    for ( int j = 0; j < 3; ++j )
    {
        int const eid = te[j];
        if ( eid == -1 ) // we don't need to modify this edge
        {
            continue;
        }
        ReplaceEdgeTriangle( eid, TriangleID, InvalidID );
        const Edge Edge = GetEdge( eid );
        if ( Edge.Tri[0] == InvalidID )
        {
            int const a = Edge.Vert[0];
            m_VertexEdgeLists.Remove( a, eid );

            int const b = Edge.Vert[1];
            m_VertexEdgeLists.Remove( b, eid );

            m_EdgeRefCounts.Decrement( eid );
        }
    }

    // Decrement vertex refcounts. If any hit 1 and we got remove-isolated flag,
    // we need to remove that vertex
    for ( int j = 0; j < 3; ++j )
    {
        int const vid = tv[j];
        if ( vid == newv[j] ) // we don't need to modify this vertex
        {
            continue;
        }
        m_VertexRefCounts.Decrement( vid );
        if ( bRemoveIsolatedVertices && m_VertexRefCounts.GetRefCount( vid ) == 1 )
        {
            m_VertexRefCounts.Decrement( vid );
            assert( m_VertexRefCounts.IsValid( vid ) == false );
            m_VertexEdgeLists.Clear( vid );
        }
    }

    // ok now re-insert with vertices
    for ( int j = 0; j < 3; ++j )
    {
        if ( newv[j] != tv[j] )
        {
            m_Triangles[TriangleID][j] = newv[j];
            m_VertexRefCounts.Increment( newv[j] );
        }
    }

    if ( te[0] != -1 )
    {
        AddTriangleEdge( TriangleID, newv[0], newv[1], 0, e0 );
    }
    if ( te[1] != -1 )
    {
        AddTriangleEdge( TriangleID, newv[1], newv[2], 1, e1 );
    }
    if ( te[2] != -1 )
    {
        AddTriangleEdge( TriangleID, newv[2], newv[0], 2, e2 );
    }

    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

MeshResult DynamicMesh3::SplitEdge( int eab, EdgeSplitInfo& SplitInfo, double SplitParameterT )
{
    SplitInfo = EdgeSplitInfo();

    if ( !IsEdge( eab ) )
    {
        return MeshResult::Failed_NotAnEdge;
    }

    // look up primary edge & triangle
    const Edge Edge = m_Edges[eab];
    int        a    = Edge.Vert[0];
    int        b    = Edge.Vert[1];
    int const  t0   = Edge.Tri[0];
    if ( t0 == InvalidID )
    {
        return MeshResult::Failed_BrokenTopology;
    }
    Index3i const T0tv = GetTriangle( t0 );
    int const     c    = IndexUtil::OrientTriEdgeAndFindOtherVtx( a, b, T0tv );

    // RefCount overflow check. Conservatively leave room for
    // extra increments from other operations.
    if ( m_VertexRefCounts.GetRawRefCount( c ) > RefCountVector::INVALID_REF_COUNT - 3 )
    {
        return MeshResult::Failed_HitValenceLimit;
    }
    if ( a != Edge.Vert[0] )
    {
        SplitParameterT = 1.0 - SplitParameterT; // if we flipped a/b order we need to reverse t
    }

    SplitInfo.OriginalEdge      = eab;
    SplitInfo.OriginalVertices  = Index2i( a, b ); // this is the oriented a,b
    SplitInfo.OriginalTriangles = Index2i( t0, InvalidID );
    SplitInfo.SplitT            = SplitParameterT;

    // quite a bit of code is duplicated between boundary and non-boundary case, but it
    //  is too hard to follow later if we factor it out...
    if ( IsBoundaryEdge( eab ) )
    {
        // create vertex
        glm::dvec3 const vNew = Lerp( GetVertex( a ), GetVertex( b ), SplitParameterT );
        int const        f    = AppendVertex( vNew );
        if ( HasVertexNormals() )
        {
            SetVertexNormal( f, Normalized( Lerp( GetVertexNormal( a ), GetVertexNormal( b ),
                                                  static_cast<float>( SplitParameterT ) ) ) );
        }
        if ( HasVertexColors() )
        {
            SetVertexColor(
                 f, Lerp( GetVertexColor( a ), GetVertexColor( b ), static_cast<float>( SplitParameterT ) ) );
        }
        if ( HasVertexUVs() )
        {
            SetVertexUV( f, Lerp( GetVertexUV( a ), GetVertexUV( b ), static_cast<float>( SplitParameterT ) ) );
        }

        // look up edge bc, which needs to be modified
        Index3i   T0te = GetTriEdges( t0 );
        int const ebc  = T0te[IndexUtil::FindEdgeIndexInTri( b, c, T0tv )];

        // rewrite existing triangle
        ReplaceTriangleVertex( t0, b, f );

        // add second triangle
        int const t2 = AddTriangleInternal( f, b, c, InvalidID, InvalidID, InvalidID );
        if ( m_TriangleGroups.has_value() )
        {
            int const group0 = m_TriangleGroups.value()[t0];
            m_TriangleGroups->InsertAt( group0, t2 );
        }

        // rewrite edge bc, create edge af
        ReplaceEdgeTriangle( ebc, t0, t2 );
        int const eaf = eab;
        ReplaceEdgeVertex( eaf, b, f );
        m_VertexEdgeLists.Remove( b, eab );
        m_VertexEdgeLists.Insert( f, eaf );

        // create edges fb and fc
        int const efb = AddEdgeInternal( f, b, t2 );
        int const efc = AddEdgeInternal( f, c, t0, t2 );

        // update triangle edge-nbrs
        ReplaceTriangleEdge( t0, ebc, efc );
        SetTriangleEdgesInternal( t2, efb, ebc, efc );

        // update vertex refcounts
        m_VertexRefCounts.Increment( c );
        m_VertexRefCounts.Increment( f, 2 );

        SplitInfo.bIsBoundary   = true;
        SplitInfo.OtherVertices = Index2i( c, InvalidID );
        SplitInfo.NewVertex     = f;
        SplitInfo.NewEdges      = Index3i( efb, efc, InvalidID );
        SplitInfo.NewTriangles  = Index2i( t2, InvalidID );

        if ( HasAttributes() )
        {
            Attributes()->OnSplitEdge( SplitInfo );
        }

        UpdateChangeStamps( true, true );
        return MeshResult::Ok;
    }
    // interior triangle branch
    // look up other triangle
    int const t1                  = m_Edges[eab].Tri[1];
    SplitInfo.OriginalTriangles.B = t1;
    Index3i const T1tv            = GetTriangle( t1 );
    int const     d               = IndexUtil::FindTriOtherVtx( a, b, T1tv );

    // RefCount overflow check. Conservatively leave room for
    // extra increments from other operations.
    if ( m_VertexRefCounts.GetRawRefCount( d ) > RefCountVector::INVALID_REF_COUNT - 3 )
    {
        return MeshResult::Failed_HitValenceLimit;
    }

    // create vertex
    glm::dvec3 const vNew = Lerp( GetVertex( a ), GetVertex( b ), SplitParameterT );
    int const        f    = AppendVertex( vNew );
    if ( HasVertexNormals() )
    {
        SetVertexNormal( f, Normalized( Lerp( GetVertexNormal( a ), GetVertexNormal( b ),
                                              static_cast<float>( SplitParameterT ) ) ) );
    }
    if ( HasVertexColors() )
    {
        SetVertexColor( f,
                        Lerp( GetVertexColor( a ), GetVertexColor( b ), static_cast<float>( SplitParameterT ) ) );
    }
    if ( HasVertexUVs() )
    {
        SetVertexUV( f, Lerp( GetVertexUV( a ), GetVertexUV( b ), static_cast<float>( SplitParameterT ) ) );
    }

    // look up edges that we are going to need to update
    // [TODO OPT] could use ordering to reduce # of compares here
    Index3i   T0te = GetTriEdges( t0 );
    int const ebc  = T0te[IndexUtil::FindEdgeIndexInTri( b, c, T0tv )];
    Index3i   T1te = GetTriEdges( t1 );
    int const edb  = T1te[IndexUtil::FindEdgeIndexInTri( d, b, T1tv )];

    // rewrite existing triangles
    ReplaceTriangleVertex( t0, b, f );
    ReplaceTriangleVertex( t1, b, f );

    // add two triangles to close holes we just created
    int const t2 = AddTriangleInternal( f, b, c, InvalidID, InvalidID, InvalidID );
    int const t3 = AddTriangleInternal( f, d, b, InvalidID, InvalidID, InvalidID );
    if ( m_TriangleGroups.has_value() )
    {
        int const group0 = m_TriangleGroups.value()[t0];
        m_TriangleGroups->InsertAt( group0, t2 );
        int const group1 = m_TriangleGroups.value()[t1];
        m_TriangleGroups->InsertAt( group1, t3 );
    }

    // update the edges we found above, to point to triangles
    ReplaceEdgeTriangle( ebc, t0, t2 );
    ReplaceEdgeTriangle( edb, t1, t3 );

    // edge eab became eaf
    int const eaf = eab; // Edge * eAF = eAB;
    ReplaceEdgeVertex( eaf, b, f );

    // update a/b/f vertex-edges
    m_VertexEdgeLists.Remove( b, eab );
    m_VertexEdgeLists.Insert( f, eaf );

    // create edges connected to f  (also updates vertex-edges)
    int const efb = AddEdgeInternal( f, b, t2, t3 );
    int const efc = AddEdgeInternal( f, c, t0, t2 );
    int const edf = AddEdgeInternal( d, f, t1, t3 );

    // update triangle edge-nbrs
    ReplaceTriangleEdge( t0, ebc, efc );
    ReplaceTriangleEdge( t1, edb, edf );
    SetTriangleEdgesInternal( t2, efb, ebc, efc );
    SetTriangleEdgesInternal( t3, edf, edb, efb );

    // update vertex refcounts
    m_VertexRefCounts.Increment( c );
    m_VertexRefCounts.Increment( d );
    m_VertexRefCounts.Increment( f, 4 );

    SplitInfo.bIsBoundary   = false;
    SplitInfo.OtherVertices = Index2i( c, d );
    SplitInfo.NewVertex     = f;
    SplitInfo.NewEdges      = Index3i( efb, efc, edf );
    SplitInfo.NewTriangles  = Index2i( t2, t3 );

    if ( HasAttributes() )
    {
        Attributes()->OnSplitEdge( SplitInfo );
    }

    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

MeshResult DynamicMesh3::SplitEdge( int EdgeVertA, int EdgeVertB, EdgeSplitInfo& SplitInfo )
{
    int const eid = FindEdge( EdgeVertA, EdgeVertB );
    if ( eid == InvalidID )
    {
        SplitInfo = EdgeSplitInfo();
        return MeshResult::Failed_NotAnEdge;
    }
    return SplitEdge( eid, SplitInfo, 0.5 ); // midpoint, as UE
}

MeshResult DynamicMesh3::FlipEdge( int eab, EdgeFlipInfo& FlipInfo )
{
    FlipInfo = EdgeFlipInfo();

    if ( !IsEdge( eab ) )
    {
        return MeshResult::Failed_NotAnEdge;
    }
    if ( IsBoundaryEdge( eab ) )
    {
        return MeshResult::Failed_IsBoundaryEdge;
    }

    // find oriented edge [a,b], tris t0,t1, and other verts c in t0, d in t1
    const Edge    Edge = m_Edges[eab];
    int           a    = Edge.Vert[0];
    int           b    = Edge.Vert[1];
    int const     t0   = Edge.Tri[0];
    int const     t1   = Edge.Tri[1];
    Index3i const T0tv = GetTriangle( t0 );
    Index3i const T1tv = GetTriangle( t1 );
    int const     c    = IndexUtil::OrientTriEdgeAndFindOtherVtx( a, b, T0tv );
    int const     d    = IndexUtil::FindTriOtherVtx( a, b, T1tv );
    if ( c == InvalidID || d == InvalidID )
    {
        return MeshResult::Failed_BrokenTopology;
    }

    int const flipped = FindEdge( c, d );
    if ( flipped != InvalidID )
    {
        return MeshResult::Failed_FlippedEdgeExists;
    }

    // find edges bc, ca, ad, db
    int const ebc = FindTriangleEdge( t0, b, c );
    int const eca = FindTriangleEdge( t0, c, a );
    int const ead = FindTriangleEdge( t1, a, d );
    int const edb = FindTriangleEdge( t1, d, b );

    // update triangles
    SetTriangleInternal( t0, c, d, b );
    SetTriangleInternal( t1, d, c, a );

    // update edge AB, which becomes flipped edge CD
    SetEdgeVerticesInternal( eab, c, d );
    SetEdgeTrianglesInternal( eab, t0, t1 );
    int const ecd = eab;

    // update the two other edges whose triangle nbrs have changed
    if ( ReplaceEdgeTriangle( eca, t0, t1 ) == -1 )
    {
        assert( ( false ) && "DynamicMesh3.FlipEdge: first ReplaceEdgeTriangle failed" );
        return MeshResult::Failed_UnrecoverableError;
    }
    if ( ReplaceEdgeTriangle( edb, t1, t0 ) == -1 )
    {
        assert( ( false ) && "DynamicMesh3.FlipEdge: second ReplaceEdgeTriangle failed" );
        return MeshResult::Failed_UnrecoverableError;
    }

    // update triangle nbr lists (these are edges)
    SetTriangleEdgesInternal( t0, ecd, edb, ebc );
    SetTriangleEdgesInternal( t1, ecd, eca, ead );

    // remove old eab from verts a and b, and Decrement ref counts
    if ( !m_VertexEdgeLists.Remove( a, eab ) )
    {
        assert( ( false ) && "DynamicMesh3.FlipEdge: first edge list remove failed" );
        return MeshResult::Failed_UnrecoverableError;
    }
    if ( !m_VertexEdgeLists.Remove( b, eab ) )
    {
        assert( ( false ) && "DynamicMesh3.FlipEdge: second edge list remove failed" );
        return MeshResult::Failed_UnrecoverableError;
    }
    m_VertexRefCounts.Decrement( a );
    m_VertexRefCounts.Decrement( b );
    if ( !IsVertex( a ) || !IsVertex( b ) )
    {
        assert( ( false ) && "DynamicMesh3.FlipEdge: either a or b is not a vertex?" );
        return MeshResult::Failed_UnrecoverableError;
    }

    // add edge ecd to verts c and d, and increment ref counts
    m_VertexEdgeLists.Insert( c, ecd );
    m_VertexEdgeLists.Insert( d, ecd );
    m_VertexRefCounts.Increment( c );
    m_VertexRefCounts.Increment( d );

    // success! collect up results
    FlipInfo.EdgeID        = eab;
    FlipInfo.OriginalVerts = Index2i( a, b );
    FlipInfo.OpposingVerts = Index2i( c, d );
    FlipInfo.Triangles     = Index2i( t0, t1 );

    if ( HasAttributes() )
    {
        Attributes()->OnFlipEdge( FlipInfo );
    }

    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

MeshResult DynamicMesh3::FlipEdge( int EdgeVertA, int EdgeVertB, EdgeFlipInfo& FlipInfo )
{
    int const eid = FindEdge( EdgeVertA, EdgeVertB );
    if ( eid == InvalidID )
    {
        FlipInfo = EdgeFlipInfo();
        return MeshResult::Failed_NotAnEdge;
    }
    return FlipEdge( eid, FlipInfo );
}

MeshResult DynamicMesh3::SplitVertex( int VertexID, const std::span<const int>& TrianglesToUpdate,
                                      VertexSplitInfo& SplitInfo )
{
    if ( !Common::EnsureOrWarn( IsVertex( VertexID ), "IsVertex( VertexID )" ) )
    {
        return MeshResult::Failed_NotAVertex;
    }

    SplitInfo.OriginalVertex = VertexID;
    SplitInfo.NewVertex      = AppendVertex( *this, VertexID );

    // Membership tests below scan TrianglesToUpdate linearly; UE notes a set copy would pay off when it is large.
    auto ProcessEdge =
         [this, &TrianglesToUpdate, &SplitInfo]( int TriID, Index3i& UpdatedTri, Index3i& TriEdges, int SubIdx )
    {
        int const  EdgeID   = TriEdges[SubIdx];
        int const  OtherTri = GetOtherEdgeTriangle( EdgeID, TriID );
        bool const bNewBoundary =
             OtherTri >= 0 && !( std::find( TrianglesToUpdate.begin(), TrianglesToUpdate.end(), OtherTri ) !=
                                 TrianglesToUpdate.end() ); // processing this edge will create a new boundary
        if ( bNewBoundary ) // there *is* a triangle across from this edge and we do need to separate from it
        {
            ReplaceEdgeTriangle( EdgeID, TriID,
                                 InvalidID ); // remove TriID from original edge, disconnecting it from OtherTri
            // add a new edge for TriID connecting the updated tri vertices
            AddTriangleEdge( TriID, UpdatedTri[SubIdx], UpdatedTri[( SubIdx + 1 ) % 3], SubIdx,
                             InvalidID ); // adds to vertexedgelists
        }
        else // othertri invalid or also in set
        {
            // if OtherTri already was processed and replaced edge, ReplaceEdgeVertex will return InvalidID and do
            // nothing
            if ( ReplaceEdgeVertex( EdgeID, SplitInfo.OriginalVertex, SplitInfo.NewVertex ) != InvalidID )
            {
                // if replace edge actually happened, also update VertexEdgeLists accordingly
                DESERT_VERIFY_WARN( m_VertexEdgeLists.Remove( SplitInfo.OriginalVertex, EdgeID ) );
                m_VertexEdgeLists.Insert( SplitInfo.NewVertex, EdgeID );
            }
        }
    };
    for ( int const TriID : TrianglesToUpdate )
    {
        Index3i   Triangle = GetTriangle( TriID );
        int const SubIdx   = Triangle.IndexOf( VertexID );
        if ( SubIdx < 0 )
        {
            continue;
        }
        Triangle[SubIdx] = SplitInfo.NewVertex; // update local copy w/ new vertex, for use by ProcessEdge helper
        Index3i TriEdges = GetTriEdges( TriID );
        ProcessEdge( TriID, Triangle, TriEdges, SubIdx );
        ProcessEdge( TriID, Triangle, TriEdges, ( SubIdx + 2 ) % 3 );

        m_Triangles[TriID][SubIdx] = SplitInfo.NewVertex;
        m_VertexRefCounts.Decrement( SplitInfo.OriginalVertex ); // remove the triangle from the original vertex
        m_VertexRefCounts.Increment( SplitInfo.NewVertex );
    }

    if ( HasAttributes() )
    {
        Attributes()->OnSplitVertex( SplitInfo, TrianglesToUpdate );
    }
    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

bool DynamicMesh3::SplitVertexWouldLeaveIsolated( int VertexID, const std::span<const int>& TrianglesToUpdate )
{
    // Isolated only when every triangle around the vertex moves to the new one; any other keeps the old VertexID.
    auto Triangles = VtxTrianglesItr( VertexID );
    return std::all_of( Triangles.begin(), Triangles.end(), [&TrianglesToUpdate]( int TID )
                        { return std::ranges::find( TrianglesToUpdate, TID ) != TrianglesToUpdate.end(); } );
}

MeshResult DynamicMesh3::CanCollapseEdgeInternal( int vKeep, int vRemove, double collapse_t,
                                                  EdgeCollapseInfo* OutCollapseInfo ) const
{
    return CanCollapseEdgeInternal( vKeep, vRemove, collapse_t, CollapseEdgeOptions(), OutCollapseInfo );
}

MeshResult DynamicMesh3::CanCollapseEdgeInternal( int vKeep, int vRemove, double collapse_t,
                                                  const CollapseEdgeOptions& Options,
                                                  EdgeCollapseInfo*          OutCollapseInfo ) const
{
    if ( !IsVertex( vKeep ) || !IsVertex( vRemove ) )
    {
        return MeshResult::Failed_NotAnEdge;
    }

    int const b = vKeep; // renaming for sanity. We remove a and keep b
    int const a = vRemove;

    int const eab = FindEdge( a, b );
    if ( eab == InvalidID )
    {
        return MeshResult::Failed_NotAnEdge;
    }

    const Edge EdgeAB = m_Edges[eab];
    int const  t0     = EdgeAB.Tri[0];
    if ( t0 == InvalidID )
    {
        return MeshResult::Failed_BrokenTopology;
    }
    Index3i const T0tv = GetTriangle( t0 );
    int const     c    = IndexUtil::FindTriOtherVtx( a, b, T0tv );

    // look up opposing triangle/vtx if we are not in boundary case
    bool      bIsBoundaryEdge = false;
    int       d               = InvalidID;
    int const t1              = EdgeAB.Tri[1];
    if ( t1 != InvalidID )
    {
        Index3i const T1tv = GetTriangle( t1 );
        d                  = IndexUtil::FindTriOtherVtx( a, b, T1tv );
        if ( c == d )
        {
            return MeshResult::Failed_FoundDuplicateTriangle;
        }
    }
    else
    {
        bIsBoundaryEdge = true;
    }

    // We cannot collapse if there is some other vertex x that is connected to both a and b,
    //  and either xa or xb is an interior edge. In other words, if there are more than two
    //  triangles that have edge xa or xb, then after collapsing a to b, we'll end up with more
    //  than two triangles trying to share edge xb, which is disallowed.
    // Additionally, depending on options, we might not even allow such a collapse even if the
    //  both edges are boundary edges.
    int const edges_a_count = m_VertexEdgeLists.GetCount( a );
    int       eac           = InvalidID;
    int       ead           = InvalidID;
    int       ebc           = InvalidID;
    int       ebd           = InvalidID;
    for ( int const eid_a : m_VertexEdgeLists.Values( a ) )
    {
        int const vax = GetOtherEdgeVertex( eid_a, a );
        if ( vax == c )
        {
            eac = eid_a;
            continue;
        }
        if ( vax == d )
        {
            ead = eid_a;
            continue;
        }
        if ( vax == b )
        {
            continue;
        }
        for ( int const eid_b : m_VertexEdgeLists.Values( b ) )
        {
            if ( GetOtherEdgeVertex( eid_b, b ) == vax )
            {
                if ( !Options.bAllowHoleCollapse || !IsBoundaryEdge( eid_b ) || !IsBoundaryEdge( eid_a ) )
                {
                    return MeshResult::Failed_InvalidNeighbourhood;
                }
                break;
            }
        }
    }

    // We may not allow collapse if we have a tetrahedron. In this case a has 3 nbr edges,
    //  and edge cd exists. But that is not conclusive, we also have to check that
    //  cd is an internal edge, and that each of its tris contain a or b
    if ( !Options.bAllowTetrahedronCollapse && edges_a_count == 3 && !bIsBoundaryEdge )
    {
        int const edc = FindEdge( d, c );
        if ( edc != InvalidID )
        {
            const Edge EdgeDC = m_Edges[edc];
            if ( EdgeDC.Tri[1] != InvalidID )
            {
                int const edc_t0 = EdgeDC.Tri[0];
                int const edc_t1 = EdgeDC.Tri[1];

                if ( ( TriangleHasVertex( edc_t0, a ) && TriangleHasVertex( edc_t1, b ) ) ||
                     ( TriangleHasVertex( edc_t0, b ) && TriangleHasVertex( edc_t1, a ) ) )
                {
                    return MeshResult::Failed_CollapseTetrahedron;
                }
            }
        }
    }
    else if ( bIsBoundaryEdge && IsBoundaryEdge( eac ) )
    {
        // Cannot collapse edge if we are down to a single triangle
        ebc = FindEdgeFromTri( b, c, t0 );
        if ( IsBoundaryEdge( ebc ) )
        {
            return MeshResult::Failed_CollapseTriangle;
        }
    }

    // We might not allow collapsing an edge where both vertices are boundary vertices
    //  because that would sometimes create a bowtie
    //
    // NOTE: potentially scanning all edges here...couldn't we
    //  pick up eac/bc/ad/bd as we go? somehow?
    if ( !Options.bAllowCollapsingInternalEdgeWithBoundaryVertices && !bIsBoundaryEdge && IsBoundaryVertex( a ) &&
         IsBoundaryVertex( b ) )
    {
        return MeshResult::Failed_InvalidNeighbourhood;
    }

    // If we're allowing internal edge collapse with boundary vertices, we open the possibility
    //  to another place where we would end up collapsing away an entire component- a quad. As
    //  with single and double sided triangles, we currently disallow collapse in this case.
    // Note that if we ever add an option to allow this, we have to deal with the possibility
    //  that our kept vert (vKeep) may not actually survive a collapse.
    if ( Options.bAllowCollapsingInternalEdgeWithBoundaryVertices && !bIsBoundaryEdge && IsBoundaryEdge( eac ) &&
         IsBoundaryEdge( ead ) )
    {
        ebc = FindEdgeFromTri( b, c, t0 );
        if ( IsBoundaryEdge( ebc ) )
        {
            ebd = FindEdgeFromTri( b, d, t1 );
            if ( IsBoundaryEdge( ebd ) )
            {
                return MeshResult::Failed_CollapseQuad;
            }
        }
    }

    if ( OutCollapseInfo != nullptr )
    {
        OutCollapseInfo->OpposingVerts = Index2i( c, d );
        OutCollapseInfo->KeptVertex    = b;
        OutCollapseInfo->RemovedVertex = a;
        OutCollapseInfo->bIsBoundary   = bIsBoundaryEdge;
        OutCollapseInfo->CollapsedEdge = eab;
        OutCollapseInfo->RemovedTris   = Index2i( t0, t1 );
        OutCollapseInfo->RemovedEdges  = Index2i( eac, ead );
        OutCollapseInfo->KeptEdges     = Index2i( ebc, ebd );
        OutCollapseInfo->CollapseT     = collapse_t;
    }

    return MeshResult::Ok;
}

MeshResult DynamicMesh3::CanCollapseEdge( int vKeep, int vRemove, double EdgeParameterT ) const
{
    // EdgeParameterT only reaches the (absent) out-info, so passing it through keeps UE's result for every value.
    return CanCollapseEdgeInternal( vKeep, vRemove, EdgeParameterT, CollapseEdgeOptions(), nullptr );
}

MeshResult DynamicMesh3::CanCollapseEdge( int vKeep, int vRemove, const CollapseEdgeOptions& Options ) const
{
    return CanCollapseEdgeInternal( vKeep, vRemove, 0, Options, nullptr );
}

MeshResult DynamicMesh3::CollapseEdge( int KeepVertID, int RemoveVertID, double EdgeParameterT,
                                       EdgeCollapseInfo& CollapseInfo )
{
    return CollapseEdge( KeepVertID, RemoveVertID, EdgeParameterT, CollapseEdgeOptions(), CollapseInfo );
}

MeshResult DynamicMesh3::CollapseEdge( int KeepVertID, int RemoveVertID, double EdgeParameterT,
                                       const CollapseEdgeOptions& Options, EdgeCollapseInfo& CollapseInfo )
{
    CollapseInfo = EdgeCollapseInfo();

    const MeshResult CanCollapseResult =
         CanCollapseEdgeInternal( KeepVertID, RemoveVertID, EdgeParameterT, Options, &CollapseInfo );
    if ( CanCollapseResult != MeshResult::Ok )
    {
        return CanCollapseResult;
    }

    const int  b               = KeepVertID; // renaming for sanity. We remove a and keep b
    const int  a               = RemoveVertID;
    const int  c               = CollapseInfo.OpposingVerts[0];
    const int  d               = CollapseInfo.OpposingVerts[1];
    const int  t0              = CollapseInfo.RemovedTris[0];
    const int  t1              = CollapseInfo.RemovedTris[1];
    const int  eab             = CollapseInfo.CollapsedEdge;
    const bool bIsBoundaryEdge = CollapseInfo.bIsBoundary;
    const int  eac             = CollapseInfo.RemovedEdges[0];
    const int  ead             = CollapseInfo.RemovedEdges[1];

    // This may or may not have been computed already
    int ebc = CollapseInfo.KeptEdges[0];
    int ebd = InvalidID;

    // save vertex positions before we delete removed (can defer kept?)
    glm::dvec3 const KeptPos    = GetVertex( KeepVertID );
    glm::dvec3 const RemovedPos = GetVertex( RemoveVertID );
    glm::vec2        RemovedUV{};
    if ( HasVertexUVs() )
    {
        RemovedUV = GetVertexUV( RemoveVertID );
    }
    glm::vec3 RemovedNormal{};
    if ( HasVertexNormals() )
    {
        RemovedNormal = GetVertexNormal( RemoveVertID );
    }
    glm::vec3 RemovedColor{};
    if ( HasVertexColors() )
    {
        RemovedColor = GetVertexColor( RemoveVertID );
    }

    // 1) remove edge ab from vtx b
    // 2) find edges ad and ac, and tris tad, tac across those edges  (will use later)
    // 3) for other edges, replace a with b, and add that edge to b
    // 4) replace a with b in all triangles connected to a
    int tad = InvalidID;
    int tac = InvalidID;
    for ( int const eid : m_VertexEdgeLists.Values( a ) )
    {
        int const o = GetOtherEdgeVertex( eid, a );
        if ( o == b )
        {
            if ( !m_VertexEdgeLists.Remove( b, eid ) )
            {
                assert( ( false ) && "DynamicMesh3::CollapseEdge: failed at remove case o == b" );
                return MeshResult::Failed_UnrecoverableError;
            }
        }
        else if ( o == c )
        {
            if ( !m_VertexEdgeLists.Remove( c, eid ) )
            {
                assert( ( false ) && "DynamicMesh3::CollapseEdge: failed at remove case o == c" );
                return MeshResult::Failed_UnrecoverableError;
            }
            tac = GetOtherEdgeTriangle( eid, t0 );
        }
        else if ( o == d )
        {
            if ( !m_VertexEdgeLists.Remove( d, eid ) )
            {
                assert( ( false ) && "DynamicMesh3::CollapseEdge: failed at remove case o == c, step 1" );
                return MeshResult::Failed_UnrecoverableError;
            }
            tad = GetOtherEdgeTriangle( eid, t1 );
        }
        else
        {
            // This is some edge oa, not in a triangle with ab, that we can in most cases change to be ob. However
            // it's
            //  possible that ob already exists as a boundary edge, and we need to instead weld the triangle
            //  incident to oa to that edge. This situation is only permitted by CanCollapseEdgeInternal if
            //  bAllowHoleCollapse is true and eid is a boundary edge.
            int32_t const ExistingEdge =
                 ( Options.bAllowHoleCollapse && IsBoundaryEdge( eid ) ) ? FindEdge( o, b ) : InvalidID;
            if ( ExistingEdge != InvalidID )
            {
                int32_t const WeldedTriangle = GetEdgeT( eid ).A;
                if ( ReplaceTriangleEdge( WeldedTriangle, eid, ExistingEdge ) == -1 ||
                     ReplaceEdgeTriangle( ExistingEdge, InvalidID, WeldedTriangle ) == -1 )
                {
                    assert( ( false ) && "DynamicMesh3::CollapseEdge: failed at remove case else" );
                    return MeshResult::Failed_UnrecoverableError;
                }
                // Edge (o,a) should no longer exist
                m_VertexEdgeLists.Remove( o, eid );
                m_EdgeRefCounts.Decrement( eid );
                assert( m_EdgeRefCounts.IsValid( eid ) == false );
            }
            else
            {
                if ( ReplaceEdgeVertex( eid, a, b ) == -1 )
                {
                    assert( ( false ) && "DynamicMesh3::CollapseEdge: failed at remove case else" );
                    return MeshResult::Failed_UnrecoverableError;
                }
                m_VertexEdgeLists.Insert( b, eid );
            }
        }

        // [TODO] perhaps we can already have unique tri list because of the manifold-nbrhood check we need to
        // do...
        const Edge Edge = m_Edges[eid];
        for ( int j = 0; j < 2; ++j )
        {
            int const t_j = Edge.Tri[j];
            if ( t_j != InvalidID && t_j != t0 && t_j != t1 )
            {
                if ( TriangleHasVertex( t_j, a ) )
                {
                    if ( ReplaceTriangleVertex( t_j, a, b ) == -1 )
                    {
                        assert( ( false ) && "DynamicMesh3::CollapseEdge: failed at remove last check" );
                        return MeshResult::Failed_UnrecoverableError;
                    }
                    m_VertexRefCounts.Increment( b );
                    m_VertexRefCounts.Decrement( a );
                }
            }
        }
    }

    if ( !bIsBoundaryEdge )
    {
        // remove all edges from vtx a, then remove vtx a
        m_VertexEdgeLists.Clear( a );
        assert( m_VertexRefCounts.GetRefCount( a ) == 3 ); // in t0,t1, and initial ref
        m_VertexRefCounts.Decrement( a, 3 );
        assert( m_VertexRefCounts.IsValid( a ) == false );

        // remove triangles T0 and T1, and update b/c/d refcounts
        m_TriangleRefCounts.Decrement( t0 );
        m_TriangleRefCounts.Decrement( t1 );
        m_VertexRefCounts.Decrement( c );
        m_VertexRefCounts.Decrement( d );
        m_VertexRefCounts.Decrement( b, 2 );
        assert( m_TriangleRefCounts.IsValid( t0 ) == false );
        assert( m_TriangleRefCounts.IsValid( t1 ) == false );

        // remove edges ead, eab, eac
        m_EdgeRefCounts.Decrement( ead );
        m_EdgeRefCounts.Decrement( eab );
        m_EdgeRefCounts.Decrement( eac );
        assert( m_EdgeRefCounts.IsValid( ead ) == false );
        assert( m_EdgeRefCounts.IsValid( eab ) == false );
        assert( m_EdgeRefCounts.IsValid( eac ) == false );

        // replace t0 and t1 in edges ebd and ebc that we kept
        ebd = FindEdgeFromTri( b, d, t1 );
        if ( ebc == InvalidID ) // we may have already looked this up
        {
            ebc = FindEdgeFromTri( b, c, t0 );
        }

        if ( ReplaceEdgeTriangle( ebd, t1, tad ) == -1 )
        {
            assert( ( false ) &&
                    "DynamicMesh3::CollapseEdge: failed at isboundary=false branch, ebd replace triangle" );
            return MeshResult::Failed_UnrecoverableError;
        }

        if ( ReplaceEdgeTriangle( ebc, t0, tac ) == -1 )
        {
            assert( ( false ) &&
                    "DynamicMesh3::CollapseEdge: failed at isboundary=false branch, ebc replace triangle" );
            return MeshResult::Failed_UnrecoverableError;
        }

        // update tri-edge-nbrs in tad and tac
        if ( tad != InvalidID )
        {
            if ( ReplaceTriangleEdge( tad, ead, ebd ) == -1 )
            {
                assert( ( false ) &&
                        "DynamicMesh3::CollapseEdge: failed at isboundary=false branch, ebd replace triangle" );
                return MeshResult::Failed_UnrecoverableError;
            }
        }
        if ( tac != InvalidID )
        {
            if ( ReplaceTriangleEdge( tac, eac, ebc ) == -1 )
            {
                assert( ( false ) &&
                        "DynamicMesh3::CollapseEdge: failed at isboundary=false branch, ebd replace triangle" );
                return MeshResult::Failed_UnrecoverableError;
            }
        }

        // If both bd and ad were boundary edges, or both bc and ac, then the edge bd/bc will have
        //  no incident triangles and will need to be deleted. In that case, if vert c/d was not
        //  kept alive by some bowtie, that vertex will also need deleting.
        // This cannot happen if bAllowCollapsingInternalEdgeWithBoundaryVertices is false, and it
        //  cannot happen for a boundary ab edge because that would require a single triangle, which
        //  we currently disallow collapsing.
        if ( Options.bAllowCollapsingInternalEdgeWithBoundaryVertices )
        {
            if ( GetEdgeT( ebc ).A == InvalidID )
            {
                m_VertexEdgeLists.Remove( b, ebc );
                m_EdgeRefCounts.Decrement( ebc );
                if ( m_VertexRefCounts.GetRefCount( c ) == 1 )
                {
                    m_VertexEdgeLists.Clear( c );
                    m_VertexRefCounts.Decrement( c );
                }
                else
                {
                    // The vert must still be part of a bowtie. Still need to remove the deleted edge.
                    m_VertexEdgeLists.Remove( c, ebc );
                }
            }
            if ( GetEdgeT( ebd ).A == InvalidID )
            {
                m_VertexEdgeLists.Remove( b, ebd );
                m_VertexEdgeLists.Remove( d, ebd );
                m_EdgeRefCounts.Decrement( ebd );
                if ( m_VertexRefCounts.GetRefCount( d ) == 1 )
                {
                    m_VertexEdgeLists.Clear( d );
                    m_VertexRefCounts.Decrement( d );
                }
                else
                {
                    // The vert must still be part of a bowtie. Still need to remove the deleted edge.
                    m_VertexEdgeLists.Remove( d, ebd );
                }
            }
        }
    }
    else
    {
        //  boundary-edge path. this is basically same code as above, just not referencing t1/d

        // remove all edges from vtx a, then remove vtx a
        m_VertexEdgeLists.Clear( a );
        assert( m_VertexRefCounts.GetRefCount( a ) == 2 ); // in t0 and initial ref
        m_VertexRefCounts.Decrement( a, 2 );
        assert( m_VertexRefCounts.IsValid( a ) == false );

        // remove triangle T0 and update b/c refcounts
        m_TriangleRefCounts.Decrement( t0 );
        m_VertexRefCounts.Decrement( c );
        m_VertexRefCounts.Decrement( b );
        assert( m_TriangleRefCounts.IsValid( t0 ) == false );

        // remove edges eab and eac
        m_EdgeRefCounts.Decrement( eab );
        m_EdgeRefCounts.Decrement( eac );
        assert( m_EdgeRefCounts.IsValid( eab ) == false );
        assert( m_EdgeRefCounts.IsValid( eac ) == false );

        // replace t0 in edge ebc that we kept
        ebc = FindEdgeFromTri( b, c, t0 );
        if ( ReplaceEdgeTriangle( ebc, t0, tac ) == -1 )
        {
            assert( ( false ) &&
                    "DynamicMesh3::CollapseEdge: failed at isboundary=false branch, ebc replace triangle" );
            return MeshResult::Failed_UnrecoverableError;
        }

        // update tri-edge-nbrs in tac
        if ( tac != InvalidID )
        {
            if ( ReplaceTriangleEdge( tac, eac, ebc ) == -1 )
            {
                assert( ( false ) &&
                        "DynamicMesh3::CollapseEdge: failed at isboundary=true branch, ebd replace triangle" );
                return MeshResult::Failed_UnrecoverableError;
            }
        }
    }

    // set kept vertex to interpolated collapse position
    SetVertex( KeepVertID, Lerp( KeptPos, RemovedPos, EdgeParameterT ) );
    if ( HasVertexUVs() )
    {
        SetVertexUV( KeepVertID,
                     Lerp( GetVertexUV( KeepVertID ), RemovedUV, static_cast<float>( EdgeParameterT ) ) );
    }
    if ( HasVertexNormals() )
    {
        SetVertexNormal( KeepVertID, Normalized( Lerp( GetVertexNormal( KeepVertID ), RemovedNormal,
                                                       static_cast<float>( EdgeParameterT ) ) ) );
    }
    if ( HasVertexColors() )
    {
        SetVertexColor( KeepVertID,
                        Lerp( GetVertexColor( KeepVertID ), RemovedColor, static_cast<float>( EdgeParameterT ) ) );
    }

    CollapseInfo.KeptEdges = Index2i( ebc, ebd );

    if ( HasAttributes() )
    {
        Attributes()->OnCollapseEdge( CollapseInfo );
    }

    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

MeshResult DynamicMesh3::MergeEdges( int KeepEdgeID, int DiscardEdgeID, MergeEdgesInfo& MergeInfo,
                                     bool bCheckValidOrientation )
{
    return MergeEdges( KeepEdgeID, DiscardEdgeID, 0, MergeInfo, bCheckValidOrientation );
}

MeshResult DynamicMesh3::MergeEdges( int KeepEdgeID, int DiscardEdgeID, double InterpolationT,
                                     MergeEdgesInfo& MergeInfo, bool bCheckValidOrientation )
{
    MergeInfo                = MergeEdgesInfo();
    MergeInfo.InterpolationT = InterpolationT;

    if ( !IsEdge( KeepEdgeID ) || !IsEdge( DiscardEdgeID ) )
    {
        return MeshResult::Failed_NotAnEdge;
    }

    const Edge edgeinfo_keep    = GetEdge( KeepEdgeID );
    const Edge edgeinfo_discard = GetEdge( DiscardEdgeID );
    if ( edgeinfo_keep.Tri[1] != InvalidID || edgeinfo_discard.Tri[1] != InvalidID )
    {
        return MeshResult::Failed_NotABoundaryEdge;
    }

    int       a   = edgeinfo_keep.Vert[0];
    int       b   = edgeinfo_keep.Vert[1];
    int const tab = edgeinfo_keep.Tri[0];
    int const eab = KeepEdgeID;
    int       c   = edgeinfo_discard.Vert[0];
    int       d   = edgeinfo_discard.Vert[1];
    int const tcd = edgeinfo_discard.Tri[0];
    int const ecd = DiscardEdgeID;

    // Need to correctly orient a,b and c,d and then check that
    // we will not join triangles with incompatible winding order
    // I can't see how to do this purely topologically.
    // So relying on closest-pairs testing.
    int const OppAB = IndexUtil::OrientTriEdgeAndFindOtherVtx( a, b, GetTriangle( tab ) );
    int const OppCD = IndexUtil::OrientTriEdgeAndFindOtherVtx( c, d, GetTriangle( tcd ) );

    // Refuse to merge if doing so would create a duplicate triangle
    if ( OppAB == OppCD )
    {
        return MeshResult::Failed_InvalidNeighbourhood;
    }

    int const x = c;
    c           = d;
    d           = x; // joinable bdry edges have opposing orientations, so flip to get ac and b/d correspondences
    glm::dvec3 const Va = GetVertex( a );
    glm::dvec3 const Vb = GetVertex( b );
    glm::dvec3 const Vc = GetVertex( c );
    glm::dvec3 const Vd = GetVertex( d );
    if ( bCheckValidOrientation && ( glm::length2( ( Va - Vc ) ) + glm::length2( ( Vb - Vd ) ) ) >
                                        ( glm::length2( ( Va - Vd ) ) + glm::length2( ( Vb - Vc ) ) ) )
    {
        return MeshResult::Failed_SameOrientation;
    }

    // alternative that detects normal flip of triangle tcd. This is a more
    // robust geometric test, but fails if tri is degenerate...also more expensive
    // FVector3d otherv = GetVertex(tcd_otherv);
    // FVector3d Ncd = VectorUtil::NormalDirection(GetVertex(c), GetVertex(d), otherv);
    // FVector3d Nab = VectorUtil::NormalDirection(GetVertex(a), GetVertex(b), otherv);
    // if (Ncd.Dot(Nab) < 0)
    // return MeshResult::Failed_SameOrientation;

    MergeInfo.KeptEdge    = eab;
    MergeInfo.RemovedEdge = ecd;

    // if a/c or b/d are connected by an existing edge, we can't merge
    if ( a != c && FindEdge( a, c ) != InvalidID )
    {
        return MeshResult::Failed_InvalidNeighbourhood;
    }
    if ( b != d && FindEdge( b, d ) != InvalidID )
    {
        return MeshResult::Failed_InvalidNeighbourhood;
    }
    // the un-matched edge vertices, a/d and b/c, should also not be directly connected
    // (unless the edges share a vertex, in which case they are always connected by one of the two merge edges)
    if ( a != c && b != d && FindEdge( a, d ) != InvalidID )
    {
        return MeshResult::Failed_InvalidNeighbourhood;
    }
    if ( a != c && b != d && FindEdge( b, c ) != InvalidID )
    {
        return MeshResult::Failed_InvalidNeighbourhood;
    }

    // if vertices at either end already share a common neighbour vertex, and we
    // do the merge, that would create duplicate edges. This is something like the
    // 'link condition' in edge collapses.
    // Note that we have to catch cases where both edges to the shared vertex are
    // boundary edges, in that case we will also merge this edge later on
    int MaxAdjBoundaryMerges[2]{ 0, 0 };
    if ( a != c )
    {
        int const other_v = ( b == d ) ? b : -1;
        for ( int const cnbr : VtxVerticesItr( c ) )
        {
            if ( cnbr == other_v )
            {
                continue;
            }
            int const ea = FindEdge( a, cnbr );
            if ( ea != InvalidID )
            {
                int const ec = FindEdge( c, cnbr );
                if ( !IsBoundaryEdge( ea ) || !IsBoundaryEdge( ec ) )
                {
                    return MeshResult::Failed_InvalidNeighbourhood;
                }

                MaxAdjBoundaryMerges[0]++;
            }
        }
    }
    if ( b != d )
    {
        int const other_v = ( a == c ) ? a : -1;
        for ( int const dnbr : VtxVerticesItr( d ) )
        {
            if ( dnbr == other_v )
            {
                continue;
            }
            int const eb = FindEdge( b, dnbr );
            if ( eb != InvalidID )
            {
                int const ed = FindEdge( d, dnbr );
                if ( !IsBoundaryEdge( eb ) || !IsBoundaryEdge( ed ) )
                {
                    return MeshResult::Failed_InvalidNeighbourhood;
                }

                MaxAdjBoundaryMerges[1]++;
            }
        }
    }

    auto ApplyInterpolation = [this, InterpolationT]( int32_t KeepVid, int32_t RemoveVid )
    {
        SetVertex( KeepVid, Lerp( GetVertex( KeepVid ), GetVertex( RemoveVid ), InterpolationT ) );
        if ( HasVertexUVs() )
        {
            SetVertexUV( KeepVid, Lerp( GetVertexUV( KeepVid ), GetVertexUV( RemoveVid ),
                                        static_cast<float>( InterpolationT ) ) );
        }
        if ( HasVertexNormals() )
        {
            SetVertexNormal( KeepVid, Normalized( Lerp( GetVertexNormal( KeepVid ), GetVertexNormal( RemoveVid ),
                                                        static_cast<float>( InterpolationT ) ) ) );
        }
        if ( HasVertexColors() )
        {
            SetVertexColor( KeepVid, Lerp( GetVertexColor( KeepVid ), GetVertexColor( RemoveVid ),
                                           static_cast<float>( InterpolationT ) ) );
        }
    };

    // [TODO] this acts on each interior tri twice. could avoid using vtx-tri iterator?
    if ( a != c )
    {
        if ( InterpolationT != 0 )
        {
            // Do the interpolation before we remove c
            ApplyInterpolation( a, c );
        }

        // replace c w/ a in edges and tris connected to c, and move edges to a
        for ( int const eid : m_VertexEdgeLists.Values( c ) )
        {
            if ( eid == DiscardEdgeID )
            {
                continue;
            }
            ReplaceEdgeVertex( eid, c, a );
            short      rc   = 0;
            const Edge Edge = m_Edges[eid];
            if ( ReplaceTriangleVertex( Edge.Tri[0], c, a ) >= 0 )
            {
                rc++;
            }
            if ( Edge.Tri[1] != InvalidID )
            {
                if ( ReplaceTriangleVertex( Edge.Tri[1], c, a ) >= 0 )
                {
                    rc++;
                }
            }
            m_VertexEdgeLists.Insert( a, eid );
            if ( rc > 0 )
            {
                m_VertexRefCounts.Increment( a, rc );
                m_VertexRefCounts.Decrement( c, rc );
            }
        }
        m_VertexEdgeLists.Clear( c );
        m_VertexRefCounts.Decrement( c );
        MergeInfo.RemovedVerts[0] = c;
    }
    else
    {
        m_VertexEdgeLists.Remove( a, ecd );
        MergeInfo.RemovedVerts[0] = InvalidID;
    }
    MergeInfo.KeptVerts[0] = a;

    if ( d != b )
    {
        if ( InterpolationT != 0 )
        {
            // Do the interpolation before we remove d
            ApplyInterpolation( b, d );
        }

        // replace d w/ b in edges and tris connected to d, and move edges to b
        for ( int const eid : m_VertexEdgeLists.Values( d ) )
        {
            if ( eid == DiscardEdgeID )
            {
                continue;
            }
            ReplaceEdgeVertex( eid, d, b );
            short      rc   = 0;
            const Edge Edge = m_Edges[eid];
            if ( ReplaceTriangleVertex( Edge.Tri[0], d, b ) >= 0 )
            {
                rc++;
            }
            if ( Edge.Tri[1] != InvalidID )
            {
                if ( ReplaceTriangleVertex( Edge.Tri[1], d, b ) >= 0 )
                {
                    rc++;
                }
            }
            m_VertexEdgeLists.Insert( b, eid );
            if ( rc > 0 )
            {
                m_VertexRefCounts.Increment( b, rc );
                m_VertexRefCounts.Decrement( d, rc );
            }
        }
        m_VertexEdgeLists.Clear( d );
        m_VertexRefCounts.Decrement( d );
        MergeInfo.RemovedVerts[1] = d;
    }
    else
    {
        m_VertexEdgeLists.Remove( b, ecd );
        MergeInfo.RemovedVerts[1] = InvalidID;
    }
    MergeInfo.KeptVerts[1] = b;

    // replace edge cd with edge ab in triangle tcd
    ReplaceTriangleEdge( tcd, ecd, eab );
    m_EdgeRefCounts.Decrement( ecd );

    // update edge-tri adjacency
    SetEdgeTrianglesInternal( eab, tab, tcd );

    // Once we merge ab to cd, there may be additional edges (now) connected
    // to either a or b that are connected to the same vertex on their 'other' side.
    // So we now have two boundary edges connecting the same two vertices - disaster!
    // We need to find and merge these edges.
    MergeInfo.ExtraRemovedEdges = Index2i( InvalidID, InvalidID );
    MergeInfo.ExtraKeptEdges    = Index2i( InvalidID, InvalidID );
    for ( int vi = 0; vi < 2; ++vi )
    {
        int v1 = a;
        int v2 = c; // vertices of merged edge
        if ( vi == 1 )
        {
            v1 = b;
            v2 = d;
        }
        if ( v1 == v2 )
        {
            continue;
        }

        DynamicMesh3::LocalIntArray edges_v;
        GetVertexEdgesList( v1, edges_v );
        int const Nedges   = static_cast<int32_t>( static_cast<int>( edges_v.size() ) );
        int       FoundNum = 0;
        // in this loop, we compare 'other' vert_1 and vert_2 of edges around v1.
        // problem case is when vert_1 == vert_2  (ie two edges w/ same other vtx).
        for ( int i = 0; i < Nedges && FoundNum < MaxAdjBoundaryMerges[vi]; ++i )
        {
            int const edge_1 = edges_v[i];
            // Skip any non-boundary edge, or edge we've already removed via merging
            if ( !m_EdgeRefCounts.IsValidUnsafe( edge_1 ) || !IsBoundaryEdge( edge_1 ) )
            {
                continue;
            }
            int const vert_1 = GetOtherEdgeVertex( edge_1, v1 );
            for ( int j = i + 1; j < Nedges; ++j )
            {
                int const edge_2 = edges_v[j];
                // Skip any non-boundary edge, or edge we've already removed via merging
                if ( !m_EdgeRefCounts.IsValidUnsafe( edge_2 ) || !IsBoundaryEdge( edge_2 ) )
                {
                    continue;
                }
                int const vert_2 = GetOtherEdgeVertex( edge_2, v1 );
                if ( vert_1 == vert_2 )
                {
                    // replace edge_2 w/ edge_1 in tri, update edge and vtx-edge-nbr lists
                    int const tri_1 = m_Edges[edge_1].Tri[0];
                    int const tri_2 = m_Edges[edge_2].Tri[0];
                    ReplaceTriangleEdge( tri_2, edge_2, edge_1 );
                    SetEdgeTrianglesInternal( edge_1, tri_1, tri_2 );
                    m_VertexEdgeLists.Remove( v1, edge_2 );
                    m_VertexEdgeLists.Remove( vert_1, edge_2 );
                    m_EdgeRefCounts.Decrement( edge_2 );
                    if ( FoundNum == 0 )
                    {
                        MergeInfo.ExtraRemovedEdges[vi] = edge_2;
                        MergeInfo.ExtraKeptEdges[vi]    = edge_1;
                    }
                    else
                    {
                        MergeInfo.BowtiesRemovedEdges.push_back( edge_2 );
                        MergeInfo.BowtiesKeptEdges.push_back( edge_1 );
                    }

                    FoundNum++; // exit outer i loop if we've found all possible merge edges
                    break;      // exit inner j loop; we won't merge anything else to edge_1
                }
            }
        }
    }

    if ( HasAttributes() )
    {
        Attributes()->OnMergeEdges( MergeInfo );
    }

    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

MeshResult DynamicMesh3::MergeVertices( int KeepVid, int DiscardVid, double InterpolationT,
                                        const MergeVerticesOptions& Options, MergeVerticesInfo& MergeInfo )
{
    MergeInfo                = MergeVerticesInfo();
    MergeInfo.InterpolationT = InterpolationT;
    MergeInfo.KeptVertex     = KeepVid;
    MergeInfo.RemovedVertex  = DiscardVid;

    if ( !IsVertex( KeepVid ) || !IsVertex( DiscardVid ) )
    {
        return MeshResult::Failed_NotAVertex;
    }
    if ( KeepVid == DiscardVid )
    {
        return MeshResult::Failed_VertexAlreadyExists;
    }

    // See if we can resolve this as an edge collapse.
    // Note that this should be checked before trying to resolve as an edge weld because edge welding
    //  will currently fail if there is an edge connecting the removed and kept vert (i.e. welding two
    //  sides of a single triangle hole will fail, but collapse of the intervening edge will succeed
    //  with our permissiveness options).
    if ( FindEdge( KeepVid, DiscardVid ) != InvalidID )
    {
        CollapseEdgeOptions CollapseOptions;
        CollapseOptions.bAllowCollapsingInternalEdgeWithBoundaryVertices = true;
        CollapseOptions.bAllowHoleCollapse                               = true;
        CollapseOptions.bAllowTetrahedronCollapse                        = true;
        MergeInfo.EdgeCollapseInfo.emplace();
        return CollapseEdge( KeepVid, DiscardVid, InterpolationT, CollapseOptions,
                             MergeInfo.EdgeCollapseInfo.value() );
    }

    // See if we can resolve this as an edge weld
    for ( int32_t const KeepAdjacentEid : m_VertexEdgeLists.Values( KeepVid ) )
    {
        int32_t const KeepAdjacentVid = GetOtherEdgeVertex( KeepAdjacentEid, KeepVid );

        // See if the adjacent vert is also adjacent to DiscardVid
        for ( int32_t const DiscardAdjacentEid : m_VertexEdgeLists.Values( DiscardVid ) )
        {
            if ( GetOtherEdgeVertex( DiscardAdjacentEid, DiscardVid ) == KeepAdjacentVid )
            {
                // We've found a V shape (a vertex adjacent to both of the vertices we're working with).
                //  Neither of the edges in the V shape can be a non boundary edge, else the merge would
                //  create a non-manifold edge- we do this check ourselves so that we return
                //  Failed_InvalidNeighbourhood instead of Failed_NotABoundaryEdge.
                if ( !IsBoundaryEdge( KeepAdjacentEid ) || !IsBoundaryEdge( DiscardAdjacentEid ) )
                {
                    return MeshResult::Failed_InvalidNeighbourhood;
                }
                // The MergeEdges operation will do the other neighbor checks for us

                MergeInfo.MergeEdgesInfo.emplace();
                return MergeEdges( KeepAdjacentEid, DiscardAdjacentEid, InterpolationT,
                                   MergeInfo.MergeEdgesInfo.value(), false );
            }
        } // end for each adjacent vid to KeepVid
    } // end V shape search

    // If we got to here, the vertices are at least three edges apart, and we're creating a bowtie.

    if ( !Options.bAllowNonBoundaryBowtieCreation &&
         ( !IsBoundaryVertex( KeepVid ) || !IsBoundaryVertex( DiscardVid ) ) )
    {
        return MeshResult::Failed_WouldCreateBowtie;
    }

    // Apply interpolation first, before removing DiscardVid
    if ( InterpolationT != 0 )
    {
        SetVertex( KeepVid, Lerp( GetVertex( KeepVid ), GetVertex( DiscardVid ), InterpolationT ) );
        if ( HasVertexUVs() )
        {
            SetVertexUV( KeepVid, Lerp( GetVertexUV( KeepVid ), GetVertexUV( DiscardVid ),
                                        static_cast<float>( InterpolationT ) ) );
        }
        if ( HasVertexNormals() )
        {
            SetVertexNormal( KeepVid, Normalized( Lerp( GetVertexNormal( KeepVid ), GetVertexNormal( DiscardVid ),
                                                        static_cast<float>( InterpolationT ) ) ) );
        }
        if ( HasVertexColors() )
        {
            SetVertexColor( KeepVid, Lerp( GetVertexColor( KeepVid ), GetVertexColor( DiscardVid ),
                                           static_cast<float>( InterpolationT ) ) );
        }
    }

    // Replace DiscardVid w/ KeepVid in edges and tris connected to DiscardVid, and move edges to KeepVid
    for ( int const Eid : m_VertexEdgeLists.Values( DiscardVid ) )
    {
        ReplaceEdgeVertex( Eid, DiscardVid, KeepVid );
        short      ReplaceCount = 0;
        const Edge Edge         = m_Edges[Eid];
        if ( ReplaceTriangleVertex( Edge.Tri[0], DiscardVid, KeepVid ) >= 0 )
        {
            ReplaceCount++;
        }
        if ( Edge.Tri[1] != InvalidID )
        {
            if ( ReplaceTriangleVertex( Edge.Tri[1], DiscardVid, KeepVid ) >= 0 )
            {
                ReplaceCount++;
            }
        }
        m_VertexEdgeLists.Insert( KeepVid, Eid );
        if ( ReplaceCount > 0 )
        {
            m_VertexRefCounts.Increment( KeepVid, ReplaceCount );
            m_VertexRefCounts.Decrement( DiscardVid, ReplaceCount );
        }
    }
    m_VertexEdgeLists.Clear( DiscardVid );
    m_VertexRefCounts.Decrement( DiscardVid );

    if ( HasAttributes() )
    {
        Attributes()->OnMergeVertices( MergeInfo );
    }

    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}

MeshResult DynamicMesh3::PokeTriangle( int TriangleID, const glm::dvec3& BaryCoordinates,
                                       PokeTriangleInfo& PokeInfo )
{
    PokeInfo = PokeTriangleInfo();

    if ( !IsTriangle( TriangleID ) )
    {
        return MeshResult::Failed_NotATriangle;
    }

    Index3i tv = GetTriangle( TriangleID );
    Index3i te = GetTriEdges( TriangleID );

    // create vertex with interpolated vertex attribs
    VertexInfo vinfo;
    GetTriBaryPoint( TriangleID, BaryCoordinates[0], BaryCoordinates[1], BaryCoordinates[2], vinfo );
    int const center = AppendVertex( vinfo );

    // add in edges to center vtx, do not connect to triangles yet
    int const eaC = AddEdgeInternal( tv[0], center, -1, -1 );
    int const ebC = AddEdgeInternal( tv[1], center, -1, -1 );
    int const ecC = AddEdgeInternal( tv[2], center, -1, -1 );
    m_VertexRefCounts.Increment( tv[0] );
    m_VertexRefCounts.Increment( tv[1] );
    m_VertexRefCounts.Increment( tv[2] );
    m_VertexRefCounts.Increment( center, 3 );

    // old triangle becomes tri along first edge
    SetTriangleInternal( TriangleID, tv[0], tv[1], center );
    SetTriangleEdgesInternal( TriangleID, te[0], ebC, eaC );

    // add two triangles
    int const t1 = AddTriangleInternal( tv[1], tv[2], center, te[1], ecC, ebC );
    int const t2 = AddTriangleInternal( tv[2], tv[0], center, te[2], eaC, ecC );

    // second and third edges of original tri have neighbours
    ReplaceEdgeTriangle( te[1], TriangleID, t1 );
    ReplaceEdgeTriangle( te[2], TriangleID, t2 );

    // set the triangles for the edges we created above
    SetEdgeTrianglesInternal( eaC, TriangleID, t2 );
    SetEdgeTrianglesInternal( ebC, TriangleID, t1 );
    SetEdgeTrianglesInternal( ecC, t1, t2 );

    // transfer groups
    if ( m_TriangleGroups.has_value() )
    {
        int const g = m_TriangleGroups.value()[TriangleID];
        m_TriangleGroups->InsertAt( g, t1 );
        m_TriangleGroups->InsertAt( g, t2 );
    }

    PokeInfo.OriginalTriangle = TriangleID;
    PokeInfo.TriVertices      = tv;
    PokeInfo.NewVertex        = center;
    PokeInfo.NewTriangles     = Index2i( t1, t2 );
    PokeInfo.NewEdges         = Index3i( eaC, ebC, ecC );
    PokeInfo.BaryCoords       = BaryCoordinates;

    if ( HasAttributes() )
    {
        Attributes()->OnPokeTriangle( PokeInfo );
    }

    UpdateChangeStamps( true, true );
    return MeshResult::Ok;
}
