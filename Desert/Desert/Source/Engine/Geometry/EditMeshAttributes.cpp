#include "EditMeshAttributes.hpp"

#include "EditMesh.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cassert>
#include <type_traits>

namespace Desert::Geometry
{
    namespace
    {
        constexpr std::array<int, 3> kUnset{ InvalidId, InvalidId, InvalidId };

        int Next( int j )
        {
            return ( j + 1 ) % 3;
        }

        int Prev( int j )
        {
            return ( j + 2 ) % 3;
        }

        int CornerOf( const std::array<int, 3>& corners, int v )
        {
            for ( int j = 0; j < 3; ++j )
                if ( corners[j] == v )
                    return j;
            return InvalidId;
        }

        // A zero-length blend (two opposite unit vectors at t = 0.5) has no direction; keeping the start
        // value is the choice that never produces NaN, and the next normal recompute replaces it anyway.
        template <typename V>
        V NormalisedOr( const V& v, const V& fallback )
        {
            const float len = glm::length( v );
            return len > 1e-12f ? v / len : fallback;
        }
    } // namespace

    // ── overlay ──────────────────────────────────────────────────────────────────────────────────────

    template <typename T>
    EditMeshOverlay<T>::EditMeshOverlay( OverlayInterpolation interpolation, int triangleSlots )
         : m_Interpolation( interpolation ), m_Triangles( static_cast<size_t>( triangleSlots ), kUnset )
    {
    }

    template <typename T>
    int EditMeshOverlay<T>::AppendElement( const T& value )
    {
        const int e = m_Pool.Allocate();
        if ( e == static_cast<int>( m_Values.size() ) )
        {
            m_Values.emplace_back();
            m_Parents.emplace_back();
            m_RefCounts.emplace_back();
        }
        m_Values[e]    = value;
        m_Parents[e]   = InvalidId;
        m_RefCounts[e] = 0;
        return e;
    }

    template <typename T>
    const T& EditMeshOverlay<T>::GetElement( int e ) const
    {
        assert( IsElement( e ) );
        return m_Values[e];
    }

    template <typename T>
    void EditMeshOverlay<T>::SetElement( int e, const T& value )
    {
        assert( IsElement( e ) );
        m_Values[e] = value;
    }

    template <typename T>
    int EditMeshOverlay<T>::GetParentVertex( int e ) const
    {
        assert( IsElement( e ) );
        return m_Parents[e];
    }

    template <typename T>
    bool EditMeshOverlay<T>::IsSetTriangle( int t ) const
    {
        return t >= 0 && t < static_cast<int>( m_Triangles.size() ) && m_Triangles[t][0] != InvalidId;
    }

    template <typename T>
    const std::array<int, 3>& EditMeshOverlay<T>::GetTriangle( int t ) const
    {
        assert( t >= 0 && t < static_cast<int>( m_Triangles.size() ) );
        return m_Triangles[t];
    }

    template <typename T>
    void EditMeshOverlay<T>::Bind( int e, int vertex )
    {
        assert( m_Parents[e] == InvalidId || m_Parents[e] == vertex );
        m_Parents[e] = vertex;
        ++m_RefCounts[e];
    }

    template <typename T>
    void EditMeshOverlay<T>::Drop( int e )
    {
        assert( m_RefCounts[e] > 0 );
        if ( --m_RefCounts[e] == 0 )
        {
            m_Parents[e] = InvalidId;
            m_Pool.Release( e );
        }
    }

    template <typename T>
    EditResult EditMeshOverlay<T>::SetTriangle( const EditMesh& mesh, int t, const std::array<int, 3>& elements )
    {
        if ( !mesh.IsTriangle( t ) )
            return EditResult::InvalidTriangle;
        const auto& corners = mesh.GetTriangle( t );
        for ( int j = 0; j < 3; ++j )
        {
            const int e = elements[j];
            if ( !IsElement( e ) )
                return EditResult::InvalidElement;
            if ( m_Parents[e] != InvalidId && m_Parents[e] != corners[j] )
                return EditResult::ElementOnOtherVertex;
        }
        // Two corners naming one unbound element would bind it to two vertices.
        if ( elements[0] == elements[1] || elements[1] == elements[2] || elements[2] == elements[0] )
            return EditResult::ElementOnOtherVertex;

        EnsureTriangleSlot( t );
        const std::array<int, 3> previous = m_Triangles[t];
        for ( int j = 0; j < 3; ++j )
            Bind( elements[j], corners[j] );
        m_Triangles[t] = elements;
        // Dropped AFTER binding: re-setting a triangle to the element it already uses must not free it.
        if ( previous[0] != InvalidId )
            for ( const int e : previous )
                Drop( e );
        return EditResult::Ok;
    }

    template <typename T>
    void EditMeshOverlay<T>::UnsetTriangle( int t )
    {
        if ( !IsSetTriangle( t ) )
            return;
        const std::array<int, 3> previous = m_Triangles[t];
        m_Triangles[t]                    = kUnset;
        for ( const int e : previous )
            Drop( e );
    }

    template <typename T>
    int EditMeshOverlay<T>::GetElementAtVertex( const EditMesh& mesh, int t, int v ) const
    {
        const int j = CornerOf( mesh.GetTriangle( t ), v );
        assert( j != InvalidId );
        return IsSetTriangle( t ) ? m_Triangles[t][j] : InvalidId;
    }

    template <typename T>
    bool EditMeshOverlay<T>::IsSeamEdge( const EditMesh& mesh, int e ) const
    {
        const auto& tris = mesh.GetEdgeTriangles( e );
        if ( tris[1] == InvalidId )
            return false;
        const bool set0 = IsSetTriangle( tris[0] );
        const bool set1 = IsSetTriangle( tris[1] );
        if ( set0 != set1 )
            return true;
        if ( !set0 )
            return false;
        for ( const int v : mesh.GetEdgeVertices( e ) )
            if ( GetElementAtVertex( mesh, tris[0], v ) != GetElementAtVertex( mesh, tris[1], v ) )
                return true;
        return false;
    }

    template <typename T>
    T EditMeshOverlay<T>::Lerp( const T& a, const T& b, float t ) const
    {
        const T mixed = a + ( b - a ) * t;
        if ( m_Interpolation == OverlayInterpolation::Linear )
            return mixed;
        if constexpr ( std::is_same_v<T, glm::vec4> )
        {
            // A tangent's w is the bitangent's handedness, +1 or -1: blending it would produce a frame with
            // no handedness at all, so the nearer end's sign is kept.
            const glm::vec3 xyz = NormalisedOr( glm::vec3( mixed ), glm::vec3( a ) );
            return T( xyz, t < 0.5f ? a.w : b.w );
        }
        else
            return NormalisedOr( mixed, a );
    }

    template <typename T>
    void EditMeshOverlay<T>::EnsureTriangleSlot( int t )
    {
        if ( t >= static_cast<int>( m_Triangles.size() ) )
            m_Triangles.resize( static_cast<size_t>( t ) + 1, kUnset );
    }

    template <typename T>
    void EditMeshOverlay<T>::OnTriangleAllocated( int t )
    {
        EnsureTriangleSlot( t );
        assert( !IsSetTriangle( t ) && "a freed triangle ID must have been unset when it was freed" );
        m_Triangles[t] = kUnset;
    }

    template <typename T>
    void EditMeshOverlay<T>::OnSplitEdge( const std::array<Detail::SplitSide, 2>& sides, int newVertex )
    {
        // Side 0 remembers what it made so side 1 can reuse it: when both triangles agree about the elements
        // at both ends (no seam along the edge) the new vertex gets ONE element, otherwise the split would
        // open a seam where there was none.
        int sharedP = InvalidId;
        int sharedQ = InvalidId;
        int sharedF = InvalidId;
        for ( int side = 0; side < 2; ++side )
        {
            const Detail::SplitSide& s = sides[side];
            if ( s.Triangle == InvalidId )
                continue;
            EnsureTriangleSlot( s.Created );
            if ( !IsSetTriangle( s.Triangle ) )
            {
                m_Triangles[s.Created] = kUnset;
                continue;
            }
            const int j  = s.Slot;
            const int ep = m_Triangles[s.Triangle][j];
            const int eq = m_Triangles[s.Triangle][Next( j )];
            const int eo = m_Triangles[s.Triangle][Prev( j )];

            // Side 1 runs the edge the other way, so a shared edge has its p where side 0 had its q.
            int ef = InvalidId;
            if ( side == 1 && sharedF != InvalidId && ep == sharedQ && eq == sharedP )
                ef = sharedF;
            else
                ef = AppendElement( Lerp( m_Values[ep], m_Values[eq], s.TowardQ ) );

            // (p, q, o) -> (p, f, o) in place, and the new (f, q, o): eq moves across, no net change.
            m_Triangles[s.Triangle][Next( j )] = ef;
            m_Triangles[s.Created]             = { ef, eq, eo };
            Bind( ef, newVertex );
            Bind( ef, newVertex );
            Bind( eo, m_Parents[eo] );

            if ( side == 0 )
            {
                sharedP = ep;
                sharedQ = eq;
                sharedF = ef;
            }
        }
    }

    template <typename T>
    bool EditMeshOverlay<T>::CanFlipEdge( int t0, int j0, int t1, int j1 ) const
    {
        const bool set0 = IsSetTriangle( t0 );
        const bool set1 = IsSetTriangle( t1 );
        if ( set0 != set1 )
            return false;
        if ( !set0 )
            return true;
        // t0 = (a, b, c) with the edge at j0; t1 = (b, a, d) with the edge at j1.
        return m_Triangles[t0][j0] == m_Triangles[t1][Next( j1 )] &&
               m_Triangles[t0][Next( j0 )] == m_Triangles[t1][j1];
    }

    template <typename T>
    void EditMeshOverlay<T>::OnFlipEdge( int t0, int j0, int t1, int j1 )
    {
        if ( !IsSetTriangle( t0 ) )
            return; // CanFlipEdge made sure both are unset then
        const int ea = m_Triangles[t0][j0];
        const int eb = m_Triangles[t0][Next( j0 )];
        const int ec = m_Triangles[t0][Prev( j0 )];
        const int ed = m_Triangles[t1][Prev( j1 )];
        // Same corner order as EditMesh::FlipEdge: (c, d, b) and (d, c, a).
        m_Triangles[t0] = { ec, ed, eb };
        m_Triangles[t1] = { ed, ec, ea };
        Bind( ec, m_Parents[ec] );
        Bind( ed, m_Parents[ed] );
        Drop( ea ); // was on both triangles, stays on t1
        Drop( eb ); // was on both triangles, stays on t0
    }

    template <typename T>
    bool EditMeshOverlay<T>::CanCollapseEdge( const std::array<Detail::CollapseSide, 2>& sides ) const
    {
        if ( sides[1].Triangle == InvalidId || !IsSetTriangle( sides[0].Triangle ) ||
             !IsSetTriangle( sides[1].Triangle ) )
            return true;
        const int ka0 = m_Triangles[sides[0].Triangle][sides[0].SlotKept];
        const int kb0 = m_Triangles[sides[0].Triangle][sides[0].SlotRemoved];
        const int ka1 = m_Triangles[sides[1].Triangle][sides[1].SlotKept];
        const int kb1 = m_Triangles[sides[1].Triangle][sides[1].SlotRemoved];
        // A seam END: the edge is split at one end only. Keeping one element merges the two sides of a
        // seam that continues elsewhere; keeping two would need an arbitrary split of the single one.
        // UE documents the same case as having "no sensible way to handle" it.
        const bool splitAtKept    = ka0 != ka1;
        const bool splitAtRemoved = kb0 != kb1;
        return splitAtKept == splitAtRemoved;
    }

    template <typename T>
    void EditMeshOverlay<T>::OnCollapseEdge( const std::array<Detail::CollapseSide, 2>& sides,
                                             const std::vector<int>& movedTriangles, int keptVertex,
                                             int removedVertex, float t )
    {
        // Per set side, the element pair on the collapsing edge: the kept element absorbs the removed one.
        // Two sides without a seam name the same pair, which must then be blended only once.
        std::array<std::array<int, 2>, 2> pairs{ { { InvalidId, InvalidId }, { InvalidId, InvalidId } } };
        int                               pairCount = 0;
        for ( const Detail::CollapseSide& s : sides )
        {
            if ( s.Triangle == InvalidId || !IsSetTriangle( s.Triangle ) )
                continue;
            const std::array<int, 2> pair{ m_Triangles[s.Triangle][s.SlotKept],
                                           m_Triangles[s.Triangle][s.SlotRemoved] };
            if ( pairCount == 1 && pairs[0] == pair )
                continue;
            pairs[pairCount++] = pair;
        }
        for ( int i = 0; i < pairCount; ++i )
            m_Values[pairs[i][0]] = Lerp( m_Values[pairs[i][0]], m_Values[pairs[i][1]], t );

        for ( const int mt : movedTriangles )
        {
            if ( !IsSetTriangle( mt ) )
                continue;
            for ( int& e : m_Triangles[mt] )
            {
                if ( m_Parents[e] != removedVertex )
                    continue;
                int replacement = InvalidId;
                for ( int i = 0; i < pairCount; ++i )
                    if ( pairs[i][1] == e )
                        replacement = pairs[i][0];
                if ( replacement == InvalidId )
                {
                    // An element of the removed vertex that is not on the collapsing edge (a seam through
                    // the removed vertex elsewhere) keeps its value and moves to the kept vertex.
                    m_Parents[e] = keptVertex;
                    continue;
                }
                const int old = e;
                e             = replacement;
                Bind( replacement, keptVertex );
                Drop( old );
            }
        }
        for ( const Detail::CollapseSide& s : sides )
            if ( s.Triangle != InvalidId )
                UnsetTriangle( s.Triangle );
    }

    template <typename T>
    std::vector<int> EditMeshOverlay<T>::OnCompact( const CompactMaps& maps )
    {
        std::vector<int> elementMap = m_Pool.Compact();
        const int        count      = m_Pool.Size();

        std::vector<T>   values( static_cast<size_t>( count ) );
        std::vector<int> parents( static_cast<size_t>( count ) );
        std::vector<int> refCounts( static_cast<size_t>( count ) );
        for ( size_t e = 0; e < elementMap.size(); ++e )
        {
            const int ne = elementMap[e];
            if ( ne == InvalidId )
                continue;
            values[ne]    = m_Values[e];
            parents[ne]   = m_Parents[e] == InvalidId ? InvalidId : maps.Vertices[m_Parents[e]];
            refCounts[ne] = m_RefCounts[e];
        }

        std::vector<std::array<int, 3>> triangles;
        for ( size_t t = 0; t < maps.Triangles.size(); ++t )
        {
            const int nt = maps.Triangles[t];
            if ( nt == InvalidId )
                continue;
            if ( static_cast<int>( triangles.size() ) <= nt )
                triangles.resize( static_cast<size_t>( nt ) + 1, kUnset );
            if ( t < m_Triangles.size() && m_Triangles[t][0] != InvalidId )
                for ( int j = 0; j < 3; ++j )
                    triangles[nt][j] = elementMap[m_Triangles[t][j]];
        }

        m_Values    = std::move( values );
        m_Parents   = std::move( parents );
        m_RefCounts = std::move( refCounts );
        m_Triangles = std::move( triangles );
        return elementMap;
    }

    template <typename T>
    Common::BoolResultStr EditMeshOverlay<T>::CheckValidity( const EditMesh& mesh, const char* name ) const
    {
        using Common::MakeFormattedError;

        if ( auto r = m_Pool.Check( name, m_Values.size() ); !r.IsSuccess() )
            return r;
        if ( m_Parents.size() != m_Values.size() || m_RefCounts.size() != m_Values.size() )
            return MakeFormattedError<bool>( "{}: {} parents / {} ref counts for {} elements", name,
                                             m_Parents.size(), m_RefCounts.size(), m_Values.size() );
        if ( static_cast<int>( m_Triangles.size() ) != mesh.MaxTriangleId() )
            return MakeFormattedError<bool>( "{}: {} triangle slots for {} mesh triangle IDs", name,
                                             m_Triangles.size(), mesh.MaxTriangleId() );

        std::vector<int> uses( m_Values.size(), 0 );
        for ( int t = 0; t < static_cast<int>( m_Triangles.size() ); ++t )
        {
            const auto& tri    = m_Triangles[t];
            const int   unsetN = static_cast<int>( std::count( tri.begin(), tri.end(), InvalidId ) );
            if ( unsetN != 0 && unsetN != 3 )
                return MakeFormattedError<bool>( "{}: triangle {} is partly set ({}, {}, {})", name, t, tri[0],
                                                 tri[1], tri[2] );
            if ( unsetN == 3 )
                continue;
            if ( !mesh.IsTriangle( t ) )
                return MakeFormattedError<bool>( "{}: dead triangle {} is still set", name, t );
            const auto& corners = mesh.GetTriangle( t );
            for ( int j = 0; j < 3; ++j )
            {
                const int e = tri[j];
                if ( !IsElement( e ) )
                    return MakeFormattedError<bool>( "{}: triangle {} corner {} names dead element {}", name, t, j,
                                                     e );
                if ( m_Parents[e] != corners[j] )
                    return MakeFormattedError<bool>(
                         "{}: triangle {} corner {} is vertex {} but element {} belongs to "
                         "vertex {}",
                         name, t, j, corners[j], e, m_Parents[e] );
                ++uses[e];
            }
        }
        for ( const int e : ElementIds() )
        {
            if ( uses[e] != m_RefCounts[e] )
                return MakeFormattedError<bool>( "{}: element {} is used {} times but counts {}", name, e, uses[e],
                                                 m_RefCounts[e] );
            if ( uses[e] == 0 )
                return MakeFormattedError<bool>( "{}: element {} is used by no triangle", name, e );
            if ( !mesh.IsVertex( m_Parents[e] ) )
                return MakeFormattedError<bool>( "{}: element {} belongs to dead vertex {}", name, e,
                                                 m_Parents[e] );
        }
        return Common::MakeSuccess( true );
    }

    template class EditMeshOverlay<glm::vec2>;
    template class EditMeshOverlay<glm::vec3>;
    template class EditMeshOverlay<glm::vec4>;

    // ── attribute set ────────────────────────────────────────────────────────────────────────────────

    template <typename Fn>
    void EditMeshAttributes::ForEachOverlay( Fn&& fn )
    {
        if ( m_Normals )
            fn( *m_Normals );
        if ( m_Tangents )
            fn( *m_Tangents );
        if ( m_Colors )
            fn( *m_Colors );
        for ( UVOverlay& uv : m_UVs )
            fn( uv );
    }

    template <typename Fn>
    void EditMeshAttributes::ForEachOverlay( Fn&& fn ) const
    {
        if ( m_Normals )
            fn( *m_Normals );
        if ( m_Tangents )
            fn( *m_Tangents );
        if ( m_Colors )
            fn( *m_Colors );
        for ( const UVOverlay& uv : m_UVs )
            fn( uv );
    }

    void EditMeshAttributes::EnableNormals()
    {
        if ( !m_Normals )
            m_Normals.emplace( OverlayInterpolation::UnitDirection, m_TriangleSlots );
    }
    void EditMeshAttributes::DisableNormals()
    {
        m_Normals.reset();
    }
    void EditMeshAttributes::EnableTangents()
    {
        if ( !m_Tangents )
            m_Tangents.emplace( OverlayInterpolation::UnitDirection, m_TriangleSlots );
    }
    void EditMeshAttributes::DisableTangents()
    {
        m_Tangents.reset();
    }
    void EditMeshAttributes::EnableColors()
    {
        if ( !m_Colors )
            m_Colors.emplace( OverlayInterpolation::Linear, m_TriangleSlots );
    }
    void EditMeshAttributes::DisableColors()
    {
        m_Colors.reset();
    }

    bool EditMeshAttributes::SetUVLayerCount( int count )
    {
        if ( count < 0 || count > MaxUVLayers )
            return false;
        while ( static_cast<int>( m_UVs.size() ) > count )
            m_UVs.pop_back();
        while ( static_cast<int>( m_UVs.size() ) < count )
            m_UVs.emplace_back( OverlayInterpolation::Linear, m_TriangleSlots );
        return true;
    }

    UVOverlay* EditMeshAttributes::UV( int layer )
    {
        return layer >= 0 && layer < UVLayerCount() ? &m_UVs[layer] : nullptr;
    }

    const UVOverlay* EditMeshAttributes::UV( int layer ) const
    {
        return layer >= 0 && layer < UVLayerCount() ? &m_UVs[layer] : nullptr;
    }

    int EditMeshAttributes::GetPolyGroup( int t ) const
    {
        assert( t >= 0 && t < m_TriangleSlots );
        return m_PolyGroups[t];
    }

    void EditMeshAttributes::SetPolyGroup( int t, int group )
    {
        assert( t >= 0 && t < m_TriangleSlots );
        m_PolyGroups[t] = group;
    }

    int EditMeshAttributes::GetMaterialId( int t ) const
    {
        assert( t >= 0 && t < m_TriangleSlots );
        return m_MaterialIds[t];
    }

    void EditMeshAttributes::SetMaterialId( int t, int material )
    {
        assert( t >= 0 && t < m_TriangleSlots );
        m_MaterialIds[t] = material;
    }

    bool EditMeshAttributes::IsAttributeSeamEdge( const EditMesh& mesh, int e ) const
    {
        const auto& tris = mesh.GetEdgeTriangles( e );
        if ( tris[1] == InvalidId )
            return false;
        if ( m_PolyGroups[tris[0]] != m_PolyGroups[tris[1]] || m_MaterialIds[tris[0]] != m_MaterialIds[tris[1]] )
            return true;
        bool seam = false;
        ForEachOverlay( [&]( const auto& overlay ) { seam = seam || overlay.IsSeamEdge( mesh, e ); } );
        return seam;
    }

    void EditMeshAttributes::OnTriangleAllocated( int t )
    {
        if ( t >= m_TriangleSlots )
        {
            m_TriangleSlots = t + 1;
            m_PolyGroups.resize( static_cast<size_t>( m_TriangleSlots ), 0 );
            m_MaterialIds.resize( static_cast<size_t>( m_TriangleSlots ), 0 );
        }
        m_PolyGroups[t]  = 0;
        m_MaterialIds[t] = 0;
        ForEachOverlay( [&]( auto& overlay ) { overlay.OnTriangleAllocated( t ); } );
    }

    void EditMeshAttributes::OnSplitEdge( const std::array<Detail::SplitSide, 2>& sides, int newVertex )
    {
        for ( const Detail::SplitSide& s : sides )
        {
            if ( s.Triangle == InvalidId )
                continue;
            m_PolyGroups[s.Created]  = m_PolyGroups[s.Triangle];
            m_MaterialIds[s.Created] = m_MaterialIds[s.Triangle];
        }
        ForEachOverlay( [&]( auto& overlay ) { overlay.OnSplitEdge( sides, newVertex ); } );
    }

    bool EditMeshAttributes::CanFlipEdge( int t0, int j0, int t1, int j1 ) const
    {
        // A flip moves the quad's diagonal; with a group, material or overlay boundary ON that diagonal the
        // flip would silently hand half of one side's area to the other.
        if ( m_PolyGroups[t0] != m_PolyGroups[t1] || m_MaterialIds[t0] != m_MaterialIds[t1] )
            return false;
        bool ok = true;
        ForEachOverlay( [&]( const auto& overlay ) { ok = ok && overlay.CanFlipEdge( t0, j0, t1, j1 ); } );
        return ok;
    }

    void EditMeshAttributes::OnFlipEdge( int t0, int j0, int t1, int j1 )
    {
        ForEachOverlay( [&]( auto& overlay ) { overlay.OnFlipEdge( t0, j0, t1, j1 ); } );
    }

    bool EditMeshAttributes::CanCollapseEdge( const std::array<Detail::CollapseSide, 2>& sides ) const
    {
        bool ok = true;
        ForEachOverlay( [&]( const auto& overlay ) { ok = ok && overlay.CanCollapseEdge( sides ); } );
        return ok;
    }

    void EditMeshAttributes::OnCollapseEdge( const std::array<Detail::CollapseSide, 2>& sides,
                                             const std::vector<int>& movedTriangles, int keptVertex,
                                             int removedVertex, float t )
    {
        ForEachOverlay( [&]( auto& overlay )
                        { overlay.OnCollapseEdge( sides, movedTriangles, keptVertex, removedVertex, t ); } );
    }

    void EditMeshAttributes::OnRemoveTriangle( int t )
    {
        ForEachOverlay( [&]( auto& overlay ) { overlay.UnsetTriangle( t ); } );
    }

    void EditMeshAttributes::OnCompact( CompactMaps& maps )
    {
        std::vector<int> groups;
        std::vector<int> materials;
        for ( size_t t = 0; t < maps.Triangles.size(); ++t )
        {
            const int nt = maps.Triangles[t];
            if ( nt == InvalidId )
                continue;
            groups.resize( static_cast<size_t>( std::max<int>( static_cast<int>( groups.size() ), nt + 1 ) ) );
            materials.resize( groups.size() );
            groups[nt]    = m_PolyGroups[t];
            materials[nt] = m_MaterialIds[t];
        }
        m_TriangleSlots = static_cast<int>( groups.size() );
        m_PolyGroups    = std::move( groups );
        m_MaterialIds   = std::move( materials );

        if ( m_Normals )
            maps.NormalElements = m_Normals->OnCompact( maps );
        if ( m_Tangents )
            maps.TangentElements = m_Tangents->OnCompact( maps );
        if ( m_Colors )
            maps.ColorElements = m_Colors->OnCompact( maps );
        maps.UVElements.clear();
        for ( UVOverlay& uv : m_UVs )
            maps.UVElements.push_back( uv.OnCompact( maps ) );
    }

    Common::BoolResultStr EditMeshAttributes::CheckValidity( const EditMesh& mesh ) const
    {
        if ( m_TriangleSlots != mesh.MaxTriangleId() ||
             static_cast<int>( m_PolyGroups.size() ) != m_TriangleSlots ||
             static_cast<int>( m_MaterialIds.size() ) != m_TriangleSlots )
            return Common::MakeFormattedError<bool>(
                 "attributes: {} slots, {} polygroups, {} material IDs for {} mesh "
                 "triangle IDs",
                 m_TriangleSlots, m_PolyGroups.size(), m_MaterialIds.size(), mesh.MaxTriangleId() );
        Common::BoolResultStr result = Common::MakeSuccess( true );
        const auto            check  = [&]( const auto& overlay, const char* name )
        {
            if ( result.IsSuccess() )
                result = overlay.CheckValidity( mesh, name );
        };
        if ( m_Normals )
            check( *m_Normals, "normals" );
        if ( m_Tangents )
            check( *m_Tangents, "tangents" );
        if ( m_Colors )
            check( *m_Colors, "colors" );
        static constexpr std::array<const char*, MaxUVLayers> kUVNames{ "uv0", "uv1", "uv2", "uv3",
                                                                        "uv4", "uv5", "uv6", "uv7" };
        for ( size_t i = 0; i < m_UVs.size(); ++i )
            check( m_UVs[i], kUVNames[i] );
        return result;
    }
} // namespace Desert::Geometry
