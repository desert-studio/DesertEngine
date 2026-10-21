// The engine's one tangent-space generator: MikkTSpace (ThirdParty/mikktspace), as UE's mesh build runs it
// (MeshUtilitiesCommon / MeshDescriptionOperations ComputeMikktTangents). Every imported and derived mesh takes
// its tangent frame from here, so a normal map baked against MikkTSpace (Substance, Blender, xNormal, UE)
// decodes on this engine's meshes in the frame it was baked in.
//
// Convention (mikktspace.h): per corner a unit tangent T and a sign; the bitangent is sign * cross(N, T) with
// N the corner's normal. The frame is computed in the space the vertices are stored in.
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
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
} // namespace Desert::Geometry
