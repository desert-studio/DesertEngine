#pragma once

#include <glm/glm.hpp>
#include <Common/Core/Math/AABB.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Desert
{
    struct Vertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec3 Tangent;
        glm::vec3 Bitangent;
        glm::vec2 TexCoord;
    };

    // ONE VERTEX OF THE OPTIONAL STREAMS (UE FVertexFactory Color + TexCoord1), interleaved in the mesh's
    // second vertex buffer (binding 1, locations 7 and 8; the layout is MeshVertexLayout's). The defaults are
    // what a mesh without the streams reads at stride 0: white, UV1 (0,0).
    struct MeshVertexStreams
    {
        std::array<uint8_t, 4> Color = { 255, 255, 255, 255 }; // linear RGBA8, read as UNORM
        glm::vec2              UV1   = { 0.0f, 0.0f };
    };
    static_assert( sizeof( MeshVertexStreams ) == 12, "the streams' stride is part of the pipeline layout" );

    // The mesh asset's Colors / UV1 (either may be absent) packed into the stream vertices. BOTH absent = empty
    // — no buffer, the mesh draws through the stride-0 variant; one absent = its default in every vertex.
    inline std::vector<MeshVertexStreams> PackMeshVertexStreams( const std::vector<std::array<uint8_t, 4>>& colors,
                                                                 const std::vector<glm::vec2>&              uv1,
                                                                 const size_t vertexCount )
    {
        std::vector<MeshVertexStreams> streams;
        if ( colors.empty() && uv1.empty() )
            return streams;
        streams.resize( vertexCount );
        for ( size_t i = 0; i < vertexCount; ++i )
        {
            if ( i < colors.size() )
                streams[i].Color = colors[i];
            if ( i < uv1.size() )
                streams[i].UV1 = uv1[i];
        }
        return streams;
    }

    struct SkinnedVertex
    {
        static constexpr size_t MAX_BONE_INFLUENCES = 4;

        Vertex StaticVertex;

        std::array<uint32_t, MAX_BONE_INFLUENCES> BoneIDs     = { 0, 0, 0, 0 };
        std::array<float, MAX_BONE_INFLUENCES>    BoneWeights = { 0.0f, 0.0f, 0.0f, 0.0f };

        bool AddBoneInfluence( uint32_t boneID, float weight )
        {
            for ( size_t i = 0; i < MAX_BONE_INFLUENCES; i++ )
            {
                if ( BoneWeights[i] == 0.0f )
                {
                    BoneIDs[i]     = boneID;
                    BoneWeights[i] = weight;
                    return true;
                }
            }

            size_t smallestIdx = 0;
            for ( size_t i = 1; i < MAX_BONE_INFLUENCES; i++ )
            {
                if ( BoneWeights[i] < BoneWeights[smallestIdx] )
                {
                    smallestIdx = i;
                }
            }

            if ( weight > BoneWeights[smallestIdx] )
            {
                BoneIDs[smallestIdx]     = boneID;
                BoneWeights[smallestIdx] = weight;
                NormalizeWeights();
                return true;
            }

            return false;
        }

        void NormalizeWeights()
        {
            float sum = 0.0f;
            for ( float weight : BoneWeights )
            {
                sum += weight;
            }

            if ( sum > 0.0f )
            {
                float invSum = 1.0f / sum;
                for ( float& weight : BoneWeights )
                {
                    weight *= invSum;
                }
            }
        }

        bool HasFreeSlot() const
        {
            for ( float weight : BoneWeights )
            {
                if ( weight == 0.0f )
                    return true;
            }
            return false;
        }

        size_t GetActiveInfluenceCount() const
        {
            size_t count = 0;
            for ( float weight : BoneWeights )
            {
                if ( weight > 0.0f )
                    count++;
            }
            return count;
        }
    };

    // Blendshape / morph target: per-vertex deltas relative to the base mesh, applied as
    // base + Σ(weight_k · target_k). Delta arrays are index-aligned with the mesh's (global, all-submeshes)
    // vertex array; a delta of 0 means the vertex is untouched by that target. DeltaNormals may be empty
    // (position-only morph). See Geometry::ApplyMorphTargets for the CPU blend.
    struct MorphTarget
    {
        std::string            Name;
        std::vector<glm::vec3> DeltaPositions;
        std::vector<glm::vec3> DeltaNormals; // optional; empty => positions only
    };

    struct Index
    {
        uint32_t V1, V2, V3;
    };

    struct TriangleCache
    {
        Vertex V1, V2, V3;
    };

    // One level of detail for a submesh: a range into the mesh's index buffer (uint32 units) that draws
    // the simplified geometry against the SAME vertex buffer (baseVertex = Submesh.VertexOffset).
    struct LODRange
    {
        uint32_t IndexOffset;
        uint32_t IndexCount;
    };

    struct Submesh
    {
        std::string        Name;
        uint32_t           VertexOffset;
        uint32_t           VertexCount;
        uint32_t           IndexOffset;
        uint32_t           IndexCount;
        glm::mat4          Transform;
        Common::Math::AABB BoundingBox;

        // LOD chain (built by StaticMesh on load). LODs[0] is the original geometry, so drawing LOD 0 is
        // byte-identical to (IndexOffset, IndexCount). Empty for meshes with no LODs (procedural / skinned).
        //
        // `= {}` on this and on BakedLODs below is not decoration: the five aggregate initialisations of
        // Submesh in the engine and the editor deliberately stop at BoundingBox, and without a default
        // member initialiser each of them is a `-Wmissing-field-initializers` asking whether the author
        // MEANT to stop. Stated once here, where the answer is.
        std::vector<LODRange> LODs = {};

        // Pre-baked LOD triangle sets (cooked at import, submesh-local, coarsest last). When non-empty,
        // BuildLODIndexBuffer APPENDS these instead of generating LODs — so cooked assets skip the load-time
        // meshopt pass. Transient input to StaticMesh; not itself the LODRange chain above.
        std::vector<std::vector<Index>> BakedLODs = {};
    };

    enum class MeshType
    {
        Static,
        Skinned
    };
} // namespace Desert