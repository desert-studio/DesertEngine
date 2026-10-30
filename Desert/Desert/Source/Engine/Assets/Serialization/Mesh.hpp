#pragma once

#include <Common/Content/AssetEnvelope.hpp>

#include <vector>
#include <array>
#include <optional>
#include <string>
#include <glm/glm.hpp>
#include <Common/Core/Math/AABB.hpp>

#include <Common/Core/UUID.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Assets/MeshDisplacementSettings.hpp>
#include <Engine/Geometry/MeshTypes.hpp>
#include <Engine/Geometry/ClusterDag.hpp>

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

    /// The runtime vertex a stored skinned vertex becomes — the one conversion, for the asset load and for a
    /// generator that works from MeshAssetData before any asset is loaded.
    [[nodiscard]] inline SkinnedVertex ToSkinnedVertex( const SkinnedVertexData& v )
    {
        SkinnedVertex vertex;
        vertex.StaticVertex.Position  = v.Position;
        vertex.StaticVertex.Normal    = v.Normal;
        vertex.StaticVertex.Tangent   = v.Tangent;
        vertex.StaticVertex.Bitangent = v.Bitangent;
        vertex.StaticVertex.TexCoord  = v.TexCoord;
        vertex.BoneIDs                = v.BoneIDs;
        vertex.BoneWeights            = v.BoneWeights;
        return vertex;
    }

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

        // The material's header GUID; null = no material. The runtime handle is HandleForGuid of it.
        Common::Content::AssetGuid MaterialGuid;

        // Baked LOD triangle sets (cooked at import, submesh-local, coarsest last). Generated at load only
        // when LodChainCooked is false.
        std::vector<std::vector<IndexData>> LODs;

        // THE LOD CHAIN IS THE COOK'S (MeshBinary v8; UE: a cooked mesh's LOD count is its
        // FStaticMeshRenderData::LODResources, nothing is generated on load). True for a submesh whose LODs above
        // are its whole chain - possibly none (LodPolicy None, or a chain whose targets the section cannot reach)
        // - so the load appends exactly those. False = no cook decided the chain; the load generates
        // Geometry::UnbakedLODChain. Every MeshBinary writer states it true (MeshDeriver, the skinned import,
        // ProceduralCharacterFactory). A v6/v7 file states none and reads as cooked exactly when it carries LODs
        // or is skinned (what its loads did), the upgrade DecodeMeshBinary applies and Tools/SceneMigrator writes
        // back.
        bool LodChainCooked = false;

        // THE SUBMESH'S CLUSTER LOD DAG (MeshBinary v7, VIRT-GEOM): built at cook for a mesh imported with
        // MeshImportSettings::VirtualGeometry, empty (no clusters) otherwise. Vertex refs are submesh-relative, as
        // the triangles of Indices / LODs are.
        Geometry::ClusterDag ClusterDag;
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
        // The asset's identity (MeshBinary v3): minted at first import, kept across re-imports. Null only
        // for a mesh read from a v1/v2 file, which stated none.
        Common::Content::AssetGuid     Guid;
        std::vector<StaticVertexData>  StaticVertices;
        std::vector<SkinnedVertexData> SkinnedVertices;
        std::vector<IndexData>         Indices;
        std::vector<SubmeshData>       Submeshes;
        // THE MESH'S SKELETON (MeshBinary v5, SKEL-TREE): the .skeleton's header GUID; null for a static mesh.
        Common::Content::AssetGuid Skeleton;
        // THE SKINNED MESH'S DISPLACEMENT MODE (MeshBinary v6, CONTRACT-DISPLACE §9.8): its import setting, kept in
        // its own file (no source envelope). None for every static mesh: its mode is its source's IMPT.
        MeshDisplacementMode Displacement = MeshDisplacementMode::None;
        // Blendshapes (empty for meshes without morph targets). New field — meshes cooked before this exists
        // are read with rfl::DefaultIfMissing so they simply come back with an empty list.
        std::vector<MorphTargetData>   MorphTargets;
        // One polygroup per FACE of Indices, or empty (a mesh cooked before MeshBinary v2, or with no groups).
        // What a DynamicMesh3 lifted from this mesh needs beyond the render arrays (MeshBinary.hpp, version 2).
        std::vector<int32_t> PolyGroups;
        // THE OPTIONAL VERTEX STREAMS (MeshBinary v4, UE FColorVertexBuffer / the second UV channel): each is
        // EMPTY or holds exactly one entry per vertex of StaticVertices (or SkinnedVertices), in that array's
        // order. They are streams beside the vertex, not fields of it, so a mesh without them pays nothing.
        // Colors are LINEAR RGBA8 (glTF COLOR_0 is linear; UE's FColor vertex colours are 8-bit as well).
        std::vector<std::array<uint8_t, 4>> Colors; // R, G, B, A
        std::vector<glm::vec2>              UV1;
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