#pragma once

#include <vector>
#include <array>
#include <optional>
#include <string>
#include <glm/glm.hpp>
#include <Common/Core/Math/AABB.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Common.hpp>

namespace Desert::Assets::Serialization
{
    struct StaticVertexData
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec3 Tangent;
        glm::vec3 Bitangent;
        glm::vec2 TexCoord;
    };

    struct SkinnedVertexData
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec3 Tangent;
        glm::vec3 Bitangent;
        glm::vec2 TexCoord;

        std::array<uint32_t, 4> BoneIDs     = { 0, 0, 0, 0 };
        std::array<float, 4>    BoneWeights = { 0, 0, 0, 0 };
    };

    struct IndexData
    {
        uint32_t V1, V2, V3;
    };

    struct SubmeshData
    {
        std::string        Name;
        uint32_t           VertexOffset;
        uint32_t           VertexCount;
        uint32_t           IndexOffset;
        uint32_t           IndexCount;
        glm::mat4          Transform;
        Common::Math::AABB BoundingBox;

        Common::UUID MaterialHandle;

        // Baked LOD triangle sets (cooked at import, submesh-local, coarsest last). Empty => LODs are
        // generated at load. New field — meshes cooked before it exists load via rfl::DefaultIfMissing with an
        // empty list (so they fall back to load-time generation, unchanged).
        std::vector<std::vector<IndexData>> LODs;
    };

    // Blendshape / morph target on disk: per-vertex deltas index-aligned with the mesh's global vertex array
    // (Static/SkinnedVertices). DeltaNormals may be empty (position-only). Mirrors Desert::MorphTarget.
    struct MorphTargetData
    {
        std::string            Name;
        std::vector<glm::vec3> DeltaPositions;
        std::vector<glm::vec3> DeltaNormals;
    };

    struct MeshAssetData
    {
        bool                           IsSkinned = false;
        std::vector<StaticVertexData>  StaticVertices;
        std::vector<SkinnedVertexData> SkinnedVertices;
        std::vector<IndexData>         Indices;
        std::vector<SubmeshData>       Submeshes;
        std::optional<uint64_t>        SkeletonSignature;
        // Blendshapes (empty for meshes without morph targets). New field — meshes cooked before this exists
        // are read with rfl::DefaultIfMissing so they simply come back with an empty list.
        std::vector<MorphTargetData>   MorphTargets;
    };

    // THE MESH'S BOX AROUND ITS OWN ORIGIN, as the cook knows it: the union of the submesh boxes the file
    // stores. The loaded asset copies those same boxes onto its submeshes and Geometry::LocalBounds unions
    // them, so a box stated here at cook time and one read back from a loaded mesh are the same numbers.
    // Nullopt for a mesh with no submeshes, which draws nothing and has no extent.
    [[nodiscard]] inline std::optional<Common::Math::AABB> MeshDataBounds( const MeshAssetData& data )
    {
        if ( data.Submeshes.empty() )
            return std::nullopt;
        Common::Math::AABB box = data.Submeshes.front().BoundingBox;
        for ( const SubmeshData& submesh : data.Submeshes )
        {
            box.Min = glm::min( box.Min, submesh.BoundingBox.Min );
            box.Max = glm::max( box.Max, submesh.BoundingBox.Max );
        }
        return box;
    }
} // namespace Desert::Assets::Serialization