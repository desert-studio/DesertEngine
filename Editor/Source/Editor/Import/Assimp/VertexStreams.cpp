#include "VertexStreams.hpp"

#include <assimp/mesh.h>
#include <assimp/scene.h>

namespace Desert::Editor
{
    SceneVertexStreams StreamsOf( const aiScene& scene )
    {
        SceneVertexStreams streams;
        for ( unsigned m = 0; m < scene.mNumMeshes; ++m )
        {
            streams.Colors |= scene.mMeshes[m]->HasVertexColors( 0 );
            streams.UV1 |= scene.mMeshes[m]->HasTextureCoords( 1 );
        }
        return streams;
    }

    void AppendVertexStreams( const aiMesh& mesh, const SceneVertexStreams& streams,
                              std::vector<std::array<uint8_t, 4>>& colors, std::vector<glm::vec2>& uv1 )
    {
        const bool hasColors = mesh.HasVertexColors( 0 );
        const bool hasUV1    = mesh.HasTextureCoords( 1 );
        for ( unsigned i = 0; i < mesh.mNumVertices; ++i )
        {
            if ( streams.Colors )
            {
                glm::vec4 c( 1.0f );
                if ( hasColors )
                {
                    const aiColor4D& from = mesh.mColors[0][i];
                    c                     = glm::vec4( from.r, from.g, from.b, from.a );
                }
                const glm::vec4 q = glm::round( glm::clamp( c, 0.0f, 1.0f ) * 255.0f );
                colors.push_back( { static_cast<uint8_t>( q.r ), static_cast<uint8_t>( q.g ),
                                    static_cast<uint8_t>( q.b ), static_cast<uint8_t>( q.a ) } );
            }
            if ( streams.UV1 )
                uv1.push_back( hasUV1 ? glm::vec2( mesh.mTextureCoords[1][i].x, mesh.mTextureCoords[1][i].y )
                                      : glm::vec2( 0.0f ) );
        }
    }
} // namespace Desert::Editor
