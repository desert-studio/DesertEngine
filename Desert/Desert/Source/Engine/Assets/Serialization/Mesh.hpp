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
        // One polygroup per FACE of Indices, or empty (a mesh cooked before MeshBinary v2, or with no groups).
        // What an EditMesh lifted from this mesh needs beyond the render arrays (MeshBinary.hpp, version 2).
        std::vector<int32_t> PolyGroups;
    };
} // namespace Desert::Assets::Serialization