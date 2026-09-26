// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/DynamicMesh3_Queries.cpp:1-1012,
// adapted: UE Core via UECore.hpp; LocalIntArray is TArray<int32_t> so its explicit instantiations collapse into
// the TArray ones; GetVertexFrame/GetTriFrame (Frame3d) not ported; bounds are computed serially.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"
#include <Common/Core/Core.hpp>

using namespace Desert::Geometry;

Index2i DynamicMesh3::GetEdgeOpposingV( int eID ) const
{
    const Edge&  Edge = m_Edges[eID];
    int const    a    = Edge.Vert[0];
    int const    b    = Edge.Vert[1];

    // ** it is important that verts returned maintain [c,d] order!!
    int const c = IndexUtil::FindTriOtherVtxUnsafe( a, b, m_Triangles[Edge.Tri[0]] );
    if ( Edge.Tri[1] != InvalidID )
    {
        int const d = IndexUtil::FindTriOtherVtxUnsafe( a, b, m_Triangles[Edge.Tri[1]] );
        return { c, d };
    }

    return { c, InvalidID };
}

int DynamicMesh3::GetVtxBoundaryEdges( int vID, int& e0, int& e1 ) const
{
    if ( m_VertexRefCounts.IsValid( vID ) )
    {
        int count = 0;
        for ( int const eid : m_VertexEdgeLists.Values( vID ) )
        {
            if ( m_Edges[eid].Tri[1] == InvalidID )
            {
                if ( count == 0 )
                {
                    e0 = eid;
                }
                else if ( count == 1 )
                {
                    e1 = eid;
                }
                count++;
            }
        }
        return count;
    }
    return 0;
}

template <typename ArrayType>
int DynamicMesh3::GetAllVtxBoundaryEdges( int vID, ArrayType& EdgeListOut ) const
{
    if ( m_VertexRefCounts.IsValid( vID ) )
    {
        int count = 0;
        for ( int const eid : m_VertexEdgeLists.Values( vID ) )
        {
            if ( m_Edges[eid].Tri[1] == InvalidID )
            {
                EdgeListOut.push_back( eid );
                count++;
            }
        }
        return count;
    }
    return 0;
}

template int DynamicMesh3::GetAllVtxBoundaryEdges<std::vector<int32_t>>( int                   vID,
                                                                         std::vector<int32_t>& EdgeListOut ) const;

void DynamicMesh3::GetVtxNbrhood( int eID, int vID, int& vOther, int& oppV1, int& oppV2, int& t1, int& t2 ) const
{
    const Edge Edge  = m_Edges[eID];
    vOther           = ( Edge.Vert[0] == vID ) ? Edge.Vert[1] : Edge.Vert[0];
    t1               = Edge.Tri[0];
    oppV1            = IndexUtil::FindTriOtherVtx( vID, vOther, m_Triangles, t1 );
    t2               = Edge.Tri[1];
    if ( t2 != InvalidID )
    {
        oppV2 = IndexUtil::FindTriOtherVtx( vID, vOther, m_Triangles, t2 );
    }
    else
    {
        t2 = InvalidID;
    }
}

int DynamicMesh3::GetVtxTriangleCount( int vID ) const
{
    if ( !IsVertex( vID ) )
    {
        return -1;
    }
    int N = 0;
    m_VertexEdgeLists.Enumerate( vID,
                                 [&]( int32_t eid )
                                 {
                                     const Edge Edge   = m_Edges[eid];
                                     const int  vOther = Edge.Vert.A == vID ? Edge.Vert.B : Edge.Vert.A;
                                     if ( TriHasSequentialVertices( Edge.Tri[0], vID, vOther ) )
                                     {
                                         N++;
                                     }
                                     if ( Edge.Tri[1] != InvalidID &&
                                          TriHasSequentialVertices( Edge.Tri[1], vID, vOther ) )
                                     {
                                         N++;
                                     }
                                 } );
    return N;
}

int DynamicMesh3::GetVtxSingleTriangle( int VertexID ) const
{
    if ( !IsVertex( VertexID ) )
    {
        return DynamicMesh3::InvalidID;
    }
    for ( int const EID : m_VertexEdgeLists.Values( VertexID ) )
    {
        return m_Edges[EID].Tri[0];
    }

    return DynamicMesh3::InvalidID;
}

template <typename ArrayType>
MeshResult DynamicMesh3::GetVtxTriangles( int vID, ArrayType& TrianglesOut ) const
{
    if ( !IsVertex( vID ) )
    {
        return MeshResult::Failed_NotAVertex;
    }

    if ( m_VertexEdgeLists.GetCount( vID ) > 20 )
    {
        m_VertexEdgeLists.Enumerate( vID,
                                     [&]( int32_t eid )
                                     {
                                         const Edge Edge   = m_Edges[eid];
                                         const int  vOther = Edge.Vert.A == vID ? Edge.Vert.B : Edge.Vert.A;
                                         if ( TriHasSequentialVertices( Edge.Tri[0], vID, vOther ) )
                                         {
                                             TrianglesOut.push_back( Edge.Tri[0] );
                                         }
                                         if ( Edge.Tri[1] != InvalidID &&
                                              TriHasSequentialVertices( Edge.Tri[1], vID, vOther ) )
                                         {
                                             TrianglesOut.push_back( Edge.Tri[1] );
                                         }
                                     } );
    }
    else
    {
        m_VertexEdgeLists.Enumerate(
             vID,
             [&]( int32_t eid )
             {
                 const Edge Edge = m_Edges[eid];
                 if ( std::find( TrianglesOut.begin(), TrianglesOut.end(), Edge.Tri[0] ) == TrianglesOut.end() )
                 {
                     TrianglesOut.push_back( Edge.Tri[0] );
                 }
                 if ( Edge.Tri[1] != InvalidID )
                 {
                     if ( std::find( TrianglesOut.begin(), TrianglesOut.end(), Edge.Tri[1] ) ==
                          TrianglesOut.end() )
                     {
                         TrianglesOut.push_back( Edge.Tri[1] );
                     }
                 }
             } );
    }
    return MeshResult::Ok;
}

template MeshResult
DynamicMesh3::GetVtxTriangles<std::vector<int32_t>>( int vID, std::vector<int32_t>& TrianglesOut ) const;

template <typename IntArray, typename BoolArray>
MeshResult DynamicMesh3::GetVtxContiguousTriangles( int VertexID, IntArray& TrianglesOut, IntArray& SpanLengths,
                                                    BoolArray& IsLoop ) const
{
    TrianglesOut.clear();
    SpanLengths.clear();
    IsLoop.clear();

    if ( !Common::EnsureOrWarn( IsVertex( VertexID ), "IsVertex( VertexID )" ) )
    {
        return MeshResult::Failed_NotAVertex;
    }

    int const NumEdges = m_VertexEdgeLists.GetCount( VertexID );
    if ( NumEdges == 0 )
    {
        return MeshResult::Ok;
    }

    // The number of StartEdgeIDs for a vertex is NumBoundaryEdges + NumRingsWithoutBoundary
    // This is only higher than 2 at non-manifold (e.g., "bowtie") vertices, and
    // it should be very rare for it to exceed this inline allocation of 8
    LocalIntArray StartEdgeIDs;
    // initial starting edge candidates == boundary edges
    for ( int const EID : m_VertexEdgeLists.Values( VertexID ) )
    {
        if ( m_Edges[EID].Tri[1] == InvalidID )
        {
            StartEdgeIDs.push_back( EID );
        }
    }
    bool const bHasBoundaries = !StartEdgeIDs.empty();

    if ( !bHasBoundaries )
    {
        for ( int const EID : m_VertexEdgeLists.Values( VertexID ) )
        {
            StartEdgeIDs.push_back( EID );
            break;
        }
    }

    int  WalkedEdges             = 0;
    bool bHasRemainingBoundaries = bHasBoundaries;
    while ( static_cast<int32_t>( StartEdgeIDs.size() ) || WalkedEdges < NumEdges )
    {
        if ( !static_cast<int32_t>( StartEdgeIDs.size() ) )
        {
            bHasRemainingBoundaries = false;
            // fallback for (hopefully very rare) case of a non-manifold vertex where there are separate one-rings
            // w/ no boundary edges --
            //  brute force search for an edge that hasn't already been walked
            for ( int const EID : m_VertexEdgeLists.Values( VertexID ) )
            {
                int const  AttachedTriID = m_Edges[EID].Tri[0];
                bool const UsedEdge = ( std::find( TrianglesOut.begin(), TrianglesOut.end(), AttachedTriID ) !=
                                        TrianglesOut.end() );
                if ( !UsedEdge )
                {
                    StartEdgeIDs.push_back( EID );
                    break;
                }
            }
        }

        // walk starting from this edge, add the found span

        int const StartEID = StartEdgeIDs.back();
        StartEdgeIDs.pop_back();
        int PrevEID  = StartEID;
        WalkedEdges++;
        int           WalkTri   = m_Edges[StartEID].Tri[0];
        auto const    SpanStart = static_cast<int32_t>( TrianglesOut.size() );
        IsLoop.push_back( !bHasRemainingBoundaries );
        while ( true )
        {
            TrianglesOut.push_back( WalkTri );

            int const       TriIdx     = WalkTri;
            const Index3i&  TriVIDs    = m_Triangles[TriIdx];
            const Index3i&  TriEIDs    = m_TriangleEdges[TriIdx];
            int const       VertSubIdx = IndexUtil::FindTriIndex( VertexID, TriVIDs );
            int             NextEID    = TriEIDs[VertSubIdx];
            if ( NextEID == PrevEID )
            {
                NextEID = TriEIDs[( VertSubIdx + 2 ) % 3];
            }
            if ( NextEID == StartEID )
            {
                break;
            }

            WalkedEdges++;

            int NextTriID = m_Edges[NextEID].Tri[0];
            if ( NextTriID == WalkTri )
            {
                NextTriID = m_Edges[NextEID].Tri[1];
            }
            if ( NextTriID == InvalidID )
            {
                // remove the corresponding boundary
                assert( !StartEdgeIDs.empty() );
                if ( !StartEdgeIDs.empty() )
                {
                    // UE RemoveSingleSwap: the first equal element takes the last one's place.
                    const auto Found = std::find( StartEdgeIDs.begin(), StartEdgeIDs.end(), NextEID );
                    if ( Found != StartEdgeIDs.end() )
                    {
                        *Found = StartEdgeIDs.back();
                        StartEdgeIDs.pop_back();
                    }
                }
                break;
            }
            WalkTri = NextTriID;
            PrevEID = NextEID;
        }
        SpanLengths.push_back( static_cast<int32_t>( TrianglesOut.size() ) - SpanStart );
    }

    return Common::EnsureOrWarn( SpanLengths.size() == IsLoop.size(), "SpanLengths.size() == IsLoop.size()" )
                ? MeshResult::Ok
                : MeshResult::Failed_InvalidNeighbourhood;
}

template MeshResult DynamicMesh3::GetVtxContiguousTriangles<std::vector<int32_t>, std::vector<bool>>(
     int VertexID, std::vector<int32_t>& TrianglesOut, std::vector<int32_t>& SpanLengths,
     std::vector<bool>& IsLoop ) const;

bool DynamicMesh3::IsBoundaryVertex( int vID ) const
{
    assert( IsVertex( vID ) );
    if ( IsVertex( vID ) )
    {
        for ( int const eid : m_VertexEdgeLists.Values( vID ) )
        {
            if ( m_Edges[eid].Tri[1] == InvalidID )
            {
                return true;
            }
        }
    }
    return false;
}

bool DynamicMesh3::IsBoundaryTriangle( int tID ) const
{
    assert( IsTriangle( tID ) );
    if ( IsTriangle( tID ) )
    {
        const Index3i& TriEdgeIDs = m_TriangleEdges[tID];
        return IsBoundaryEdge( TriEdgeIDs[0] ) || IsBoundaryEdge( TriEdgeIDs[1] ) ||
               IsBoundaryEdge( TriEdgeIDs[2] );
    }

    return false;
}

Index2i DynamicMesh3::GetOrientedBoundaryEdgeV( int eID ) const
{
    if ( m_EdgeRefCounts.IsValid( eID ) )
    {
        const Edge Edge = m_Edges[eID];
        if ( Edge.Tri[1] == InvalidID )
        {
            int const       a   = Edge.Vert[0];
            int const       b   = Edge.Vert[1];
            int const       ti  = Edge.Tri[0];
            const Index3i&  tri = m_Triangles[ti];
            int const       ai  = IndexUtil::FindEdgeIndexInTri( a, b, tri );
            return { tri[ai], tri[( ai + 1 ) % 3] };
        }
    }
    assert( false );
    return InvalidEdge;
}

bool DynamicMesh3::IsGroupBoundaryEdge( int eID ) const
{
    if ( !HasTriangleGroups() )
        return false;

    const Edge  Edge = m_Edges[eID];
    int const   et1  = Edge.Tri[1];
    if ( et1 == InvalidID )
    {
        return false;
    }
    int const g1  = m_TriangleGroups.value()[et1];
    int const et0 = Edge.Tri[0];
    int const g0  = m_TriangleGroups.value()[et0];
    return g1 != g0;
}

bool DynamicMesh3::IsGroupBoundaryVertex( int vID ) const
{
    if ( !HasTriangleGroups() )
        return false;

    int group_id = InvalidID;
    for ( int const eID : m_VertexEdgeLists.Values( vID ) )
    {
        const Edge  Edge = m_Edges[eID];
        int const   et0  = Edge.Tri[0];
        int const   g0   = m_TriangleGroups.value()[et0];
        if ( group_id != g0 )
        {
            if ( group_id == InvalidID )
            {
                group_id = g0;
            }
            else
            {
                return true; // saw multiple group IDs
            }
        }
        int const et1 = Edge.Tri[1];
        if ( et1 != InvalidID )
        {
            int const g1 = m_TriangleGroups.value()[et1];
            if ( group_id != g1 )
            {
                return true; // saw multiple group IDs
            }
        }
    }
    return false;
}

bool DynamicMesh3::IsGroupJunctionVertex( int vID ) const
{
    if ( !HasTriangleGroups() )
        return false;

    Index2i groups( InvalidID, InvalidID );
    for ( int const eID : m_VertexEdgeLists.Values( vID ) )
    {
        const Edge Edge = m_Edges[eID];
        Index2i    et   = Edge.Tri;
        for ( int k = 0; k < 2; ++k )
        {
            if ( et[k] == InvalidID )
            {
                continue;
            }
            int const g0 = m_TriangleGroups.value()[et[k]];
            if ( g0 != groups[0] && g0 != groups[1] )
            {
                if ( groups[0] != InvalidID && groups[1] != InvalidID )
                {
                    return true;
                }
                if ( groups[0] == InvalidID )
                {
                    groups[0] = g0;
                }
                else
                {
                    groups[1] = g0;
                }
            }
        }
    }
    return false;
}

bool DynamicMesh3::GetVertexGroups( int vID, Index4i& groups ) const
{
    groups = Index4i( InvalidID, InvalidID, InvalidID, InvalidID );
    if ( !HasTriangleGroups() )
        return false;
    int ng = 0;

    for ( int const eID : m_VertexEdgeLists.Values( vID ) )
    {
        const Edge Edge = m_Edges[eID];

        int const et0 = Edge.Tri[0];
        int const g0  = m_TriangleGroups.value()[et0];
        if ( !groups.Contains( g0 ) )
        {
            groups[ng++] = g0;
        }
        if ( ng == 4 )
        {
            return false;
        }
        int const et1 = Edge.Tri[1];
        if ( et1 != InvalidID )
        {
            int const g1 = m_TriangleGroups.value()[et1];
            if ( !groups.Contains( g1 ) )
            {
                groups[ng++] = g1;
            }
            if ( ng == 4 )
            {
                return false;
            }
        }
    }
    return true;
}

template <typename ArrayType>
bool DynamicMesh3::GetAllVertexGroups( int vID, ArrayType& GroupsOut ) const
{
    if ( !HasTriangleGroups() )
        return false;

    for ( int const eID : m_VertexEdgeLists.Values( vID ) )
    {
        const Edge  Edge = m_Edges[eID];
        int const   et0  = Edge.Tri[0];
        int const   g0   = m_TriangleGroups.value()[et0];
        if ( std::find( GroupsOut.begin(), GroupsOut.end(), g0 ) == GroupsOut.end() )
        {
            GroupsOut.push_back( g0 );
        }

        int const et1 = Edge.Tri[1];
        if ( et1 != InvalidID )
        {
            int const g1 = m_TriangleGroups.value()[et1];
            if ( std::find( GroupsOut.begin(), GroupsOut.end(), g1 ) == GroupsOut.end() )
            {
                GroupsOut.push_back( g1 );
            }
        }
    }
    return true;
}

template bool DynamicMesh3::GetAllVertexGroups<std::vector<int32_t>>( int                   vID,
                                                                      std::vector<int32_t>& GroupsOut ) const;

/**
 * returns true if vID is a "bowtie" vertex, ie multiple disjoint triangle sets in one-ring
 */
bool DynamicMesh3::IsBowtieVertex( int vID ) const
{
    if ( !m_VertexRefCounts.IsValid( vID ) )
    {
        return false;
    }

    int const nEdges = m_VertexEdgeLists.GetCount( vID );
    if ( nEdges == 0 )
    {
        return false;
    }

    // find a boundary edge to start at
    int  start_eid         = -1;
    bool start_at_boundary = false;
    for ( int const eid : m_VertexEdgeLists.Values( vID ) )
    {
        const Edge Edge = m_Edges[eid];
        if ( Edge.Tri[1] == InvalidID )
        {
            start_at_boundary = true;
            start_eid         = eid;
            break;
        }
    }
    // if no boundary edge, start at arbitrary edge
    if ( start_eid == -1 )
    {
        start_eid = m_VertexEdgeLists.First( vID );
    }
    // initial triangle
    int const start_tid = m_Edges[start_eid].Tri[0];

    int prev_tid = start_tid;
    int prev_eid = start_eid;

    // walk forward to next edge. if we hit start edge or boundary edge,
    // we are done the walk. count number of edges as we go.
    int count = 1;
    while ( true )
    {
        int const       i        = prev_tid;
        const Index3i&  tv       = m_Triangles[i];
        const Index3i&  te       = m_TriangleEdges[i];
        int const       vert_idx = IndexUtil::FindTriIndex( vID, tv );
        int const       e1       = te[vert_idx];
        int const       e2       = te[( vert_idx + 2 ) % 3];
        int const       next_eid = ( e1 == prev_eid ) ? e2 : e1;
        if ( next_eid == start_eid )
        {
            break;
        }
        Index2i  next_eid_tris = GetEdgeT( next_eid );
        int const next_tid      = ( next_eid_tris[0] == prev_tid ) ? next_eid_tris[1] : next_eid_tris[0];
        if ( next_tid == InvalidID )
        {
            break;
        }
        prev_eid = next_eid;
        prev_tid = next_tid;
        count++;
    }

    // if we did not see all edges at vertex, we have a bowtie
    int const  target_count = ( start_at_boundary ) ? nEdges - 1 : nEdges;
    bool const is_bowtie    = ( target_count != count );
    return is_bowtie;
}

int DynamicMesh3::FindTriangle( int a, int b, int c ) const
{
    int const eid = FindEdge( a, b );
    if ( eid == InvalidID )
    {
        return InvalidID;
    }
    const Edge Edge = m_Edges[eid];

    // triangles attached to edge [a,b] must contain verts a and b...
    int ti = Edge.Tri[0];
    if ( m_Triangles[ti][0] == c || m_Triangles[ti][1] == c || m_Triangles[ti][2] == c )
    {
        return Edge.Tri[0];
    }
    if ( Edge.Tri[1] != InvalidID )
    {
        ti = Edge.Tri[1];
        if ( m_Triangles[ti][0] == c || m_Triangles[ti][1] == c || m_Triangles[ti][2] == c )
        {
            return Edge.Tri[1];
        }
    }

    return InvalidID;
}

/**
 * Computes bounding box of all vertices.
 */
AxisAlignedBox3d DynamicMesh3::GetBounds() const
{
    if ( VertexCount() == 0 )
    {
        return AxisAlignedBox3d::Empty();
    }

    glm::dvec3 MinVec = m_Vertices[*( VertexIndicesItr().begin() )];
    glm::dvec3 MaxVec = MinVec;
    for ( int const vi : VertexIndicesItr() )
    {
        MinVec = Min( MinVec, m_Vertices[vi] );
        MaxVec = Max( MaxVec, m_Vertices[vi] );
    }
    return AxisAlignedBox3d( MinVec, MaxVec );
}

/**
 * Computes bounding box of selected vertices.
 */
AxisAlignedBox3d DynamicMesh3::GetBoundsForVertexSelection( std::span<const int32_t> VertexIDs ) const
{
    auto const NumVertices = static_cast<int32_t>( VertexIDs.size() );
    if ( NumVertices == 0 )
    {
        return AxisAlignedBox3d::Empty();
    }

    glm::dvec3 MinVec = m_Vertices[VertexIDs[0]];
    glm::dvec3 MaxVec = MinVec;
    for ( int Idx = 1; Idx < static_cast<int32_t>( VertexIDs.size() ); ++Idx )
    {
        int32_t const VID = VertexIDs[Idx];
        MinVec            = Min( MinVec, m_Vertices[VID] );
        MaxVec            = Max( MaxVec, m_Vertices[VID] );
    }
    return AxisAlignedBox3d( MinVec, MaxVec );
}

AxisAlignedBox3d DynamicMesh3::GetBoundsForTriangleSelection( std::span<const int32_t> TriangleIDs ) const
{
    auto const NumTriangles = static_cast<int32_t>( TriangleIDs.size() );
    if ( NumTriangles == 0 )
    {
        return AxisAlignedBox3d::Empty();
    }

    auto UpdateBoundsWithTriangles = [this]( AxisAlignedBox3d& Bounds, std::span<const int32_t> IndexArray )
    {
        for ( int32_t const TID : IndexArray )
        {
            const Index3i& Tri = m_Triangles[TID];
            Bounds.Contain( m_Vertices[Tri.A] );
            Bounds.Contain( m_Vertices[Tri.B] );
            Bounds.Contain( m_Vertices[Tri.C] );
        }
    };

    AxisAlignedBox3d ResultBounds = AxisAlignedBox3d::Empty();

    UpdateBoundsWithTriangles( ResultBounds, TriangleIDs );
    return ResultBounds;
}

bool DynamicMesh3::IsClosed() const
{
    if ( TriangleCount() == 0 )
    {
        return false;
    }

    int const N = MaxEdgeID();
    for ( int i = 0; i < N; ++i )
    {
        if ( m_EdgeRefCounts.IsValid( i ) && IsBoundaryEdge( i ) )
        {
            return false;
        }
    }
    return true;
}

// average of 1 or 2 face normals
glm::dvec3 DynamicMesh3::GetEdgeNormal( int eID ) const
{
    if ( m_EdgeRefCounts.IsValid( eID ) )
    {
        const Index2i  Tris = m_Edges[eID].Tri;
        glm::dvec3     n    = GetTriNormal( Tris[0] );
        if ( Tris[1] != InvalidID )
        {
            n += GetTriNormal( Tris[1] );
            Normalize( n );
        }
        return n;
    }
    assert( false );
    return glm::dvec3( 0 );
}

glm::dvec3 DynamicMesh3::GetEdgePoint( int eID, double t ) const
{
    t = VectorUtil::Clamp( t, 0.0, 1.0 );
    if ( m_EdgeRefCounts.IsValid( eID ) )
    {
        Index2i   Verts = m_Edges[eID].Vert;
        const int iv0   = Verts[0];
        const int iv1   = Verts[1];
        double const mt    = 1.0 - t;
        return mt * m_Vertices[iv0] + t * m_Vertices[iv1];
    }
    assert( false );
    return glm::dvec3( 0 );
}

void DynamicMesh3::GetVtxOneRingCentroid( int vID, glm::dvec3& centroid ) const
{
    centroid = glm::dvec3( 0 );
    if ( m_VertexRefCounts.IsValid( vID ) )
    {
        int n = 0;
        for ( int const eid : m_VertexEdgeLists.Values( vID ) )
        {
            int const other_idx = GetOtherEdgeVertex( eid, vID );
            centroid += m_Vertices[other_idx];
            n++;
        }
        if ( n > 0 )
        {
            centroid *= 1.0 / n;
        }
    }
}

glm::dvec3 DynamicMesh3::GetTriNormal( int tID ) const
{
    glm::dvec3 v0{};
    glm::dvec3 v1{};
    glm::dvec3 v2{};
    GetTriVertices( tID, v0, v1, v2 );
    return VectorUtil::Normal( v0, v1, v2 );
}

double DynamicMesh3::GetTriArea( int tID ) const
{
    glm::dvec3 v0{};
    glm::dvec3 v1{};
    glm::dvec3 v2{};
    GetTriVertices( tID, v0, v1, v2 );
    return VectorUtil::Area( v0, v1, v2 );
}

void DynamicMesh3::GetTriInfo( int tID, glm::dvec3& Normal, double& Area, glm::dvec3& Centroid ) const
{
    glm::dvec3 v0{};
    glm::dvec3 v1{};
    glm::dvec3 v2{};
    GetTriVertices( tID, v0, v1, v2 );
    Centroid = ( v0 + v1 + v2 ) * ( 1.0 / 3.0 );
    Normal   = VectorUtil::NormalArea( v0, v1, v2, Area );
}

glm::dvec3 DynamicMesh3::GetTriBaryPoint( int tID, double bary0, double bary1, double bary2 ) const
{
    const Index3i& tIDs = m_Triangles[tID];
    return bary0 * m_Vertices[tIDs[0]] + bary1 * m_Vertices[tIDs[1]] + bary2 * m_Vertices[tIDs[2]];
}

glm::dvec3 DynamicMesh3::GetTriBaryNormal( int tID, double bary0, double bary1, double bary2 ) const
{
    assert( HasVertexNormals() );
    if ( HasVertexNormals() )
    {
        const Index3i&                  tIDs     = m_Triangles[tID];
        const DynamicVector<glm::vec3>& normalsR = m_VertexNormals.value();
        glm::dvec3                      n        = glm::dvec3( static_cast<float>( bary0 ) * normalsR[tIDs[0]] +
                                                               static_cast<float>( bary1 ) * normalsR[tIDs[1]] +
                                                               static_cast<float>( bary2 ) * normalsR[tIDs[2]] );
        Normalize( n );
        return n;
    }
    return glm::dvec3( 0 );
}

glm::dvec3 DynamicMesh3::GetTriCentroid( int tID ) const
{
    const Index3i&  tIDs = m_Triangles[tID];
    double const    f    = ( 1.0 / 3.0 );
    return ( m_Vertices[tIDs[0]] + m_Vertices[tIDs[1]] + m_Vertices[tIDs[2]] ) * f;
}

void DynamicMesh3::GetTriBaryPoint( int tID, double bary0, double bary1, double bary2, VertexInfo& vinfo ) const
{
    vinfo                = VertexInfo();
    const Index3i& tIDs  = m_Triangles[tID];
    vinfo.Position       = bary0 * m_Vertices[tIDs[0]] + bary1 * m_Vertices[tIDs[1]] + bary2 * m_Vertices[tIDs[2]];
    vinfo.bHaveN         = HasVertexNormals();
    if ( vinfo.bHaveN )
    {
        const DynamicVector<glm::vec3>& normalsR = this->m_VertexNormals.value();
        vinfo.Normal                             = static_cast<float>( bary0 ) * normalsR[tIDs[0]] +
                       static_cast<float>( bary1 ) * normalsR[tIDs[1]] +
                       static_cast<float>( bary2 ) * normalsR[tIDs[2]];
        Normalize( vinfo.Normal );
    }
    vinfo.bHaveC = HasVertexColors();
    if ( vinfo.bHaveC )
    {
        const DynamicVector<glm::vec3>& colorsR = this->m_VertexColors.value();
        vinfo.Color                             = static_cast<float>( bary0 ) * colorsR[tIDs[0]] +
                      static_cast<float>( bary1 ) * colorsR[tIDs[1]] +
                      static_cast<float>( bary2 ) * colorsR[tIDs[2]];
    }
    vinfo.bHaveUV = HasVertexUVs();
    if ( vinfo.bHaveUV )
    {
        const DynamicVector<glm::vec2>& uvR = this->m_VertexUVs.value();
        vinfo.UV = static_cast<float>( bary0 ) * uvR[tIDs[0]] + static_cast<float>( bary1 ) * uvR[tIDs[1]] +
                   static_cast<float>( bary2 ) * uvR[tIDs[2]];
    }
}

AxisAlignedBox3d DynamicMesh3::GetTriBounds( int tID ) const
{
    const Index3i&    tIDs = m_Triangles[tID];
    const glm::dvec3& A    = m_Vertices[tIDs.A];
    const glm::dvec3& B    = m_Vertices[tIDs.B];
    const glm::dvec3& C    = m_Vertices[tIDs.C];
    return AxisAlignedBox3d( A, B, C );
}

double DynamicMesh3::GetTriSolidAngle( int tID, const glm::dvec3& p ) const
{
    // inlined version of GetTriVertices & VectorUtil::TriSolidAngle
    const Index3i&   Triangle = m_Triangles[tID];
    const glm::dvec3 TV[3]    = { m_Vertices[Triangle[0]] - p, m_Vertices[Triangle[1]] - p,
                                  m_Vertices[Triangle[2]] - p };

    double const la  = glm::length( TV[0] );
    double const lb  = glm::length( TV[1] );
    double const lc  = glm::length( TV[2] );
    double const top = ( la * lb * lc ) + glm::dot( TV[0], TV[1] ) * lc + glm::dot( TV[1], TV[2] ) * la +
                       glm::dot( TV[2], TV[0] ) * lb;
    double const bottom = TV[0].x * ( TV[1].y * TV[2].z - TV[2].y * TV[1].z ) -
                          TV[0].y * ( TV[1].x * TV[2].z - TV[2].x * TV[1].z ) +
                          TV[0].z * ( TV[1].x * TV[2].y - TV[2].x * TV[1].y );
    // -2 instead of 2 to account for UE winding
    return -2.0 * atan2( bottom, top );
}

double DynamicMesh3::GetTriInternalAngleR( int tID, int i ) const
{
    const Index3i&   Triangle = m_Triangles[tID];
    const glm::dvec3 TV[3]    = { m_Vertices[Triangle[0]], m_Vertices[Triangle[1]], m_Vertices[Triangle[2]] };
    if ( i == 0 )
    {
        return AngleR( Normalized( TV[1] - TV[0] ), Normalized( TV[2] - TV[0] ) );
    }
    if ( i == 1 )
    {
        return AngleR( Normalized( TV[0] - TV[1] ), Normalized( TV[2] - TV[1] ) );
    }
    else
    {
        return AngleR( Normalized( TV[0] - TV[2] ), Normalized( TV[1] - TV[2] ) );
    }
}

glm::dvec3 DynamicMesh3::GetTriInternalAnglesR( int tID ) const
{
    const Index3i& Triangle = m_Triangles[tID];
    return VectorUtil::TriangleInternalAngles( m_Vertices[Triangle[0]], m_Vertices[Triangle[1]],
                                               m_Vertices[Triangle[2]] );
}

double DynamicMesh3::CalculateWindingNumber( const glm::dvec3& QueryPoint ) const
{
    double sum = 0;
    for ( int const tid : TriangleIndicesItr() )
    {
        sum += GetTriSolidAngle( tid, QueryPoint );
    }
    return sum / ( static_cast<double>( 4 ) * glm::pi<double>() );
}
