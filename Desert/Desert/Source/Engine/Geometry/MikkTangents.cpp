#include "Engine/Geometry/MikkTangents.hpp"

#include <mikktspace.h>

#include <cmath>

namespace Desert::Geometry
{
    namespace
    {
        MikkTangentSource& SourceOf( const SMikkTSpaceContext* context )
        {
            return *static_cast<MikkTangentSource*>( context->m_pUserData );
        }

        void Store( float out[], const glm::vec3& v )
        {
            out[0] = v.x;
            out[1] = v.y;
            out[2] = v.z;
        }

        // A unit tangent perpendicular to n (Duff et al. 2017, Building an Orthonormal Basis, Revisited).
        glm::vec3 AnyTangent( const glm::vec3& n )
        {
            const float sign = std::copysign( 1.0f, n.z );
            const float a    = -1.0f / ( sign + n.z );
            const float b    = n.x * n.y * a;
            return glm::vec3( 1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x );
        }

        struct CornerFrame
        {
            glm::vec3 Tangent{ 0.0f };
            float     Sign = 1.0f;
        };

        class IndexedSource final : public MikkTangentSource
        {
        public:
            IndexedSource( std::span<const glm::vec3> positions, std::span<const glm::vec3> normals,
                           std::span<const glm::vec2> texCoords, std::span<const glm::uvec3> triangles )
                 : m_Positions( positions ), m_Normals( normals ), m_TexCoords( texCoords ),
                   m_Triangles( triangles ), m_Frames( triangles.size() * 3 )
            {
            }

            int TriangleCount() const override
            {
                return static_cast<int>( m_Triangles.size() );
            }
            glm::vec3 Position( int t, int c ) const override
            {
                return m_Positions[m_Triangles[t][c]];
            }
            glm::vec3 Normal( int t, int c ) const override
            {
                return m_Normals[m_Triangles[t][c]];
            }
            glm::vec2 TexCoord( int t, int c ) const override
            {
                return m_TexCoords[m_Triangles[t][c]];
            }
            void SetFrame( int t, int c, const glm::vec3& tangent, float sign ) override
            {
                m_Frames[static_cast<size_t>( t ) * 3 + c] = { tangent, sign };
            }

            const std::vector<CornerFrame>& Frames() const
            {
                return m_Frames;
            }

        private:
            std::span<const glm::vec3>  m_Positions;
            std::span<const glm::vec3>  m_Normals;
            std::span<const glm::vec2>  m_TexCoords;
            std::span<const glm::uvec3> m_Triangles;
            std::vector<CornerFrame>    m_Frames;
        };
    } // namespace

    bool ComputeMikkTangents( MikkTangentSource& source )
    {
        SMikkTSpaceInterface callbacks{};
        callbacks.m_getNumFaces = []( const SMikkTSpaceContext* context ) -> int
        { return SourceOf( context ).TriangleCount(); };
        callbacks.m_getNumVerticesOfFace = []( const SMikkTSpaceContext*, const int ) -> int { return 3; };
        callbacks.m_getPosition =
             []( const SMikkTSpaceContext* context, float out[], const int face, const int vert )
        { Store( out, SourceOf( context ).Position( face, vert ) ); };
        callbacks.m_getNormal = []( const SMikkTSpaceContext* context, float out[], const int face,
                                    const int vert ) { Store( out, SourceOf( context ).Normal( face, vert ) ); };
        callbacks.m_getTexCoord =
             []( const SMikkTSpaceContext* context, float out[], const int face, const int vert )
        {
            const glm::vec2 uv = SourceOf( context ).TexCoord( face, vert );
            out[0]             = uv.x;
            out[1]             = uv.y;
        };
        callbacks.m_setTSpaceBasic = []( const SMikkTSpaceContext* context, const float tangent[],
                                         const float sign, const int face, const int vert )
        { SourceOf( context ).SetFrame( face, vert, glm::vec3( tangent[0], tangent[1], tangent[2] ), sign ); };

        SMikkTSpaceContext context{};
        context.m_pInterface = &callbacks;
        context.m_pUserData  = &source;
        return genTangSpaceDefault( &context ) != 0;
    }

    bool ComputeIndexedMikkTangents( std::span<const glm::vec3> positions, std::span<const glm::vec3> normals,
                                     std::span<const glm::vec2> texCoords, std::span<const glm::uvec3> triangles,
                                     IndexedMikkTangents& out )
    {
        IndexedSource source( positions, normals, texCoords, triangles );
        if ( !ComputeMikkTangents( source ) )
            return false;

        const size_t vertexCount = positions.size();
        out.SourceVertex.assign( vertexCount, 0 );
        out.Tangents.assign( vertexCount, glm::vec3( 0.0f ) );
        out.Bitangents.assign( vertexCount, glm::vec3( 0.0f ) );
        out.Triangles.resize( triangles.size() );

        // The first corner written to a vertex takes the vertex's own index; a corner whose frame differs from
        // every frame already on its vertex splits it. MikkTSpace writes one value to every corner of a group, so
        // corners of one group compare equal exactly.
        std::vector<char>                  claimed( vertexCount, 0 );
        std::vector<std::vector<uint32_t>> splits( vertexCount );
        std::vector<float>                 signs( vertexCount, 1.0f );
        const std::vector<CornerFrame>&    frames = source.Frames();
        for ( size_t t = 0; t < triangles.size(); ++t )
        {
            for ( int c = 0; c < 3; ++c )
            {
                const uint32_t     v     = triangles[t][c];
                const CornerFrame& frame = frames[t * 3 + c];
                uint32_t           index = v;
                if ( !claimed[v] )
                {
                    claimed[v]          = 1;
                    out.SourceVertex[v] = v;
                    out.Tangents[v]     = frame.Tangent;
                    signs[v]            = frame.Sign;
                }
                else if ( out.Tangents[v] != frame.Tangent || signs[v] != frame.Sign )
                {
                    index = UINT32_MAX;
                    for ( const uint32_t s : splits[v] )
                    {
                        if ( out.Tangents[s] == frame.Tangent && signs[s] == frame.Sign )
                        {
                            index = s;
                            break;
                        }
                    }
                    if ( index == UINT32_MAX )
                    {
                        index = static_cast<uint32_t>( out.SourceVertex.size() );
                        out.SourceVertex.push_back( v );
                        out.Tangents.push_back( frame.Tangent );
                        out.Bitangents.emplace_back( 0.0f );
                        signs.push_back( frame.Sign );
                        splits[v].push_back( index );
                    }
                }
                out.Triangles[t][c] = index;
            }
        }

        for ( size_t k = 0; k < out.SourceVertex.size(); ++k )
        {
            if ( k < vertexCount && !claimed[k] )
            {
                out.SourceVertex[k] = static_cast<uint32_t>( k );
                out.Tangents[k]     = AnyTangent( normals[k] );
                signs[k]            = 1.0f;
            }
            out.Bitangents[k] = MikkBitangent( normals[out.SourceVertex[k]], out.Tangents[k], signs[k] );
        }
        return true;
    }
} // namespace Desert::Geometry
