#include "EditMeshSelection.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Desert::Geometry
{
    const char* ToString( ElementMode mode )
    {
        switch ( mode )
        {
            case ElementMode::Vertex:
                return "Vertex";
            case ElementMode::Edge:
                return "Edge";
            case ElementMode::Triangle:
                return "Triangle";
            case ElementMode::PolyGroup:
                return "PolyGroup";
        }
        return "Unknown";
    }

    bool ProjectToViewport( const glm::vec3& world, const glm::mat4& viewProj, const glm::vec2& viewportPos,
                            const glm::vec2& viewportSize, glm::vec2& outPixels )
    {
        const glm::vec4 clip = viewProj * glm::vec4( world, 1.0f );
        if ( clip.w <= 1e-4f )
            return false;
        const glm::vec2 ndc = glm::vec2( clip ) / clip.w;
        outPixels.x         = viewportPos.x + ( ndc.x * 0.5f + 0.5f ) * viewportSize.x;
        outPixels.y         = viewportPos.y + ( 0.5f - ndc.y * 0.5f ) * viewportSize.y;
        return true;
    }

    namespace
    {
        constexpr float kNoHit = std::numeric_limits<float>::infinity();

        // Möller-Trumbore, both sides. The ray parameter on a hit in front of the origin.
        bool RayTriangle( const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b,
                          const glm::vec3& c, float& outT )
        {
            const glm::vec3 e1  = b - a;
            const glm::vec3 e2  = c - a;
            const glm::vec3 p   = glm::cross( d, e2 );
            const float     det = glm::dot( e1, p );
            if ( std::abs( det ) < 1e-12f )
                return false;
            const float     inv = 1.0f / det;
            const glm::vec3 tv  = o - a;
            const float     u   = glm::dot( tv, p ) * inv;
            if ( u < 0.0f || u > 1.0f )
                return false;
            const glm::vec3 q = glm::cross( tv, e1 );
            const float     v = glm::dot( d, q ) * inv;
            if ( v < 0.0f || u + v > 1.0f )
                return false;
            outT = glm::dot( e2, q ) * inv;
            return outT > 0.0f;
        }

        glm::vec3 WorldPosition( const EditMesh& mesh, const PickView& view, int v )
        {
            return glm::vec3( view.LocalToWorld * glm::vec4( mesh.GetPosition( v ), 1.0f ) );
        }

        // Nearest triangle the ray crosses: its ID and ray parameter (kNoHit when none).
        std::pair<int, float> RayCast( const EditMesh& mesh, const PickView& view )
        {
            int   best  = InvalidId;
            float bestT = kNoHit;
            for ( const int t : mesh.TriangleIds() )
            {
                const auto& tri = mesh.GetTriangle( t );
                float       hitT;
                if ( RayTriangle( view.RayOrigin, view.RayDirection, WorldPosition( mesh, view, tri[0] ),
                                  WorldPosition( mesh, view, tri[1] ), WorldPosition( mesh, view, tri[2] ),
                                  hitT ) &&
                     hitT < bestT )
                {
                    bestT = hitT;
                    best  = t;
                }
            }
            return { best, bestT };
        }

        // A point is hidden when a triangle crosses its OWN line of sight in front of it. Not "farther along
        // the cursor ray than the first surface": that compares the point's projection onto a ray it is not
        // on, and an edge seen a few pixels inside its own face projects past that face. The slack is
        // relative so the triangles the point lies on do not hide it, at any distance.
        bool Visible( const EditMesh& mesh, const PickView& view, const glm::vec3& point )
        {
            const glm::vec3 toPoint = point - view.RayOrigin;
            const float     dist    = glm::length( toPoint );
            if ( dist <= 0.0f )
                return true;
            const glm::vec3 dir   = toPoint / dist;
            const float     limit = dist - ( 1e-4f * dist + 1e-2f );
            for ( const int t : mesh.TriangleIds() )
            {
                const auto& tri = mesh.GetTriangle( t );
                float       hitT;
                if ( RayTriangle( view.RayOrigin, dir, WorldPosition( mesh, view, tri[0] ),
                                  WorldPosition( mesh, view, tri[1] ), WorldPosition( mesh, view, tri[2] ),
                                  hitT ) &&
                     hitT < limit )
                    return false;
            }
            return true;
        }

        float SegmentDistance( const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, float& outS )
        {
            const glm::vec2 ab  = b - a;
            const float     len = glm::dot( ab, ab );
            outS                = len > 0.0f ? std::clamp( glm::dot( p - a, ab ) / len, 0.0f, 1.0f ) : 0.0f;
            return glm::length( p - ( a + outS * ab ) );
        }

        struct Candidate
        {
            ElementHit Hit;
            glm::vec3  Point;
        };

        // Nearest to the cursor first (ties: nearer along the ray); the first one nothing hides wins. Only
        // the candidates inside the tolerance pay for an occlusion cast.
        ElementHit FirstVisible( const EditMesh& mesh, const PickView& view, std::vector<Candidate>& candidates )
        {
            std::sort( candidates.begin(), candidates.end(),
                       []( const Candidate& l, const Candidate& r )
                       {
                           return l.Hit.PixelDistance != r.Hit.PixelDistance
                                       ? l.Hit.PixelDistance < r.Hit.PixelDistance
                                       : l.Hit.RayT < r.Hit.RayT;
                       } );
            for ( const Candidate& c : candidates )
                if ( Visible( mesh, view, c.Point ) )
                    return c.Hit;
            return {};
        }

        ElementHit PickVertex( const EditMesh& mesh, const PickView& view )
        {
            std::vector<Candidate> candidates;
            for ( const int v : mesh.VertexIds() )
            {
                const glm::vec3 w = WorldPosition( mesh, view, v );
                glm::vec2       px;
                if ( !ProjectToViewport( w, view.ViewProj, view.ViewportPos, view.ViewportSize, px ) )
                    continue;
                const float dist  = glm::length( px - view.Cursor );
                const float depth = glm::dot( w - view.RayOrigin, view.RayDirection );
                if ( dist <= view.TolerancePixels )
                    candidates.push_back( { { v, depth, dist }, w } );
            }
            return FirstVisible( mesh, view, candidates );
        }

        ElementHit PickEdge( const EditMesh& mesh, const PickView& view )
        {
            std::vector<Candidate> candidates;
            for ( const int e : mesh.EdgeIds() )
            {
                const auto&     ev = mesh.GetEdgeVertices( e );
                const glm::vec3 a  = WorldPosition( mesh, view, ev[0] );
                const glm::vec3 b  = WorldPosition( mesh, view, ev[1] );
                const glm::vec4 ca = view.ViewProj * glm::vec4( a, 1.0f );
                const glm::vec4 cb = view.ViewProj * glm::vec4( b, 1.0f );
                glm::vec2       pa, pb;
                if ( !ProjectToViewport( a, view.ViewProj, view.ViewportPos, view.ViewportSize, pa ) ||
                     !ProjectToViewport( b, view.ViewProj, view.ViewportPos, view.ViewportSize, pb ) )
                    continue;
                float       s;
                const float dist = SegmentDistance( view.Cursor, pa, pb, s );
                if ( dist > view.TolerancePixels )
                    continue;
                // The screen parameter is not the edge's: undo the perspective divide to find the 3D point.
                const float     sw    = ( s / cb.w ) / ( ( 1.0f - s ) / ca.w + s / cb.w );
                const glm::vec3 point = a + sw * ( b - a );
                const float     depth = glm::dot( point - view.RayOrigin, view.RayDirection );
                candidates.push_back( { { e, depth, dist }, point } );
            }
            return FirstVisible( mesh, view, candidates );
        }

        // ── conversion steps: one mode up or down ────────────────────────────────────────────────────────

        std::unordered_map<int, std::vector<int>> TrianglesByGroup( const EditMesh& mesh )
        {
            std::unordered_map<int, std::vector<int>> groups;
            for ( const int t : mesh.TriangleIds() )
                groups[mesh.Attributes().GetPolyGroup( t )].push_back( t );
            return groups;
        }

        std::vector<char> Mask( int size, std::span<const int> ids )
        {
            std::vector<char> mask( static_cast<size_t>( size ), 0 );
            for ( const int id : ids )
                mask[id] = 1;
            return mask;
        }

        std::vector<int> SortedUnique( std::vector<int> ids )
        {
            std::sort( ids.begin(), ids.end() );
            ids.erase( std::unique( ids.begin(), ids.end() ), ids.end() );
            return ids;
        }

        // Finer: every part of every selected element.
        std::vector<int> StepDown( const EditMesh& mesh, ElementMode from, std::span<const int> ids )
        {
            std::vector<int> out;
            switch ( from )
            {
                case ElementMode::Edge:
                    for ( const int e : ids )
                        for ( const int v : mesh.GetEdgeVertices( e ) )
                            out.push_back( v );
                    break;
                case ElementMode::Triangle:
                    for ( const int t : ids )
                        for ( const int e : mesh.GetTriangleEdges( t ) )
                            out.push_back( e );
                    break;
                case ElementMode::PolyGroup:
                {
                    const std::unordered_set<int> wanted( ids.begin(), ids.end() );
                    for ( const int t : mesh.TriangleIds() )
                        if ( wanted.count( mesh.Attributes().GetPolyGroup( t ) ) )
                            out.push_back( t );
                    break;
                }
                case ElementMode::Vertex:
                    break;
            }
            return SortedUnique( std::move( out ) );
        }

        // Coarser: every element all of whose parts are selected.
        std::vector<int> StepUp( const EditMesh& mesh, ElementMode from, std::span<const int> ids )
        {
            std::vector<int> out;
            switch ( from )
            {
                case ElementMode::Vertex:
                {
                    const auto mask = Mask( mesh.MaxVertexId(), ids );
                    for ( const int e : mesh.EdgeIds() )
                    {
                        const auto& ev = mesh.GetEdgeVertices( e );
                        if ( mask[ev[0]] && mask[ev[1]] )
                            out.push_back( e );
                    }
                    break;
                }
                case ElementMode::Edge:
                {
                    const auto mask = Mask( mesh.MaxEdgeId(), ids );
                    for ( const int t : mesh.TriangleIds() )
                    {
                        const auto& te = mesh.GetTriangleEdges( t );
                        if ( mask[te[0]] && mask[te[1]] && mask[te[2]] )
                            out.push_back( t );
                    }
                    break;
                }
                case ElementMode::Triangle:
                {
                    const auto mask = Mask( mesh.MaxTriangleId(), ids );
                    for ( const auto& [group, tris] : TrianglesByGroup( mesh ) )
                        if ( std::all_of( tris.begin(), tris.end(), [&]( int t ) { return mask[t] != 0; } ) )
                            out.push_back( group );
                    break;
                }
                case ElementMode::PolyGroup:
                    break;
            }
            return SortedUnique( std::move( out ) );
        }

        // The vertices each element of `mode` stands on.
        template <typename Fn>
        void ForEachElementVertex( const EditMesh& mesh, ElementMode mode, int id, Fn&& fn )
        {
            switch ( mode )
            {
                case ElementMode::Vertex:
                    fn( id );
                    break;
                case ElementMode::Edge:
                    for ( const int v : mesh.GetEdgeVertices( id ) )
                        fn( v );
                    break;
                case ElementMode::Triangle:
                    for ( const int v : mesh.GetTriangle( id ) )
                        fn( v );
                    break;
                case ElementMode::PolyGroup:
                    break; // groups are resolved through their triangles by the callers
            }
        }

        // Every element of `mode` with at least one vertex in `vertexMask`.
        std::vector<int> ElementsTouching( const EditMesh& mesh, ElementMode mode,
                                           const std::vector<char>& vertexMask )
        {
            std::vector<int> out;
            const auto       touches = [&]( ElementMode m, int id )
            {
                bool hit = false;
                ForEachElementVertex( mesh, m, id, [&]( int v ) { hit = hit || vertexMask[v] != 0; } );
                return hit;
            };
            switch ( mode )
            {
                case ElementMode::Vertex:
                    for ( const int v : mesh.VertexIds() )
                        if ( vertexMask[v] )
                            out.push_back( v );
                    break;
                case ElementMode::Edge:
                    for ( const int e : mesh.EdgeIds() )
                        if ( touches( ElementMode::Edge, e ) )
                            out.push_back( e );
                    break;
                case ElementMode::Triangle:
                    for ( const int t : mesh.TriangleIds() )
                        if ( touches( ElementMode::Triangle, t ) )
                            out.push_back( t );
                    break;
                case ElementMode::PolyGroup:
                    for ( const int t : mesh.TriangleIds() )
                        if ( touches( ElementMode::Triangle, t ) )
                            out.push_back( mesh.Attributes().GetPolyGroup( t ) );
                    break;
            }
            return SortedUnique( std::move( out ) );
        }

        ElementSelection Build( const EditMesh& mesh, ElementMode mode, std::span<const int> ids )
        {
            ElementSelection out( mode );
            for ( const int id : ids )
            {
                // Every ID here was derived from the live mesh, so Add cannot refuse it.
                const auto added = out.Add( mesh, id );
                (void)added;
            }
            return out;
        }

        std::vector<char> VerticesOf( const EditMesh& mesh, const ElementSelection& selection )
        {
            const ElementSelection verts = ConvertSelection( mesh, selection, ElementMode::Vertex );
            return Mask( mesh.MaxVertexId(), verts.Ids() );
        }
    } // namespace

    ElementHit PickElement( const EditMesh& mesh, ElementMode mode, const PickView& view )
    {
        switch ( mode )
        {
            case ElementMode::Triangle:
            case ElementMode::PolyGroup:
            {
                const auto [triangle, rayT] = RayCast( mesh, view );
                if ( triangle == InvalidId )
                    return {};
                const int id =
                     mode == ElementMode::Triangle ? triangle : mesh.Attributes().GetPolyGroup( triangle );
                return { id, rayT, 0.0f };
            }
            case ElementMode::Vertex:
                return PickVertex( mesh, view );
            case ElementMode::Edge:
                return PickEdge( mesh, view );
        }
        return {};
    }

    // ── ElementSelection ─────────────────────────────────────────────────────────────────────────────────

    bool ElementSelection::Exists( const EditMesh& mesh, ElementMode mode, int id )
    {
        switch ( mode )
        {
            case ElementMode::Vertex:
                return mesh.IsVertex( id );
            case ElementMode::Edge:
                return mesh.IsEdge( id );
            case ElementMode::Triangle:
                return mesh.IsTriangle( id );
            case ElementMode::PolyGroup:
                for ( const int t : mesh.TriangleIds() )
                    if ( mesh.Attributes().GetPolyGroup( t ) == id )
                        return true;
                return false;
        }
        return false;
    }

    ElementSelection::Key ElementSelection::KeyOf( const EditMesh& mesh, ElementMode mode, int id )
    {
        switch ( mode )
        {
            case ElementMode::Edge:
            {
                const auto& ev = mesh.GetEdgeVertices( id );
                return { ev[0], ev[1], InvalidId };
            }
            case ElementMode::Triangle:
                return mesh.GetTriangle( id );
            case ElementMode::Vertex:
            case ElementMode::PolyGroup:
                break;
        }
        return { InvalidId, InvalidId, InvalidId };
    }

    bool ElementSelection::Contains( int id ) const
    {
        return std::binary_search( m_Ids.begin(), m_Ids.end(), id );
    }

    Common::BoolResultStr ElementSelection::Add( const EditMesh& mesh, int id )
    {
        if ( !Exists( mesh, m_Mode, id ) )
            return Common::MakeFormattedError<bool>( "ElementSelection: the mesh has no {} {}", ToString( m_Mode ),
                                                     id );
        const auto it = std::lower_bound( m_Ids.begin(), m_Ids.end(), id );
        if ( it != m_Ids.end() && *it == id )
            return Common::MakeSuccess( true );
        const auto at = it - m_Ids.begin();
        m_Ids.insert( it, id );
        m_Keys.insert( m_Keys.begin() + at, KeyOf( mesh, m_Mode, id ) );
        return Common::MakeSuccess( true );
    }

    bool ElementSelection::Remove( int id )
    {
        const auto it = std::lower_bound( m_Ids.begin(), m_Ids.end(), id );
        if ( it == m_Ids.end() || *it != id )
            return false;
        m_Keys.erase( m_Keys.begin() + ( it - m_Ids.begin() ) );
        m_Ids.erase( it );
        return true;
    }

    Common::BoolResultStr ElementSelection::Toggle( const EditMesh& mesh, int id )
    {
        if ( Remove( id ) )
            return Common::MakeSuccess( true );
        return Add( mesh, id );
    }

    void ElementSelection::Clear()
    {
        m_Ids.clear();
        m_Keys.clear();
    }

    PruneReport ElementSelection::Prune( const EditMesh& mesh )
    {
        PruneReport      report;
        std::vector<int> ids;
        std::vector<Key> keys;
        for ( size_t i = 0; i < m_Ids.size(); ++i )
        {
            if ( !Exists( mesh, m_Mode, m_Ids[i] ) )
                ++report.Missing;
            else if ( KeyOf( mesh, m_Mode, m_Ids[i] ) != m_Keys[i] )
                ++report.Changed;
            else
            {
                ids.push_back( m_Ids[i] );
                keys.push_back( m_Keys[i] );
            }
        }
        m_Ids  = std::move( ids );
        m_Keys = std::move( keys );
        return report;
    }

    PruneReport ElementSelection::Remap( const CompactMaps& maps )
    {
        const std::vector<int>* map = nullptr;
        switch ( m_Mode )
        {
            case ElementMode::Vertex:
                map = &maps.Vertices;
                break;
            case ElementMode::Edge:
                map = &maps.Edges;
                break;
            case ElementMode::Triangle:
                map = &maps.Triangles;
                break;
            case ElementMode::PolyGroup:
                return {};
        }
        const auto renumber = [&]( int id )
        { return id >= 0 && id < static_cast<int>( map->size() ) ? ( *map )[id] : InvalidId; };

        PruneReport                      report;
        std::vector<std::pair<int, Key>> kept;
        for ( size_t i = 0; i < m_Ids.size(); ++i )
        {
            const int id = renumber( m_Ids[i] );
            if ( id == InvalidId )
            {
                ++report.Missing;
                continue;
            }
            Key key = m_Keys[i];
            // A key names vertices; they were renumbered too.
            if ( m_Mode != ElementMode::Vertex )
                for ( int& v : key )
                    if ( v != InvalidId )
                        v = v < static_cast<int>( maps.Vertices.size() ) ? maps.Vertices[v] : InvalidId;
            kept.emplace_back( id, key );
        }
        std::sort( kept.begin(), kept.end(), []( const auto& l, const auto& r ) { return l.first < r.first; } );
        m_Ids.clear();
        m_Keys.clear();
        for ( const auto& [id, key] : kept )
        {
            m_Ids.push_back( id );
            m_Keys.push_back( key );
        }
        return report;
    }

    // ── operations ───────────────────────────────────────────────────────────────────────────────────────

    ElementSelection ConvertSelection( const EditMesh& mesh, const ElementSelection& selection,
                                       ElementMode target )
    {
        ElementMode      mode = selection.Mode();
        std::vector<int> ids( selection.Ids().begin(), selection.Ids().end() );
        while ( mode > target )
        {
            ids  = StepDown( mesh, mode, ids );
            mode = static_cast<ElementMode>( static_cast<int>( mode ) - 1 );
        }
        while ( mode < target )
        {
            ids  = StepUp( mesh, mode, ids );
            mode = static_cast<ElementMode>( static_cast<int>( mode ) + 1 );
        }
        return Build( mesh, target, ids );
    }

    ElementSelection SelectConnected( const EditMesh& mesh, const ElementSelection& selection )
    {
        std::vector<char> inPiece = VerticesOf( mesh, selection );
        std::vector<int>  stack;
        for ( const int v : mesh.VertexIds() )
            if ( inPiece[v] )
                stack.push_back( v );
        while ( !stack.empty() )
        {
            const int v = stack.back();
            stack.pop_back();
            for ( const int n : mesh.GetVertexNeighbours( v ) )
                if ( !inPiece[n] )
                {
                    inPiece[n] = 1;
                    stack.push_back( n );
                }
        }
        std::vector<int> verts;
        for ( const int v : mesh.VertexIds() )
            if ( inPiece[v] )
                verts.push_back( v );
        return ConvertSelection( mesh, Build( mesh, ElementMode::Vertex, verts ), selection.Mode() );
    }

    ElementSelection GrowSelection( const EditMesh& mesh, const ElementSelection& selection )
    {
        std::vector<char> seed = VerticesOf( mesh, selection );
        if ( selection.Mode() == ElementMode::Vertex )
            for ( const int v : selection.Ids() )
                for ( const int n : mesh.GetVertexNeighbours( v ) )
                    seed[n] = 1;
        std::vector<int> ids = ElementsTouching( mesh, selection.Mode(), seed );
        ids.insert( ids.end(), selection.Ids().begin(), selection.Ids().end() );
        return Build( mesh, selection.Mode(), SortedUnique( std::move( ids ) ) );
    }

    ElementSelection ShrinkSelection( const EditMesh& mesh, const ElementSelection& selection )
    {
        const ElementMode mode = selection.Mode();
        // The vertices of every UNselected element: a selected element touching one is on the rim.
        std::vector<char> rim( static_cast<size_t>( mesh.MaxVertexId() ), 0 );
        if ( mode == ElementMode::Vertex )
        {
            for ( const int v : mesh.VertexIds() )
                if ( !selection.Contains( v ) )
                    for ( const int n : mesh.GetVertexNeighbours( v ) )
                        rim[n] = 1;
        }
        else if ( mode == ElementMode::PolyGroup )
        {
            for ( const int t : mesh.TriangleIds() )
                if ( !selection.Contains( mesh.Attributes().GetPolyGroup( t ) ) )
                    for ( const int v : mesh.GetTriangle( t ) )
                        rim[v] = 1;
        }
        else
        {
            const IdRange all = mode == ElementMode::Edge ? mesh.EdgeIds() : mesh.TriangleIds();
            for ( const int id : all )
                if ( !selection.Contains( id ) )
                    ForEachElementVertex( mesh, mode, id, [&]( int v ) { rim[v] = 1; } );
        }

        std::vector<int> kept;
        if ( mode == ElementMode::PolyGroup )
        {
            const std::vector<int> onRim = ElementsTouching( mesh, ElementMode::PolyGroup, rim );
            for ( const int g : selection.Ids() )
                if ( !std::binary_search( onRim.begin(), onRim.end(), g ) )
                    kept.push_back( g );
        }
        else
        {
            for ( const int id : selection.Ids() )
            {
                bool touches = false;
                ForEachElementVertex( mesh, mode, id, [&]( int v ) { touches = touches || rim[v] != 0; } );
                if ( !touches )
                    kept.push_back( id );
            }
        }
        return Build( mesh, mode, kept );
    }
} // namespace Desert::Geometry
