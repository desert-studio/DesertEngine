// The engine's one tangent-space generator: MikkTSpace (ThirdParty/mikktspace), as UE's mesh build runs it
// (MeshUtilitiesCommon / MeshDescriptionOperations ComputeMikktTangents). Every imported and derived mesh takes
// its tangent frame from here, so a normal map baked against MikkTSpace (Substance, Blender, xNormal, UE)
// decodes on this engine's meshes in the frame it was baked in.
//
// Convention (mikktspace.h): per corner a unit tangent T and a sign; the bitangent is sign * cross(N, T) with
// N the corner's normal. The frame is computed in the space the vertices are stored in.
#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Desert::Geometry
{
    // One triangle mesh as MikkTSpace reads it: per-corner position, normal and UV, and a sink for the frame
    // MikkTSpace writes back per corner. Corners are (triangle, 0..2) in the triangle's winding order.
    class MikkTangentSource
    {
    public:
        virtual ~MikkTangentSource() = default;

        [[nodiscard]] virtual int       TriangleCount() const                                            = 0;
        [[nodiscard]] virtual glm::vec3 Position( int triangle, int corner ) const                       = 0;
        [[nodiscard]] virtual glm::vec3 Normal( int triangle, int corner ) const                         = 0;
        [[nodiscard]] virtual glm::vec2 TexCoord( int triangle, int corner ) const                       = 0;
        virtual void SetFrame( int triangle, int corner, const glm::vec3& tangent, float bitangentSign ) = 0;
    };

    // Runs MikkTSpace's genTangSpaceDefault (angular threshold off, as UE and Blender run it) over source.
    // False only when MikkTSpace could not allocate its working set; otherwise every corner is written
    // (a degenerate triangle receives the frame of its vertex's other triangles, or an arbitrary one).
    [[nodiscard]] bool ComputeMikkTangents( MikkTangentSource& source );

    // MikkTSpace's bitangent for a corner: sign * cross(N, T).
    [[nodiscard]] inline glm::vec3 MikkBitangent( const glm::vec3& normal, const glm::vec3& tangent, float sign )
    {
        return sign * glm::cross( normal, tangent );
    }

    // An indexed triangle mesh's MikkTSpace frames. MikkTSpace works per corner; where the corners of one vertex
    // disagree (a UV mirror seam, a sign flip) the vertex is split, as UE welds its wedges after
    // ComputeMikktTangents. Output vertex k copies input vertex SourceVertex[k]; the first frame of every input
    // vertex keeps the vertex's index (an unsplit mesh keeps its vertex order), splits are appended after.
    struct IndexedMikkTangents
    {
        std::vector<uint32_t>   SourceVertex;
        std::vector<glm::uvec3> Triangles;
        std::vector<glm::vec3>  Tangents;
        std::vector<glm::vec3>  Bitangents;
    };

    // positions / normals / texCoords are per input vertex; triangles index them. A vertex no triangle names
    // keeps its index with a frame built around its normal.
    [[nodiscard]] bool ComputeIndexedMikkTangents( std::span<const glm::vec3>  positions,
                                                   std::span<const glm::vec3>  normals,
                                                   std::span<const glm::vec2>  texCoords,
                                                   std::span<const glm::uvec3> triangles,
                                                   IndexedMikkTangents&        out );

    // The MikkTSpace frames of an engine vertex array, written in place: every importer and procedural producer
    // takes its tangents from here. VertexT is any vertex with Position, Normal, TexCoord, Tangent and Bitangent;
    // TriangleT any triangle with V1, V2, V3. The triangles index vertices[first, vertices.size()) relative to
    // `first`; the range is re-laid as ComputeIndexedMikkTangents splits it (range vertex k becomes the old range
    // vertex SourceVertex[k], splits appended) and the triangles are re-pointed. The result is SourceVertex, so a
    // per-vertex array parallel to the range follows the split; nullopt only when MikkTSpace could not allocate.
    template <class VertexT, class TriangleT>
    [[nodiscard]] std::optional<std::vector<uint32_t>>
    ApplyMikkTangents( std::vector<VertexT>& vertices, size_t first, std::span<TriangleT> triangles )
    {
        const size_t           count = vertices.size() - first;
        std::vector<glm::vec3> positions( count );
        std::vector<glm::vec3> normals( count );
        std::vector<glm::vec2> texCoords( count );
        for ( size_t i = 0; i < count; ++i )
        {
            positions[i] = vertices[first + i].Position;
            normals[i]   = vertices[first + i].Normal;
            texCoords[i] = vertices[first + i].TexCoord;
        }
        std::vector<glm::uvec3> corners( triangles.size() );
        for ( size_t t = 0; t < triangles.size(); ++t )
            corners[t] = glm::uvec3( triangles[t].V1, triangles[t].V2, triangles[t].V3 );

        IndexedMikkTangents frames;
        if ( !ComputeIndexedMikkTangents( positions, normals, texCoords, corners, frames ) )
            return std::nullopt;

        std::vector<VertexT> range;
        range.reserve( frames.SourceVertex.size() );
        for ( size_t k = 0; k < frames.SourceVertex.size(); ++k )
        {
            VertexT v   = vertices[first + frames.SourceVertex[k]];
            v.Tangent   = frames.Tangents[k];
            v.Bitangent = frames.Bitangents[k];
            range.push_back( v );
        }
        vertices.resize( first );
        vertices.insert( vertices.end(), range.begin(), range.end() );
        for ( size_t t = 0; t < triangles.size(); ++t )
        {
            triangles[t].V1 = frames.Triangles[t].x;
            triangles[t].V2 = frames.Triangles[t].y;
            triangles[t].V3 = frames.Triangles[t].z;
        }
        return std::move( frames.SourceVertex );
    }

    // How a linear map carries a surface frame (UE: FMatrix::TransformVector for tangent directions,
    // FMatrix::TransposeAdjoint times the determinant's sign for normals, as
    // FStaticMeshOperations::ApplyTransform does). A direction (tangent, bitangent, position delta) goes by the
    // map itself; a normal by its inverse transpose, taken as the cofactor matrix times sign(det): the same
    // direction, defined for a singular map too. So a normal stays perpendicular to its surface under non-uniform
    // scale and keeps its side under a mirror. Every place that bakes a transform into vertices goes through this.
    class SurfaceFrameTransform
    {
    public:
        explicit SurfaceFrameTransform( const glm::mat3& linear ) : m_Linear( linear )
        {
            const glm::vec3 a   = linear[0];
            const glm::vec3 b   = linear[1];
            const glm::vec3 c   = linear[2];
            const float     det = glm::dot( a, glm::cross( b, c ) );
            const float     s   = det < 0.0f ? -1.0f : 1.0f;
            m_Normal = glm::mat3( s * glm::cross( b, c ), s * glm::cross( c, a ), s * glm::cross( a, b ) );
        }

        // A direction carried by the map, unnormalized (a position delta keeps its length).
        [[nodiscard]] glm::vec3 Direction( const glm::vec3& d ) const
        {
            return m_Linear * d;
        }
        // A unit direction (tangent, bitangent) carried by the map, renormalized.
        [[nodiscard]] glm::vec3 UnitDirection( const glm::vec3& d ) const
        {
            return Unit( m_Linear * d );
        }
        // A normal carried by the map's inverse transpose, renormalized.
        [[nodiscard]] glm::vec3 Normal( const glm::vec3& n ) const
        {
            return Unit( m_Normal * n );
        }
        // A morph target's normal delta: the carried morphed normal minus the carried base normal, so
        // Normal(base) + delta is exactly Normal(base + delta) whatever the map's scale.
        [[nodiscard]] glm::vec3 NormalDelta( const glm::vec3& base, const glm::vec3& delta ) const
        {
            return Normal( base + delta ) - Normal( base );
        }

    private:
        static glm::vec3 Unit( const glm::vec3& d )
        {
            const float length = glm::length( d );
            return length > 0.0f ? d / length : d;
        }

        glm::mat3 m_Linear;
        glm::mat3 m_Normal{ 1.0f };
    };
} // namespace Desert::Geometry
