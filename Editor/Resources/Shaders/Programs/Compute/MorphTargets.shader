// DesertAsset {"Kind":"Shader","Guid":"84bc3bad2c69419ebfd7eec9712f39bc","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MorphTargets"
{
    Compute
    {
        // "Mesh: Morph {entity}" (MeshRendererMorph.cpp). Port of UE GPUMorphUpdateCS (MorphTargets.usf): one
        // invocation per vertex of a skinned mesh blends base + sum(weight_k * delta_k) for the position and the
        // normal (renormalised, as Geometry::ApplyMorphTargets - the CPU twin - does) and writes the whole vertex
        // into the component's morphed vertex buffer, which every pass then skins exactly like the base one. UE
        // accumulates sparse deltas into a delta stream the vertex factory adds; the deltas here are dense per
        // target and the output is the full vertex, so the skinning shaders read it as the base one; last frame's
        // positions go to MorphPrevious for velocity.
        LocalSize(64, 1, 1);

        // Desert::SkinnedVertex, std430 as 22 words: Position 0-2, Normal 3-5, Tangent 6-8, Bitangent 9-11,
        // TexCoord 12-13, BoneIDs 14-17, BoneWeights 18-21 (sizeof 88, Geometry/MeshTypes.hpp).
        Buffer(0) MorphBaseVertices
        {
            uint BaseVertices[];
        };

        // SkinnedMesh::GetMorphDeltas: target k's delta of vertex i at [(k * VertexCount + i) * 2] (position)
        // and [... + 1] (normal).
        Buffer(1) MorphDeltas
        {
            vec4 Deltas[];
        };

        // x = VertexCount, y = the number of active targets, z = the mesh's target count, w = what MorphPrevious
        // receives (MeshRendererMorph HistoryMode): 1 = the output holds the shape the component was drawn with
        // last frame, moved there before the overwrite; 0 = it does not (a new twin, one not drawn last frame),
        // the new shape goes into both; 2 = nothing (the skin cache keeps the history, MorphPrevious is scratch);
        // then per active target (target index, weight bits).
        Buffer(2) MorphWeights
        {
            uvec4 MorphHeader;
            uvec2 Active[];
        };

        Buffer(3) MorphOutput
        {
            uint MorphedVertices[];
        };

        // THE PREVIOUS POSITIONS (UE FMorphVertexBufferPool's double buffer, read as PreviousMorph): float3 per
        // vertex, 3 words - binding 3 of the skinned vertex format reads it at 12 bytes a vertex
        // (ShaderProtocols/VertexInputs.hpp kMeshPreviousPositionStride). Each invocation moves its vertex's
        // output position here before overwriting it, as the skin cache rotates its streams; without a history
        // both get the new shape, so a fresh twin carries camera and bone motion only.
        Buffer(4) MorphPrevious
        {
            uint PreviousVertices[];
        };

        void main()
        {
            const uint vertex      = gl_GlobalInvocationID.x;
            const uint vertexCount = MorphHeader.x;
            if (vertex >= vertexCount)
                return;

            const uint first    = vertex * 22u;
            const uint previous = vertex * 3u;
            const uint mode     = MorphHeader.w;
            if (mode == 1u)
            {
                PreviousVertices[previous + 0u] = MorphedVertices[first + 0u];
                PreviousVertices[previous + 1u] = MorphedVertices[first + 1u];
                PreviousVertices[previous + 2u] = MorphedVertices[first + 2u];
            }
            for (uint w = 0u; w < 22u; ++w)
                MorphedVertices[first + w] = BaseVertices[first + w];

            vec3 position = vec3(uintBitsToFloat(BaseVertices[first + 0u]), uintBitsToFloat(BaseVertices[first + 1u]),
                                 uintBitsToFloat(BaseVertices[first + 2u]));
            vec3 normal   = vec3(uintBitsToFloat(BaseVertices[first + 3u]), uintBitsToFloat(BaseVertices[first + 4u]),
                                 uintBitsToFloat(BaseVertices[first + 5u]));
            for (uint a = 0u; a < MorphHeader.y; ++a)
            {
                const uint  target = Active[a].x;
                const float weight = uintBitsToFloat(Active[a].y);
                const uint  delta  = (target * vertexCount + vertex) * 2u;
                position += weight * Deltas[delta].xyz;
                normal   += weight * Deltas[delta + 1u].xyz;
            }
            const float length2 = dot(normal, normal);
            if (length2 > 1e-12)
                normal *= inversesqrt(length2);

            MorphedVertices[first + 0u] = floatBitsToUint(position.x);
            MorphedVertices[first + 1u] = floatBitsToUint(position.y);
            MorphedVertices[first + 2u] = floatBitsToUint(position.z);
            MorphedVertices[first + 3u] = floatBitsToUint(normal.x);
            MorphedVertices[first + 4u] = floatBitsToUint(normal.y);
            MorphedVertices[first + 5u] = floatBitsToUint(normal.z);
            if (mode == 0u)
            {
                PreviousVertices[previous + 0u] = floatBitsToUint(position.x);
                PreviousVertices[previous + 1u] = floatBitsToUint(position.y);
                PreviousVertices[previous + 2u] = floatBitsToUint(position.z);
            }
        }
    }
}
