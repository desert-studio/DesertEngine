#include "EditMeshNormals.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <map>
#include <cmath>
#include <utility>

#include <spdlog/fmt/fmt.h>
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

    int TriangleCornerOf( const EditMesh& mesh, int t, int v )
    {
        const auto& c = mesh.GetTriangle( t );
        for ( int j = 0; j < 3; ++j )
            if ( c[j] == v )
                return j;
        return -1;
    }

    // Every triangle touching `vertices` gets, at those corners, the angle-weighted normal of the
    // triangles in ITS polygroup at that vertex; its other corners keep their element. A triangle unset in
    // the layer must have all three corners in the set (the operations below guarantee it).
    Common::BoolResultStr RebuildNormalsByPolyGroupAt( EditMesh& mesh, const std::vector<int>& vertices )
    {
        NormalOverlay* normals = mesh.Attributes().Normals();
        if ( normals == nullptr )
            return Common::MakeSuccess( true );
        std::vector<char> inSet( static_cast<size_t>( mesh.MaxVertexId() ), 0 );
        for ( const int v : vertices )
            inSet[v] = 1;

        std::map<std::pair<int, int>, int> element; // (vertex, polygroup) -> new element
        std::vector<int>                   touched;
        for ( const int v : vertices )
        {
            std::map<int, glm::vec3> sums;
            for ( const int t : mesh.GetVertexTriangles( v ) )
            {
                sums[mesh.Attributes().GetPolyGroup( t )] +=
                     TriangleNormal( mesh, t ) * CornerAngle( mesh, t, TriangleCornerOf( mesh, t, v ) );
                touched.push_back( t );
            }
            for ( const auto& [group, sum] : sums )
            {
                const float     length = glm::length( sum );
                const glm::vec3 n      = length > 0.0f ? sum / length : glm::vec3( 0.0f, 1.0f, 0.0f );
                element[{ v, group }]  = normals->AppendElement( n );
            }
        }
        std::sort( touched.begin(), touched.end() );
        touched.erase( std::unique( touched.begin(), touched.end() ), touched.end() );
        for ( const int t : touched )
        {
            const auto&        corners = mesh.GetTriangle( t );
            const bool         wasSet  = normals->IsSetTriangle( t );
            std::array<int, 3> next{};
            for ( int j = 0; j < 3; ++j )
            {
                if ( inSet[corners[j]] != 0 )
                    next[j] = element.at( { corners[j], mesh.Attributes().GetPolyGroup( t ) } );
                else if ( wasSet )
                    next[j] = normals->GetTriangle( t )[j];
                else
                    return Common::MakeFormattedError<bool>(
                         "normal rebuild: triangle {} has no normals and its corner {} is outside the "
                         "rebuilt set",
                         t, corners[j] );
            }
            if ( const EditResult r = normals->SetTriangle( mesh, t, next ); r != EditResult::Ok )
                return Common::MakeFormattedError<bool>( "normal rebuild: triangle {} refused: {}", t,
                                                         ToString( r ) );
        }
        return Common::MakeSuccess( true );
    }

    // Tangents for the given triangles from UV layer 0 and the (already rebuilt) normals, one element per
    // corner. Without a UV layer the tangent follows the triangle's first edge: the frame is then
    // arbitrary but valid, which is all a mesh without UVs can have.
    Common::BoolResultStr ComputeTangentsAt( EditMesh& mesh, const std::vector<int>& triangles )
    {
        TangentOverlay* tangents = mesh.Attributes().Tangents();
        if ( tangents == nullptr )
            return Common::MakeSuccess( true );
        const UVOverlay*     uvs     = mesh.Attributes().UV( 0 );
        const NormalOverlay* normals = mesh.Attributes().Normals();
        for ( const int t : triangles )
        {
            const auto&     c       = mesh.GetTriangle( t );
            const glm::vec3 e1      = mesh.GetPosition( c[1] ) - mesh.GetPosition( c[0] );
            const glm::vec3 e2      = mesh.GetPosition( c[2] ) - mesh.GetPosition( c[0] );
            glm::vec3       tangent = e1;
            glm::vec3       bitangent( 0.0f );
            if ( uvs != nullptr && uvs->IsSetTriangle( t ) )
            {
                const auto&     u   = uvs->GetTriangle( t );
                const glm::vec2 d1  = uvs->GetElement( u[1] ) - uvs->GetElement( u[0] );
                const glm::vec2 d2  = uvs->GetElement( u[2] ) - uvs->GetElement( u[0] );
                const float     det = d1.x * d2.y - d2.x * d1.y;
                if ( std::abs( det ) > 1e-12f )
                {
                    tangent   = ( e1 * d2.y - e2 * d1.y ) / det;
                    bitangent = ( e2 * d1.x - e1 * d2.x ) / det;
                }
            }
            std::array<int, 3> next{};
            for ( int j = 0; j < 3; ++j )
            {
                const glm::vec3 n  = normals != nullptr && normals->IsSetTriangle( t )
                                          ? normals->GetElement( normals->GetTriangle( t )[j] )
                                          : TriangleNormal( mesh, t );
                glm::vec3       tj = tangent - n * glm::dot( n, tangent );
                if ( glm::length( tj ) <= 1e-8f )
                    tj = glm::cross( n, std::abs( n.x ) < 0.9f ? glm::vec3( 1, 0, 0 ) : glm::vec3( 0, 1, 0 ) );
                tj            = glm::normalize( tj );
                const float w = glm::dot( glm::cross( n, tj ), bitangent ) < 0.0f ? -1.0f : 1.0f;
                next[j]       = tangents->AppendElement( glm::vec4( tj, w ) );
            }
            if ( const EditResult r = tangents->SetTriangle( mesh, t, next ); r != EditResult::Ok )
                return Common::MakeFormattedError<bool>( "tangents: triangle {} refused: {}", t, ToString( r ) );
        }
        return Common::MakeSuccess( true );
    }

} // namespace Desert::Geometry
