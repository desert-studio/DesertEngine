#include "EditMeshSelection.hpp"

#include "ElementSelectionAlgorithms.inl"

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

    ElementHit PickElement( const EditMesh& mesh, ElementMode mode, const PickView& view )
    {
        return PickElementT( mesh, mode, view );
    }

    Common::BoolResultStr ElementSelection::Add( const EditMesh& mesh, int id )
    {
        return AddIn( mesh, id );
    }

    Common::BoolResultStr ElementSelection::Toggle( const EditMesh& mesh, int id )
    {
        return ToggleIn( mesh, id );
    }

    PruneReport ElementSelection::Prune( const EditMesh& mesh )
    {
        return PruneIn( mesh );
    }

    bool ElementSelection::Contains( int id ) const
    {
        return std::binary_search( m_Ids.begin(), m_Ids.end(), id );
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

    void ElementSelection::Clear()
    {
        m_Ids.clear();
        m_Keys.clear();
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

    ElementSelection ConvertSelection( const EditMesh& mesh, const ElementSelection& selection,
                                       ElementMode target )
    {
        return ConvertSelectionT( mesh, selection, target );
    }

    ElementSelection SelectConnected( const EditMesh& mesh, const ElementSelection& selection )
    {
        return SelectConnectedT( mesh, selection );
    }

    ElementSelection GrowSelection( const EditMesh& mesh, const ElementSelection& selection )
    {
        return GrowSelectionT( mesh, selection );
    }

    ElementSelection ShrinkSelection( const EditMesh& mesh, const ElementSelection& selection )
    {
        return ShrinkSelectionT( mesh, selection );
    }
} // namespace Desert::Geometry
