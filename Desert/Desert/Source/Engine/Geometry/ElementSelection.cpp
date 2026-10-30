#include "ElementSelection.hpp"

#include <algorithm>

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

} // namespace Desert::Geometry
