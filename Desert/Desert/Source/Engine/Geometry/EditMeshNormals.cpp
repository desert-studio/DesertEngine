#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace Desert::Geometry
{
    glm::vec3 TriangleNormal( const EditMesh& mesh, int t )
    {
        const auto&     c   = mesh.GetTriangle( t );
        const glm::vec3 n   = glm::cross( mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] ),
                                          mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] ) );
        const float     len = glm::length( n );
        return len > 0.0f ? n / len : glm::vec3( 0.0f );
    }

    namespace
    {
        // Interior angle of t at corner j; zero for a degenerate corner so it contributes nothing.
        float CornerAngle( const EditMesh& mesh, int t, int j )
        {
            const auto&     c  = mesh.GetTriangle( t );
            const glm::vec3 p  = mesh.GetPosition( c[j] );
            const glm::vec3 e1 = mesh.GetPosition( c[( j + 1 ) % 3] ) - p;
            const glm::vec3 e2 = mesh.GetPosition( c[( j + 2 ) % 3] ) - p;
            const float     l  = glm::length( e1 ) * glm::length( e2 );
            if ( l <= 0.0f )
                return 0.0f;
            return std::acos( std::clamp( glm::dot( e1, e2 ) / l, -1.0f, 1.0f ) );
        }
    } // namespace

    void ComputeNormalsByPolyGroup( EditMesh& mesh )
    {
        EditMeshAttributes& attributes = mesh.Attributes();
        attributes.EnableNormals();
        NormalOverlay& normals = *attributes.Normals();
        for ( const int t : mesh.TriangleIds() )
            normals.UnsetTriangle( t );

        std::vector<std::array<int, 3>> elements( static_cast<size_t>( mesh.MaxTriangleId() ) );
        for ( const int v : mesh.VertexIds() )
        {
            // (polygroup, weighted normal sum, element) for each group meeting at v; few per vertex.
            std::vector<std::pair<int, glm::vec3>> sums;
            const std::vector<int>                 triangles = mesh.GetVertexTriangles( v );
            for ( const int t : triangles )
            {
                const int group = attributes.GetPolyGroup( t );
                const int j =
                     static_cast<int>( std::find( mesh.GetTriangle( t ).begin(), mesh.GetTriangle( t ).end(), v ) -
                                       mesh.GetTriangle( t ).begin() );
                const glm::vec3 weighted = TriangleNormal( mesh, t ) * CornerAngle( mesh, t, j );
                auto            it =
                     std::find_if( sums.begin(), sums.end(), [&]( const auto& s ) { return s.first == group; } );
                if ( it == sums.end() )
                    sums.emplace_back( group, weighted );
                else
                    it->second += weighted;
            }
            std::vector<int> groupElements;
            for ( const auto& [group, sum] : sums )
            {
                const float len = glm::length( sum );
                // A group whose triangles at v are all degenerate has no direction to offer; +Z is arbitrary
                // but finite, and the zero-area triangles it shades are invisible.
                groupElements.push_back( normals.AppendElement( len > 0.0f ? sum / len : glm::vec3( 0, 0, 1 ) ) );
            }
            for ( const int t : triangles )
            {
                const auto& corners = mesh.GetTriangle( t );
                const int   group   = attributes.GetPolyGroup( t );
                const auto  gi =
                     std::find_if( sums.begin(), sums.end(), [&]( const auto& s ) { return s.first == group; } ) -
                     sums.begin();
                for ( int j = 0; j < 3; ++j )
                    if ( corners[j] == v )
                        elements[t][j] = groupElements[static_cast<size_t>( gi )];
            }
        }
        for ( const int t : mesh.TriangleIds() )
            (void)normals.SetTriangle( mesh, t, elements[t] );
    }
} // namespace Desert::Geometry
