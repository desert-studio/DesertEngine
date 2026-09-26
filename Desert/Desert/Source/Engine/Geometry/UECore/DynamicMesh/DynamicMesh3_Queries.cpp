// Ported from UE 5.8 Engine/Source/Runtime/GeometryCore/Private/DynamicMesh/DynamicMesh3_Queries.cpp:1-1012,
// adapted: UE Core via UECore.hpp; LocalIntArray is TArray<int32_t> so its explicit instantiations collapse into
// the TArray ones; GetVertexFrame/GetTriFrame (Frame3d) not ported; bounds are computed serially.
#include "Engine/Geometry/UECore/DynamicMesh/DynamicMesh3.hpp"

using namespace Desert::Geometry;

Index2i DynamicMesh3::GetEdgeOpposingV( int eID ) const
{
    const Edge&  Edge = Edges[eID];
    int          a    = Edge.Vert[0];
    int          b    = Edge.Vert[1];

    // ** it is important that verts returned maintain [c,d] order!!
    int c = IndexUtil::FindTriOtherVtxUnsafe( a, b, Triangles[Edge.Tri[0]] );
    if ( Edge.Tri[1] != InvalidID )
    {
        int d = IndexUtil::FindTriOtherVtxUnsafe( a, b, Triangles[Edge.Tri[1]] );
        return Index2i( c, d );
    }
    else
    {
        return Index2i( c, InvalidID );
    }
}

int DynamicMesh3::GetVtxBoundaryEdges( int vID, int& e0, int& e1 ) const
{
    if ( VertexRefCounts.IsValid( vID ) )
    {
        int count = 0;
        for ( int eid : VertexEdgeLists.Values( vID ) )
        {
            if ( Edges[eid].Tri[1] == InvalidID )
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
    if ( VertexRefCounts.IsValid( vID ) )
    {
        int count = 0;
        for ( int eid : VertexEdgeLists.Values( vID ) )
        {
            if ( Edges[eid].Tri[1] == InvalidID )
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
    const Edge Edge  = Edges[eID];
    vOther           = ( Edge.Vert[0] == vID ) ? Edge.Vert[1] : Edge.Vert[0];
    t1               = Edge.Tri[0];
    oppV1            = IndexUtil::FindTriOtherVtx( vID, vOther, Triangles, t1 );
    t2               = Edge.Tri[1];
    if ( t2 != InvalidID )
    {
        oppV2 = IndexUtil::FindTriOtherVtx( vID, vOther, Triangles, t2 );
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
    VertexEdgeLists.Enumerate( vID,
                               [&]( int32_t eid )
                               {
                                   const Edge  Edge   = Edges[eid];
                                   const int   vOther = Edge.Vert.A == vID ? Edge.Vert.B : Edge.Vert.A;
                                   if ( TriHasSequentialVertices( Edge.Tri[0], vID, vOther ) )
                                   {
                                       N++;
                                   }
                                   const int et1 = Edge.Tri[1];
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
    for ( int EID : VertexEdgeLists.Values( VertexID ) )
    {
        return Edges[EID].Tri[0];
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

    if ( VertexEdgeLists.GetCount( vID ) > 20 )
    {
        VertexEdgeLists.Enumerate( vID,
                                   [&]( int32_t eid )
                                   {
                                       const Edge  Edge   = Edges[eid];
                                       const int   vOther = Edge.Vert.A == vID ? Edge.Vert.B : Edge.Vert.A;
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
        VertexEdgeLists.Enumerate( vID,
                                   [&]( int32_t eid )
                                   {
                                       const Edge Edge = Edges[eid];
                                       if ( std::find( TrianglesOut.begin(), TrianglesOut.end(), Edge.Tri[0] ) ==
                                            TrianglesOut.end() )
                                       {
                                           TrianglesOut.push_back( Edge.Tri[0] );
                                       }
                                       if ( Edge.Tri[1] != InvalidID )
                                       {
                                           if ( std::find( TrianglesOut.begin(), TrianglesOut.end(),
                                                           Edge.Tri[1] ) == TrianglesOut.end() )
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

    if ( !UE_ENSURE( IsVertex( VertexID ) ) )
    {
        return MeshResult::Failed_NotAVertex;
    }

    int NumEdges = VertexEdgeLists.GetCount( VertexID );
    if ( NumEdges == 0 )
    {
        return MeshResult::Ok;
    }

    // The number of StartEdgeIDs for a vertex is NumBoundaryEdges + NumRingsWithoutBoundary
    // This is only higher than 2 at non-manifold (e.g., "bowtie") vertices, and
    // it should be very rare for it to exceed this inline allocation of 8
    LocalIntArray StartEdgeIDs;
    // initial starting edge candidates == boundary edges
    for ( int EID : VertexEdgeLists.Values( VertexID ) )
    {
        if ( Edges[EID].Tri[1] == InvalidID )
        {
            StartEdgeIDs.push_back( EID );
        }
    }
    bool bHasBoundaries = !StartEdgeIDs.empty();

    if ( !bHasBoundaries )
    {
        for ( int EID : VertexEdgeLists.Values( VertexID ) )
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
            for ( int EID : VertexEdgeLists.Values( VertexID ) )
            {
                int  AttachedTriID = Edges[EID].Tri[0];
                bool UsedEdge      = ( std::find( TrianglesOut.begin(), TrianglesOut.end(), AttachedTriID ) !=
                                  TrianglesOut.end() );
                if ( !UsedEdge )
                {
                    StartEdgeIDs.push_back( EID );
                    break;
                }
            }
        }

        // walk starting from this edge, add the found span

        int StartEID = StartEdgeIDs.back();
        StartEdgeIDs.pop_back();
        int PrevEID  = StartEID;
        WalkedEdges++;
        int   WalkTri   = Edges[StartEID].Tri[0];
        int32_t const SpanStart = static_cast<int32_t>( TrianglesOut.size() );
        IsLoop.push_back( !bHasRemainingBoundaries );
        while ( true )
        {
            TrianglesOut.push_back( WalkTri );

            int             TriIdx     = WalkTri;
            const Index3i&  TriVIDs    = Triangles[TriIdx];
            const Index3i&  TriEIDs    = TriangleEdges[TriIdx];
            int             VertSubIdx = IndexUtil::FindTriIndex( VertexID, TriVIDs );
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

            int NextTriID = Edges[NextEID].Tri[0];
            if ( NextTriID == WalkTri )
            {
                NextTriID = Edges[NextEID].Tri[1];
            }
            if ( NextTriID == InvalidID )
            {
                // remove the corresponding boundary
                UE_CHECK_SLOW( !StartEdgeIDs.empty() );
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

    return UE_ENSURE( SpanLengths.size() == IsLoop.size() ) ? MeshResult::Ok
                                                            : MeshResult::Failed_InvalidNeighbourhood;
}

template MeshResult DynamicMesh3::GetVtxContiguousTriangles<std::vector<int32_t>, std::vector<bool>>(
     int VertexID, std::vector<int32_t>& TrianglesOut, std::vector<int32_t>& SpanLengths,
     std::vector<bool>& IsLoop ) const;

bool DynamicMesh3::IsBoundaryVertex( int vID ) const
{
    UE_CHECK_SLOW( IsVertex( vID ) );
    if ( IsVertex( vID ) )
    {
        for ( int eid : VertexEdgeLists.Values( vID ) )
        {
            if ( Edges[eid].Tri[1] == InvalidID )
            {
                return true;
            }
        }
    }
    return false;
}

bool DynamicMesh3::IsBoundaryTriangle( int tID ) const
{
    UE_CHECK_SLOW( IsTriangle( tID ) );
    if ( IsTriangle( tID ) )
    {
        const Index3i& TriEdgeIDs = TriangleEdges[tID];
        return IsBoundaryEdge( TriEdgeIDs[0] ) || IsBoundaryEdge( TriEdgeIDs[1] ) ||
               IsBoundaryEdge( TriEdgeIDs[2] );
    }
    else
    {
        return false;
    }
}

Index2i DynamicMesh3::GetOrientedBoundaryEdgeV( int eID ) const
{
    if ( EdgeRefCounts.IsValid( eID ) )
    {
        const Edge Edge = Edges[eID];
        if ( Edge.Tri[1] == InvalidID )
        {
            int             a = Edge.Vert[0], b = Edge.Vert[1];
            int             ti  = Edge.Tri[0];
            const Index3i&  tri = Triangles[ti];
            int             ai  = IndexUtil::FindEdgeIndexInTri( a, b, tri );
            return Index2i( tri[ai], tri[( ai + 1 ) % 3] );
        }
    }
    UE_CHECK_SLOW( false );
    return InvalidEdge;
}

bool DynamicMesh3::IsGroupBoundaryEdge( int eID ) const
{
    if ( !HasTriangleGroups() )
        return false;

    const Edge  Edge = Edges[eID];
    int         et1  = Edge.Tri[1];
    if ( et1 == InvalidID )
    {
        return false;
    }
    int g1  = TriangleGroups.value()[et1];
    int et0 = Edge.Tri[0];
    int g0  = TriangleGroups.value()[et0];
    return g1 != g0;
}

bool DynamicMesh3::IsGroupBoundaryVertex( int vID ) const
{
    if ( !HasTriangleGroups() )
        return false;

    int group_id = InvalidID;
    for ( int eID : VertexEdgeLists.Values( vID ) )
    {
        const Edge  Edge = Edges[eID];
        int         et0  = Edge.Tri[0];
        int         g0   = TriangleGroups.value()[et0];
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
        int et1 = Edge.Tri[1];
        if ( et1 != InvalidID )
        {
            int g1 = TriangleGroups.value()[et1];
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
    for ( int eID : VertexEdgeLists.Values( vID ) )
    {
        const Edge Edge = Edges[eID];
        Index2i    et   = Edge.Tri;
        for ( int k = 0; k < 2; ++k )
        {
            if ( et[k] == InvalidID )
            {
                continue;
            }
            int g0 = TriangleGroups.value()[et[k]];
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

    for ( int eID : VertexEdgeLists.Values( vID ) )
    {
        const Edge Edge = Edges[eID];

        int et0 = Edge.Tri[0];
        int g0  = TriangleGroups.value()[et0];
        if ( groups.Contains( g0 ) == false )
        {
            groups[ng++] = g0;
        }
        if ( ng == 4 )
        {
            return false;
        }
        int et1 = Edge.Tri[1];
        if ( et1 != InvalidID )
        {
            int g1 = TriangleGroups.value()[et1];
            if ( groups.Contains( g1 ) == false )
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

    for ( int eID : VertexEdgeLists.Values( vID ) )
    {
        const Edge  Edge = Edges[eID];
        int         et0  = Edge.Tri[0];
        int         g0   = TriangleGroups.value()[et0];
        if ( std::find( GroupsOut.begin(), GroupsOut.end(), g0 ) == GroupsOut.end() )
        {
            GroupsOut.push_back( g0 );
        }

        int et1 = Edge.Tri[1];
        if ( et1 != InvalidID )
        {
            int g1 = TriangleGroups.value()[et1];
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
    if ( VertexRefCounts.IsValid( vID ) == false )
    {
        return false;
    }

    int nEdges = VertexEdgeLists.GetCount( vID );
    if ( nEdges == 0 )
    {
        return false;
    }

    // find a boundary edge to start at
    int  start_eid         = -1;
    bool start_at_boundary = false;
    for ( int eid : VertexEdgeLists.Values( vID ) )
    {
        const Edge Edge = Edges[eid];
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
        start_eid = VertexEdgeLists.First( vID );
    }
    // initial triangle
    int start_tid = Edges[start_eid].Tri[0];

    int prev_tid = start_tid;
    int prev_eid = start_eid;

    // walk forward to next edge. if we hit start edge or boundary edge,
    // we are done the walk. count number of edges as we go.
    int count = 1;
    while ( true )
    {
        int             i        = prev_tid;
        const Index3i&  tv       = Triangles[i];
        const Index3i&  te       = TriangleEdges[i];
        int             vert_idx = IndexUtil::FindTriIndex( vID, tv );
        int             e1 = te[vert_idx], e2 = te[( vert_idx + 2 ) % 3];
        int             next_eid = ( e1 == prev_eid ) ? e2 : e1;
        if ( next_eid == start_eid )
        {
            break;
        }
        Index2i  next_eid_tris = GetEdgeT( next_eid );
        int      next_tid      = ( next_eid_tris[0] == prev_tid ) ? next_eid_tris[1] : next_eid_tris[0];
        if ( next_tid == InvalidID )
        {
            break;
        }
        prev_eid = next_eid;
        prev_tid = next_tid;
        count++;
    }

    // if we did not see all edges at vertex, we have a bowtie
    int  target_count = ( start_at_boundary ) ? nEdges - 1 : nEdges;
    bool is_bowtie    = ( target_count != count );
    return is_bowtie;
}

int DynamicMesh3::FindTriangle( int a, int b, int c ) const
{
    int eid = FindEdge( a, b );
    if ( eid == InvalidID )
    {
        return InvalidID;
    }
    const Edge Edge = Edges[eid];

    // triangles attached to edge [a,b] must contain verts a and b...
    int ti = Edge.Tri[0];
    if ( Triangles[ti][0] == c || Triangles[ti][1] == c || Triangles[ti][2] == c )
    {
        return Edge.Tri[0];
    }
    if ( Edge.Tri[1] != InvalidID )
    {
        ti = Edge.Tri[1];
        if ( Triangles[ti][0] == c || Triangles[ti][1] == c || Triangles[ti][2] == c )
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

    glm::dvec3 MinVec = Vertices[*( VertexIndicesItr().begin() )];
    glm::dvec3 MaxVec = MinVec;
    for ( int vi : VertexIndicesItr() )
    {
        MinVec = Min( MinVec, Vertices[vi] );
        MaxVec = Max( MaxVec, Vertices[vi] );
    }
    return AxisAlignedBox3d( MinVec, MaxVec );
}

/**
 * Computes bounding box of selected vertices.
 */
AxisAlignedBox3d DynamicMesh3::GetBoundsForVertexSelection( std::span<const int32_t> VertexIDs ) const
{
    int32_t const NumVertices = static_cast<int32_t>( VertexIDs.size() );
    if ( NumVertices == 0 )
    {
        return AxisAlignedBox3d::Empty();
    }

    glm::dvec3 MinVec = Vertices[VertexIDs[0]];
    glm::dvec3 MaxVec = MinVec;
    for ( int Idx = 1; Idx < static_cast<int32_t>( VertexIDs.size() ); ++Idx )
    {
        int32_t const VID = VertexIDs[Idx];
        MinVec    = Min( MinVec, Vertices[VID] );
        MaxVec    = Max( MaxVec, Vertices[VID] );
    }
    return AxisAlignedBox3d( MinVec, MaxVec );
}

AxisAlignedBox3d DynamicMesh3::GetBoundsForTriangleSelection( std::span<const int32_t> TriangleIDs ) const
{
    int32_t const NumTriangles = static_cast<int32_t>( TriangleIDs.size() );
    if ( NumTriangles == 0 )
    {
        return AxisAlignedBox3d::Empty();
    }

    auto UpdateBoundsWithTriangles = [this]( AxisAlignedBox3d& Bounds, std::span<const int32_t> IndexArray )
    {
        for ( int32_t const TID : IndexArray )
        {
            const Index3i& Tri = Triangles[TID];
            Bounds.Contain( Vertices[Tri.A] );
            Bounds.Contain( Vertices[Tri.B] );
            Bounds.Contain( Vertices[Tri.C] );
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

    int N = MaxEdgeID();
    for ( int i = 0; i < N; ++i )
    {
        if ( EdgeRefCounts.IsValid( i ) && IsBoundaryEdge( i ) )
        {
            return false;
        }
    }
    return true;
}

// average of 1 or 2 face normals
glm::dvec3 DynamicMesh3::GetEdgeNormal( int eID ) const
{
    if ( EdgeRefCounts.IsValid( eID ) )
    {
        const Index2i  Tris = Edges[eID].Tri;
        glm::dvec3     n    = GetTriNormal( Tris[0] );
        if ( Tris[1] != InvalidID )
        {
            n += GetTriNormal( Tris[1] );
            Normalize( n );
        }
        return n;
    }
    UE_CHECK_SLOW( false );
    return glm::dvec3( 0 );
}

glm::dvec3 DynamicMesh3::GetEdgePoint( int eID, double t ) const
{
    t = VectorUtil::Clamp( t, 0.0, 1.0 );
    if ( EdgeRefCounts.IsValid( eID ) )
    {
        Index2i   Verts = Edges[eID].Vert;
        const int iv0   = Verts[0];
        const int iv1   = Verts[1];
        double    mt    = 1.0 - t;
        return mt * Vertices[iv0] + t * Vertices[iv1];
    }
    UE_CHECK_SLOW( false );
    return glm::dvec3( 0 );
}

void DynamicMesh3::GetVtxOneRingCentroid( int vID, glm::dvec3& centroid ) const
{
    centroid = glm::dvec3( 0 );
    if ( VertexRefCounts.IsValid( vID ) )
    {
        int n = 0;
        for ( int eid : VertexEdgeLists.Values( vID ) )
        {
            int other_idx = GetOtherEdgeVertex( eid, vID );
            centroid += Vertices[other_idx];
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
    glm::dvec3 v0{}, v1{}, v2{};
    GetTriVertices( tID, v0, v1, v2 );
    return VectorUtil::Normal( v0, v1, v2 );
}

double DynamicMesh3::GetTriArea( int tID ) const
{
    glm::dvec3 v0{}, v1{}, v2{};
    GetTriVertices( tID, v0, v1, v2 );
    return VectorUtil::Area( v0, v1, v2 );
}

void DynamicMesh3::GetTriInfo( int tID, glm::dvec3& Normal, double& Area, glm::dvec3& Centroid ) const
{
    glm::dvec3 v0{}, v1{}, v2{};
    GetTriVertices( tID, v0, v1, v2 );
    Centroid = ( v0 + v1 + v2 ) * ( 1.0 / 3.0 );
    Normal   = VectorUtil::NormalArea( v0, v1, v2, Area );
}

glm::dvec3 DynamicMesh3::GetTriBaryPoint( int tID, double bary0, double bary1, double bary2 ) const
{
    const Index3i& tIDs = Triangles[tID];
    return bary0 * Vertices[tIDs[0]] + bary1 * Vertices[tIDs[1]] + bary2 * Vertices[tIDs[2]];
}

glm::dvec3 DynamicMesh3::GetTriBaryNormal( int tID, double bary0, double bary1, double bary2 ) const
{
    UE_CHECK_SLOW( HasVertexNormals() );
    if ( HasVertexNormals() )
    {
        const Index3i&                  tIDs     = Triangles[tID];
        const DynamicVector<glm::vec3>& normalsR = VertexNormals.value();
        glm::dvec3 n = glm::dvec3( float( bary0 ) * normalsR[tIDs[0]] + float( bary1 ) * normalsR[tIDs[1]] +
                                   float( bary2 ) * normalsR[tIDs[2]] );
        Normalize( n );
        return n;
    }
    return glm::dvec3( 0 );
}

glm::dvec3 DynamicMesh3::GetTriCentroid( int tID ) const
{
    const Index3i&  tIDs = Triangles[tID];
    double          f    = ( 1.0 / 3.0 );
    return ( Vertices[tIDs[0]] + Vertices[tIDs[1]] + Vertices[tIDs[2]] ) * f;
}

void DynamicMesh3::GetTriBaryPoint( int tID, double bary0, double bary1, double bary2, VertexInfo& vinfo ) const
{
    vinfo                = VertexInfo();
    const Index3i& tIDs  = Triangles[tID];
    vinfo.Position       = bary0 * Vertices[tIDs[0]] + bary1 * Vertices[tIDs[1]] + bary2 * Vertices[tIDs[2]];
    vinfo.bHaveN         = HasVertexNormals();
    if ( vinfo.bHaveN )
    {
        const DynamicVector<glm::vec3>& normalsR = this->VertexNormals.value();
        vinfo.Normal = (float)bary0 * normalsR[tIDs[0]] + (float)bary1 * normalsR[tIDs[1]] +
                       (float)bary2 * normalsR[tIDs[2]];
        Normalize( vinfo.Normal );
    }
    vinfo.bHaveC = HasVertexColors();
    if ( vinfo.bHaveC )
    {
        const DynamicVector<glm::vec3>& colorsR = this->VertexColors.value();
        vinfo.Color =
             (float)bary0 * colorsR[tIDs[0]] + (float)bary1 * colorsR[tIDs[1]] + (float)bary2 * colorsR[tIDs[2]];
    }
    vinfo.bHaveUV = HasVertexUVs();
    if ( vinfo.bHaveUV )
    {
        const DynamicVector<glm::vec2>& uvR = this->VertexUVs.value();
        vinfo.UV = (float)bary0 * uvR[tIDs[0]] + (float)bary1 * uvR[tIDs[1]] + (float)bary2 * uvR[tIDs[2]];
    }
}

AxisAlignedBox3d DynamicMesh3::GetTriBounds( int tID ) const
{
    const Index3i&    tIDs = Triangles[tID];
    const glm::dvec3& A    = Vertices[tIDs.A];
    const glm::dvec3& B    = Vertices[tIDs.B];
    const glm::dvec3& C    = Vertices[tIDs.C];
    return AxisAlignedBox3d( A, B, C );
}

double DynamicMesh3::GetTriSolidAngle( int tID, const glm::dvec3& p ) const
{
    // inlined version of GetTriVertices & VectorUtil::TriSolidAngle
    const Index3i&   Triangle = Triangles[tID];
    const glm::dvec3 TV[3] = { Vertices[Triangle[0]] - p, Vertices[Triangle[1]] - p, Vertices[Triangle[2]] - p };

    double la = glm::length( TV[0] ), lb = glm::length( TV[1] ), lc = glm::length( TV[2] );
    double top = ( la * lb * lc ) + glm::dot( TV[0], TV[1] ) * lc + glm::dot( TV[1], TV[2] ) * la +
                 glm::dot( TV[2], TV[0] ) * lb;
    double bottom = TV[0].x * ( TV[1].y * TV[2].z - TV[2].y * TV[1].z ) -
                    TV[0].y * ( TV[1].x * TV[2].z - TV[2].x * TV[1].z ) +
                    TV[0].z * ( TV[1].x * TV[2].y - TV[2].x * TV[1].y );
    // -2 instead of 2 to account for UE winding
    return -2.0 * atan2( bottom, top );
}

double DynamicMesh3::GetTriInternalAngleR( int tID, int i ) const
{
    const Index3i&   Triangle = Triangles[tID];
    const glm::dvec3 TV[3]    = { Vertices[Triangle[0]], Vertices[Triangle[1]], Vertices[Triangle[2]] };
    if ( i == 0 )
    {
        return AngleR( Normalized( TV[1] - TV[0] ), Normalized( TV[2] - TV[0] ) );
    }
    else if ( i == 1 )
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
    const Index3i& Triangle = Triangles[tID];
    return VectorUtil::TriangleInternalAngles( Vertices[Triangle[0]], Vertices[Triangle[1]],
                                               Vertices[Triangle[2]] );
}

double DynamicMesh3::CalculateWindingNumber( const glm::dvec3& QueryPoint ) const
{
    double sum = 0;
    for ( int tid : TriangleIndicesItr() )
    {
        sum += GetTriSolidAngle( tid, QueryPoint );
    }
    return sum / ( double( 4 ) * glm::pi<double>() );
}
