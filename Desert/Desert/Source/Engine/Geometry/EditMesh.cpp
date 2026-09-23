#include "EditMesh.hpp"

#include <glm/common.hpp>

#include <algorithm>
#include <cassert>
#include <utility>

namespace Desert::Geometry
{
    const char* ToString( EditResult result )
    {
        switch ( result )
        {
            case EditResult::Ok:
                return "Ok";
            case EditResult::InvalidVertex:
                return "InvalidVertex";
            case EditResult::InvalidTriangle:
                return "InvalidTriangle";
            case EditResult::InvalidEdge:
                return "InvalidEdge";
            case EditResult::DegenerateTriangle:
                return "DegenerateTriangle";
            case EditResult::DuplicateTriangle:
                return "DuplicateTriangle";
            case EditResult::NonManifoldEdge:
                return "NonManifoldEdge";
            case EditResult::InconsistentOrientation:
                return "InconsistentOrientation";
            case EditResult::BoundaryEdge:
                return "BoundaryEdge";
            case EditResult::FlipCreatesExistingEdge:
                return "FlipCreatesExistingEdge";
            case EditResult::CollapseBreaksTopology:
                return "CollapseBreaksTopology";
        }
        return "Unknown";
    }

    namespace
    {
        std::array<int, 2> Sorted( int a, int b )
        {
            return a < b ? std::array<int, 2>{ a, b } : std::array<int, 2>{ b, a };
        }

        int Next( int j )
        {
            return ( j + 1 ) % 3;
        }

        int Prev( int j )
        {
            return ( j + 2 ) % 3;
        }
    } // namespace

    // ── ID pools ─────────────────────────────────────────────────────────────────────────────────────

    int EditMesh::AllocateId( std::vector<uint8_t>& alive, std::vector<int>& freeList, int& liveCount )
    {
        ++liveCount;
        if ( !freeList.empty() )
        {
            const int id = freeList.back();
            freeList.pop_back();
            alive[id] = 1;
            return id;
        }
        alive.push_back( 1 );
        return static_cast<int>( alive.size() ) - 1;
    }

    void EditMesh::FreeId( std::vector<uint8_t>& alive, std::vector<int>& freeList, int& liveCount, int id )
    {
        assert( alive[id] != 0 );
        alive[id] = 0;
        freeList.push_back( id );
        --liveCount;
    }

    // ── low-level bookkeeping ────────────────────────────────────────────────────────────────────────

    int EditMesh::AddEdge( int a, int b )
    {
        const int e = AllocateId( m_EdgeAlive, m_EdgeFree, m_EdgeLive );
        if ( e == static_cast<int>( m_EdgeVertices.size() ) )
        {
            m_EdgeVertices.emplace_back();
            m_EdgeTriangles.emplace_back();
        }
        m_EdgeVertices[e]  = Sorted( a, b );
        m_EdgeTriangles[e] = { InvalidId, InvalidId };
        m_VertexEdges[a].push_back( e );
        m_VertexEdges[b].push_back( e );
        return e;
    }

    void EditMesh::RemoveEdge( int e )
    {
        EraseVertexEdge( m_EdgeVertices[e][0], e );
        EraseVertexEdge( m_EdgeVertices[e][1], e );
        m_EdgeTriangles[e] = { InvalidId, InvalidId };
        FreeId( m_EdgeAlive, m_EdgeFree, m_EdgeLive, e );
    }

    void EditMesh::AddEdgeTriangle( int e, int t )
    {
        auto& tris = m_EdgeTriangles[e];
        if ( tris[0] == InvalidId )
            tris[0] = t;
        else
        {
            assert( tris[1] == InvalidId );
            tris[1] = t;
        }
    }

    void EditMesh::RemoveEdgeTriangle( int e, int t )
    {
        auto& tris = m_EdgeTriangles[e];
        if ( tris[0] == t )
        {
            tris[0] = tris[1];
            tris[1] = InvalidId;
        }
        else if ( tris[1] == t )
            tris[1] = InvalidId;
    }

    void EditMesh::ReplaceEdgeTriangle( int e, int oldT, int newT )
    {
        if ( newT == InvalidId )
        {
            RemoveEdgeTriangle( e, oldT );
            return;
        }
        auto& tris = m_EdgeTriangles[e];
        if ( tris[0] == oldT )
            tris[0] = newT;
        else if ( tris[1] == oldT )
            tris[1] = newT;
    }

    void EditMesh::EraseVertexEdge( int v, int e )
    {
        auto& edges = m_VertexEdges[v];
        const auto it = std::find( edges.begin(), edges.end(), e );
        if ( it != edges.end() )
            edges.erase( it );
    }

    void EditMesh::ReplaceEdgeVertex( int e, int oldV, int newV )
    {
        EraseVertexEdge( oldV, e );
        m_VertexEdges[newV].push_back( e );
        const auto& verts = m_EdgeVertices[e];
        const int   other = verts[0] == oldV ? verts[1] : verts[0];
        m_EdgeVertices[e] = Sorted( other, newV );
    }

    int EditMesh::OtherTriangle( int e, int t ) const
    {
        const auto& tris = m_EdgeTriangles[e];
        return tris[0] == t ? tris[1] : tris[0];
    }

    int EditMesh::EdgeSlot( int t, int e ) const
    {
        const auto& edges = m_TriangleEdges[t];
        for ( int j = 0; j < 3; ++j )
            if ( edges[j] == e )
                return j;
        assert( false && "triangle does not own this edge" );
        return 0;
    }

    // ── construction ─────────────────────────────────────────────────────────────────────────────────

    int EditMesh::AppendVertex( const glm::vec3& position )
    {
        const int v = AllocateId( m_VertexAlive, m_VertexFree, m_VertexLive );
        if ( v == static_cast<int>( m_Positions.size() ) )
        {
            m_Positions.emplace_back();
            m_VertexEdges.emplace_back();
        }
        m_Positions[v] = position;
        m_VertexEdges[v].clear();
        return v;
    }

    EditResult EditMesh::AppendTriangle( int a, int b, int c, int& outTriangle )
    {
        if ( !IsVertex( a ) || !IsVertex( b ) || !IsVertex( c ) )
            return EditResult::InvalidVertex;
        if ( a == b || b == c || c == a )
            return EditResult::DegenerateTriangle;
        if ( FindTriangle( a, b, c ) != InvalidId )
            return EditResult::DuplicateTriangle;

        const std::array<int, 3> corners{ a, b, c };
        std::array<int, 3>       edges{ InvalidId, InvalidId, InvalidId };
        for ( int j = 0; j < 3; ++j )
        {
            const int u = corners[j];
            const int w = corners[Next( j )];
            const int e = FindEdge( u, w );
            if ( e == InvalidId )
                continue;
            const auto& tris = m_EdgeTriangles[e];
            if ( tris[1] != InvalidId )
                return EditResult::NonManifoldEdge;
            // The one triangle already on this edge must run w -> u for the two to agree on a side.
            const int owner = tris[0];
            if ( m_TriangleVertices[owner][EdgeSlot( owner, e )] == u )
                return EditResult::InconsistentOrientation;
            edges[j] = e;
        }

        const int t = AllocateId( m_TriangleAlive, m_TriangleFree, m_TriangleLive );
        if ( t == static_cast<int>( m_TriangleVertices.size() ) )
        {
            m_TriangleVertices.emplace_back();
            m_TriangleEdges.emplace_back();
        }
        for ( int j = 0; j < 3; ++j )
        {
            if ( edges[j] == InvalidId )
                edges[j] = AddEdge( corners[j], corners[Next( j )] );
            AddEdgeTriangle( edges[j], t );
        }
        m_TriangleVertices[t] = corners;
        m_TriangleEdges[t]    = edges;
        outTriangle           = t;
        return EditResult::Ok;
    }

    EditResult EditMesh::RemoveTriangle( int triangle, bool removeIsolatedVertices )
    {
        if ( !IsTriangle( triangle ) )
            return EditResult::InvalidTriangle;

        const std::array<int, 3> corners = m_TriangleVertices[triangle];
        for ( const int e : m_TriangleEdges[triangle] )
        {
            RemoveEdgeTriangle( e, triangle );
            if ( m_EdgeTriangles[e][0] == InvalidId )
                RemoveEdge( e );
        }
        FreeId( m_TriangleAlive, m_TriangleFree, m_TriangleLive, triangle );

        if ( removeIsolatedVertices )
            for ( const int v : corners )
                if ( m_VertexEdges[v].empty() )
                    FreeId( m_VertexAlive, m_VertexFree, m_VertexLive, v );
        return EditResult::Ok;
    }

    // ── edits ────────────────────────────────────────────────────────────────────────────────────────

    EditResult EditMesh::SplitEdge( int edge, float t, SplitEdgeInfo& out )
    {
        if ( !IsEdge( edge ) )
            return EditResult::InvalidEdge;

        const int                v0   = m_EdgeVertices[edge][0];
        const int                v1   = m_EdgeVertices[edge][1];
        const std::array<int, 2> tris = m_EdgeTriangles[edge];

        const int f       = AppendVertex( glm::mix( m_Positions[v0], m_Positions[v1], t ) );
        const int newEdge = AddEdge( f, v1 );
        ReplaceEdgeVertex( edge, v1, f );
        m_EdgeTriangles[edge] = { InvalidId, InvalidId };

        out              = {};
        out.OriginalEdge = edge;
        out.NewVertex    = f;
        out.NewEdge      = newEdge;

        for ( int side = 0; side < 2; ++side )
        {
            const int tri = tris[side];
            if ( tri == InvalidId )
                continue;

            // tri runs p -> q -> o with (p, q) the split edge; it becomes (p, f, o) in place and the new
            // triangle takes (f, q, o), so both keep the original winding.
            const int j   = EdgeSlot( tri, edge );
            const int p   = m_TriangleVertices[tri][j];
            const int q   = m_TriangleVertices[tri][Next( j )];
            const int o   = m_TriangleVertices[tri][Prev( j )];
            const int eqo = m_TriangleEdges[tri][Next( j )];

            const int halfP = p == v0 ? edge : newEdge;
            const int halfQ = q == v0 ? edge : newEdge;
            const int spoke = AddEdge( f, o );

            const int created = AllocateId( m_TriangleAlive, m_TriangleFree, m_TriangleLive );
            if ( created == static_cast<int>( m_TriangleVertices.size() ) )
            {
                m_TriangleVertices.emplace_back();
                m_TriangleEdges.emplace_back();
            }

            m_TriangleVertices[tri][Next( j )] = f;
            m_TriangleEdges[tri][j]            = halfP;
            m_TriangleEdges[tri][Next( j )]    = spoke;

            m_TriangleVertices[created] = { f, q, o };
            m_TriangleEdges[created]    = { halfQ, eqo, spoke };

            AddEdgeTriangle( halfP, tri );
            AddEdgeTriangle( halfQ, created );
            ReplaceEdgeTriangle( eqo, tri, created );
            AddEdgeTriangle( spoke, tri );
            AddEdgeTriangle( spoke, created );

            out.NewTriangles[side] = created;
            out.NewSpokes[side]    = spoke;
        }
        return EditResult::Ok;
    }

    EditResult EditMesh::FlipEdge( int edge, FlipEdgeInfo& out )
    {
        if ( !IsEdge( edge ) )
            return EditResult::InvalidEdge;
        const int t0 = m_EdgeTriangles[edge][0];
        const int t1 = m_EdgeTriangles[edge][1];
        if ( t1 == InvalidId )
            return EditResult::BoundaryEdge;

        // t0 = (a, b, c), t1 = (b, a, d): opposite windings on the shared edge, an invariant.
        const int j0 = EdgeSlot( t0, edge );
        const int j1 = EdgeSlot( t1, edge );
        const int a  = m_TriangleVertices[t0][j0];
        const int b  = m_TriangleVertices[t0][Next( j0 )];
        const int c  = m_TriangleVertices[t0][Prev( j0 )];
        const int d  = m_TriangleVertices[t1][Prev( j1 )];
        if ( c == d || FindEdge( c, d ) != InvalidId )
            return EditResult::FlipCreatesExistingEdge;

        const int ebc = m_TriangleEdges[t0][Next( j0 )];
        const int eca = m_TriangleEdges[t0][Prev( j0 )];
        const int ead = m_TriangleEdges[t1][Next( j1 )];
        const int edb = m_TriangleEdges[t1][Prev( j1 )];

        // The quad's outer loop is b->c->a->d->b; its other diagonal splits it into (c, d, b) and (d, c, a).
        m_TriangleVertices[t0] = { c, d, b };
        m_TriangleEdges[t0]    = { edge, edb, ebc };
        m_TriangleVertices[t1] = { d, c, a };
        m_TriangleEdges[t1]    = { edge, eca, ead };
        ReplaceEdgeTriangle( eca, t0, t1 );
        ReplaceEdgeTriangle( edb, t1, t0 );

        EraseVertexEdge( a, edge );
        EraseVertexEdge( b, edge );
        m_VertexEdges[c].push_back( edge );
        m_VertexEdges[d].push_back( edge );
        m_EdgeVertices[edge] = Sorted( c, d );

        out             = {};
        out.Edge        = edge;
        out.OldVertices = Sorted( a, b );
        out.NewVertices = Sorted( c, d );
        out.Triangles   = { t0, t1 };
        return EditResult::Ok;
    }

    EditResult EditMesh::CollapseEdge( int keepVertex, int removeVertex, float t, CollapseEdgeInfo& out )
    {
        if ( !IsVertex( keepVertex ) || !IsVertex( removeVertex ) )
            return EditResult::InvalidVertex;
        const int a    = keepVertex;
        const int b    = removeVertex;
        const int edge = FindEdge( a, b );
        if ( edge == InvalidId )
            return EditResult::InvalidEdge;

        const std::array<int, 2> tris = m_EdgeTriangles[edge];
        std::array<int, 2>       opposite{ InvalidId, InvalidId };
        for ( int side = 0; side < 2; ++side )
        {
            const int tri = tris[side];
            if ( tri == InvalidId )
                continue;
            opposite[side] = m_TriangleVertices[tri][Prev( EdgeSlot( tri, edge ) )];
        }
        if ( opposite[0] == opposite[1] )
            return EditResult::CollapseBreaksTopology;

        // Link condition: a and b may share no neighbour except the corners opposite the edge; any other
        // common neighbour x would fold (a, x) and (b, x) into one edge carrying 3+ triangles.
        for ( const int be : m_VertexEdges[b] )
        {
            const int x = m_EdgeVertices[be][0] == b ? m_EdgeVertices[be][1] : m_EdgeVertices[be][0];
            if ( x == a || x == opposite[0] || x == opposite[1] )
                continue;
            if ( FindEdge( a, x ) != InvalidId )
                return EditResult::CollapseBreaksTopology;
        }

        // An interior edge whose ends are both on the boundary spans the surface: collapsing it pinches the
        // two boundary stretches into one vertex, a bowtie.
        if ( tris[1] != InvalidId && IsBoundaryVertex( a ) && IsBoundaryVertex( b ) )
            return EditResult::CollapseBreaksTopology;

        for ( int side = 0; side < 2; ++side )
        {
            if ( tris[side] == InvalidId )
                continue;
            const int eac = FindEdge( a, opposite[side] );
            const int ebc = FindEdge( b, opposite[side] );
            if ( IsBoundaryEdge( eac ) && IsBoundaryEdge( ebc ) )
                return EditResult::CollapseBreaksTopology; // an ear: its two edges would merge into a wire
        }

        for ( const int bt : GetVertexTriangles( b ) )
        {
            if ( bt == tris[0] || bt == tris[1] )
                continue;
            std::array<int, 3> renamed = m_TriangleVertices[bt];
            for ( int& v : renamed )
                if ( v == b )
                    v = a;
            if ( FindTriangle( renamed[0], renamed[1], renamed[2] ) != InvalidId )
                return EditResult::CollapseBreaksTopology; // the tetrahedron case
        }

        // ── every check passed; from here on nothing is refused ──
        out               = {};
        out.KeptVertex    = a;
        out.RemovedVertex = b;
        out.CollapsedEdge = edge;

        m_Positions[a] = glm::mix( m_Positions[a], m_Positions[b], t );

        for ( int side = 0; side < 2; ++side )
        {
            const int tri = tris[side];
            if ( tri == InvalidId )
                continue;
            const int eac = FindEdge( a, opposite[side] );
            const int ebc = FindEdge( b, opposite[side] );

            // The triangle beyond (b, c) now sits on (a, c), in the place the collapsed triangle leaves.
            const int beyond = OtherTriangle( ebc, tri );
            ReplaceEdgeTriangle( eac, tri, beyond );
            if ( beyond != InvalidId )
                m_TriangleEdges[beyond][EdgeSlot( beyond, ebc )] = eac;
            RemoveEdge( ebc );
            FreeId( m_TriangleAlive, m_TriangleFree, m_TriangleLive, tri );

            out.RemovedTriangles[side] = tri;
            out.RemovedEdges[side]     = ebc;
            out.KeptEdges[side]        = eac;
        }
        RemoveEdge( edge );

        const std::vector<int> movedTriangles = GetVertexTriangles( b );
        for ( const int bt : movedTriangles )
            for ( int& v : m_TriangleVertices[bt] )
                if ( v == b )
                    v = a;
        const std::vector<int> movedEdges( m_VertexEdges[b].begin(), m_VertexEdges[b].end() );
        for ( const int be : movedEdges )
            ReplaceEdgeVertex( be, b, a );

        assert( m_VertexEdges[b].empty() );
        FreeId( m_VertexAlive, m_VertexFree, m_VertexLive, b );
        return EditResult::Ok;
    }

    CompactMaps EditMesh::Compact()
    {
        CompactMaps maps;
        const auto  buildMap = []( const std::vector<uint8_t>& alive, std::vector<int>& map )
        {
            map.assign( alive.size(), InvalidId );
            int next = 0;
            for ( size_t id = 0; id < alive.size(); ++id )
                if ( alive[id] != 0 )
                    map[id] = next++;
            return next;
        };
        const int vertexCount   = buildMap( m_VertexAlive, maps.Vertices );
        const int triangleCount = buildMap( m_TriangleAlive, maps.Triangles );
        const int edgeCount     = buildMap( m_EdgeAlive, maps.Edges );

        const auto remap = []( const std::vector<int>& map, int id ) { return id == InvalidId ? id : map[id]; };

        std::vector<glm::vec3>        positions( vertexCount );
        std::vector<std::vector<int>> vertexEdges( vertexCount );
        for ( size_t v = 0; v < maps.Vertices.size(); ++v )
        {
            const int nv = maps.Vertices[v];
            if ( nv == InvalidId )
                continue;
            positions[nv] = m_Positions[v];
            for ( const int e : m_VertexEdges[v] )
                vertexEdges[nv].push_back( maps.Edges[e] );
        }

        std::vector<std::array<int, 3>> triangleVertices( triangleCount );
        std::vector<std::array<int, 3>> triangleEdges( triangleCount );
        for ( size_t t = 0; t < maps.Triangles.size(); ++t )
        {
            const int nt = maps.Triangles[t];
            if ( nt == InvalidId )
                continue;
            for ( int j = 0; j < 3; ++j )
            {
                triangleVertices[nt][j] = maps.Vertices[m_TriangleVertices[t][j]];
                triangleEdges[nt][j]    = maps.Edges[m_TriangleEdges[t][j]];
            }
        }

        std::vector<std::array<int, 2>> edgeVertices( edgeCount );
        std::vector<std::array<int, 2>> edgeTriangles( edgeCount );
        for ( size_t e = 0; e < maps.Edges.size(); ++e )
        {
            const int ne = maps.Edges[e];
            if ( ne == InvalidId )
                continue;
            // The vertex map is monotonic, so the pair stays ascending.
            edgeVertices[ne]  = { maps.Vertices[m_EdgeVertices[e][0]], maps.Vertices[m_EdgeVertices[e][1]] };
            edgeTriangles[ne] = { remap( maps.Triangles, m_EdgeTriangles[e][0] ),
                                  remap( maps.Triangles, m_EdgeTriangles[e][1] ) };
        }

        m_Positions        = std::move( positions );
        m_VertexEdges      = std::move( vertexEdges );
        m_TriangleVertices = std::move( triangleVertices );
        m_TriangleEdges    = std::move( triangleEdges );
        m_EdgeVertices     = std::move( edgeVertices );
        m_EdgeTriangles    = std::move( edgeTriangles );
        m_VertexAlive.assign( vertexCount, 1 );
        m_TriangleAlive.assign( triangleCount, 1 );
        m_EdgeAlive.assign( edgeCount, 1 );
        m_VertexFree.clear();
        m_TriangleFree.clear();
        m_EdgeFree.clear();
        return maps;
    }

    // ── queries ──────────────────────────────────────────────────────────────────────────────────────

    const glm::vec3& EditMesh::GetPosition( int v ) const
    {
        assert( IsVertex( v ) );
        return m_Positions[v];
    }

    void EditMesh::SetPosition( int v, const glm::vec3& position )
    {
        assert( IsVertex( v ) );
        m_Positions[v] = position;
    }

    const std::array<int, 3>& EditMesh::GetTriangle( int t ) const
    {
        assert( IsTriangle( t ) );
        return m_TriangleVertices[t];
    }

    const std::array<int, 3>& EditMesh::GetTriangleEdges( int t ) const
    {
        assert( IsTriangle( t ) );
        return m_TriangleEdges[t];
    }

    const std::array<int, 2>& EditMesh::GetEdgeVertices( int e ) const
    {
        assert( IsEdge( e ) );
        return m_EdgeVertices[e];
    }

    const std::array<int, 2>& EditMesh::GetEdgeTriangles( int e ) const
    {
        assert( IsEdge( e ) );
        return m_EdgeTriangles[e];
    }

    std::span<const int> EditMesh::GetVertexEdges( int v ) const
    {
        assert( IsVertex( v ) );
        return m_VertexEdges[v];
    }

    std::vector<int> EditMesh::GetVertexTriangles( int v ) const
    {
        std::vector<int> result;
        for ( const int e : m_VertexEdges[v] )
            for ( const int t : m_EdgeTriangles[e] )
                if ( t != InvalidId )
                    result.push_back( t );
        std::sort( result.begin(), result.end() );
        result.erase( std::unique( result.begin(), result.end() ), result.end() );
        return result;
    }

    std::vector<int> EditMesh::GetVertexNeighbours( int v ) const
    {
        std::vector<int> result;
        result.reserve( m_VertexEdges[v].size() );
        for ( const int e : m_VertexEdges[v] )
            result.push_back( m_EdgeVertices[e][0] == v ? m_EdgeVertices[e][1] : m_EdgeVertices[e][0] );
        return result;
    }

    int EditMesh::FindEdge( int a, int b ) const
    {
        if ( !IsVertex( a ) || !IsVertex( b ) || a == b )
            return InvalidId;
        const std::array<int, 2> key = Sorted( a, b );
        for ( const int e : m_VertexEdges[a] )
            if ( m_EdgeVertices[e] == key )
                return e;
        return InvalidId;
    }

    int EditMesh::FindTriangle( int a, int b, int c ) const
    {
        const int e = FindEdge( a, b );
        if ( e == InvalidId )
            return InvalidId;
        for ( const int t : m_EdgeTriangles[e] )
        {
            if ( t == InvalidId )
                continue;
            const auto& corners = m_TriangleVertices[t];
            if ( corners[0] == c || corners[1] == c || corners[2] == c )
                return t;
        }
        return InvalidId;
    }

    bool EditMesh::IsBoundaryEdge( int e ) const
    {
        assert( IsEdge( e ) );
        return m_EdgeTriangles[e][1] == InvalidId;
    }

    bool EditMesh::IsBoundaryVertex( int v ) const
    {
        assert( IsVertex( v ) );
        for ( const int e : m_VertexEdges[v] )
            if ( m_EdgeTriangles[e][1] == InvalidId )
                return true;
        return false;
    }

    bool EditMesh::IsBowtieVertex( int v ) const
    {
        assert( IsVertex( v ) );
        const std::vector<int> triangles = GetVertexTriangles( v );
        if ( triangles.empty() )
            return false;

        // Walk one fan across v's interior edges; a manifold vertex reaches every triangle around it.
        std::vector<int> reached{ triangles[0] };
        for ( size_t i = 0; i < reached.size(); ++i )
        {
            const int t = reached[i];
            for ( const int e : m_TriangleEdges[t] )
            {
                if ( m_EdgeVertices[e][0] != v && m_EdgeVertices[e][1] != v )
                    continue;
                const int other = OtherTriangle( e, t );
                if ( other != InvalidId && std::find( reached.begin(), reached.end(), other ) == reached.end() )
                    reached.push_back( other );
            }
        }
        return reached.size() != triangles.size();
    }

    bool EditMesh::IsManifold() const
    {
        for ( const int v : VertexIds() )
            if ( IsBowtieVertex( v ) )
                return false;
        return true;
    }

    Common::BoolResultStr EditMesh::CheckValidity() const
    {
        using Common::MakeFormattedError;

        const auto checkPool = []( const char* kind, const std::vector<uint8_t>& alive, const std::vector<int>& freeList,
                                   int liveCount, size_t payloadSize ) -> Common::BoolResultStr
        {
            if ( payloadSize != alive.size() )
                return MakeFormattedError<bool>( "{}: {} payload slots for {} IDs", kind, payloadSize, alive.size() );
            const auto live = static_cast<int>( std::count( alive.begin(), alive.end(), uint8_t( 1 ) ) );
            if ( live != liveCount )
                return MakeFormattedError<bool>( "{}: {} live IDs but the count says {}", kind, live, liveCount );
            if ( static_cast<size_t>( live ) + freeList.size() != alive.size() )
                return MakeFormattedError<bool>( "{}: {} live + {} free != {} IDs", kind, live, freeList.size(),
                                                 alive.size() );
            std::vector<uint8_t> seen( alive.size(), 0 );
            for ( const int id : freeList )
            {
                if ( id < 0 || id >= static_cast<int>( alive.size() ) || alive[id] != 0 || seen[id] != 0 )
                    return MakeFormattedError<bool>( "{}: free list holds {} which is out of range, live or repeated",
                                                     kind, id );
                seen[id] = 1;
            }
            return Common::MakeSuccess( true );
        };

        if ( auto r = checkPool( "vertex", m_VertexAlive, m_VertexFree, m_VertexLive, m_Positions.size() );
             !r.IsSuccess() )
            return r;
        if ( m_VertexEdges.size() != m_VertexAlive.size() )
            return MakeFormattedError<bool>( "vertex: {} edge lists for {} IDs", m_VertexEdges.size(),
                                             m_VertexAlive.size() );
        if ( auto r = checkPool( "triangle", m_TriangleAlive, m_TriangleFree, m_TriangleLive,
                                 m_TriangleVertices.size() );
             !r.IsSuccess() )
            return r;
        if ( m_TriangleEdges.size() != m_TriangleAlive.size() )
            return MakeFormattedError<bool>( "triangle: {} edge triples for {} IDs", m_TriangleEdges.size(),
                                             m_TriangleAlive.size() );
        if ( auto r = checkPool( "edge", m_EdgeAlive, m_EdgeFree, m_EdgeLive, m_EdgeVertices.size() );
             !r.IsSuccess() )
            return r;
        if ( m_EdgeTriangles.size() != m_EdgeAlive.size() )
            return MakeFormattedError<bool>( "edge: {} triangle pairs for {} IDs", m_EdgeTriangles.size(),
                                             m_EdgeAlive.size() );

        // Triangles: live distinct corners, and edge j is exactly (corner j, corner j+1) and knows t.
        std::vector<std::array<int, 3>> vertexSets;
        for ( const int t : TriangleIds() )
        {
            const auto& corners = m_TriangleVertices[t];
            for ( const int v : corners )
                if ( !IsVertex( v ) )
                    return MakeFormattedError<bool>( "triangle {} names dead vertex {}", t, v );
            if ( corners[0] == corners[1] || corners[1] == corners[2] || corners[2] == corners[0] )
                return MakeFormattedError<bool>( "triangle {} is degenerate ({}, {}, {})", t, corners[0], corners[1],
                                                 corners[2] );
            for ( int j = 0; j < 3; ++j )
            {
                const int e = m_TriangleEdges[t][j];
                if ( !IsEdge( e ) )
                    return MakeFormattedError<bool>( "triangle {} edge slot {} names dead edge {}", t, j, e );
                if ( m_EdgeVertices[e] != Sorted( corners[j], corners[Next( j )] ) )
                    return MakeFormattedError<bool>( "triangle {} edge slot {} is edge {} = ({}, {}), expected ({}, {})",
                                                     t, j, e, m_EdgeVertices[e][0], m_EdgeVertices[e][1], corners[j],
                                                     corners[Next( j )] );
                if ( m_EdgeTriangles[e][0] != t && m_EdgeTriangles[e][1] != t )
                    return MakeFormattedError<bool>( "triangle {} uses edge {} which does not list it", t, e );
            }
            std::array<int, 3> key = corners;
            std::sort( key.begin(), key.end() );
            vertexSets.push_back( key );
        }
        std::sort( vertexSets.begin(), vertexSets.end() );
        if ( const auto dup = std::adjacent_find( vertexSets.begin(), vertexSets.end() ); dup != vertexSets.end() )
            return MakeFormattedError<bool>( "two triangles span the same vertices ({}, {}, {})", ( *dup )[0],
                                             ( *dup )[1], ( *dup )[2] );

        // Edges: ascending live ends that list the edge; 1-2 live triangles that own it, in opposite windings.
        for ( const int e : EdgeIds() )
        {
            const auto& ends = m_EdgeVertices[e];
            if ( !IsVertex( ends[0] ) || !IsVertex( ends[1] ) || ends[0] >= ends[1] )
                return MakeFormattedError<bool>( "edge {} has ends ({}, {}): not two live ascending vertices", e,
                                                 ends[0], ends[1] );
            for ( const int v : ends )
                if ( std::count( m_VertexEdges[v].begin(), m_VertexEdges[v].end(), e ) != 1 )
                    return MakeFormattedError<bool>( "edge {} is not listed exactly once by its end {}", e, v );
            const auto& tris = m_EdgeTriangles[e];
            if ( !IsTriangle( tris[0] ) )
                return MakeFormattedError<bool>( "edge {} has no live first triangle ({})", e, tris[0] );
            if ( tris[1] != InvalidId && ( !IsTriangle( tris[1] ) || tris[1] == tris[0] ) )
                return MakeFormattedError<bool>( "edge {} second triangle {} is dead or repeats the first", e, tris[1] );
            int firstCorner = InvalidId;
            for ( int side = 0; side < 2; ++side )
            {
                const int t = tris[side];
                if ( t == InvalidId )
                    continue;
                const auto slot = std::find( m_TriangleEdges[t].begin(), m_TriangleEdges[t].end(), e );
                if ( slot == m_TriangleEdges[t].end() )
                    return MakeFormattedError<bool>( "edge {} lists triangle {} which does not use it", e, t );
                const int corner = m_TriangleVertices[t][slot - m_TriangleEdges[t].begin()];
                if ( side == 1 && corner == firstCorner )
                    return MakeFormattedError<bool>( "edge {}: triangles {} and {} both run it from vertex {}", e,
                                                     tris[0], t, corner );
                firstCorner = corner;
            }
        }

        // Vertices: every listed edge is live, touches v, and leads to a distinct neighbour; dead vertices
        // hold nothing.
        for ( size_t id = 0; id < m_VertexAlive.size(); ++id )
        {
            const int   v     = static_cast<int>( id );
            const auto& edges = m_VertexEdges[v];
            if ( m_VertexAlive[v] == 0 )
            {
                if ( !edges.empty() )
                    return MakeFormattedError<bool>( "dead vertex {} still lists {} edges", v, edges.size() );
                continue;
            }
            std::vector<int> neighbours;
            for ( const int e : edges )
            {
                if ( !IsEdge( e ) || ( m_EdgeVertices[e][0] != v && m_EdgeVertices[e][1] != v ) )
                    return MakeFormattedError<bool>( "vertex {} lists edge {} which is dead or does not touch it", v, e );
                neighbours.push_back( m_EdgeVertices[e][0] == v ? m_EdgeVertices[e][1] : m_EdgeVertices[e][0] );
            }
            std::sort( neighbours.begin(), neighbours.end() );
            if ( const auto dup = std::adjacent_find( neighbours.begin(), neighbours.end() ); dup != neighbours.end() )
                return MakeFormattedError<bool>( "vertices {} and {} are joined by more than one edge", v, *dup );
        }

        return Common::MakeSuccess( true );
    }
} // namespace Desert::Geometry
