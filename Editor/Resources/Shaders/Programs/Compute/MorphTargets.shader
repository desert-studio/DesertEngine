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
        // target and the output is the full vertex, so the skinning shaders do not change at all.
        LocalSize(64, 1, 1);

        // Desert::SkinnedVertex, std430 as 22 words: Position 0-2, Normal 3-5, Tangent 6-8, Bitangent 9-11,
        // TexCoord 12-13, BoneIDs 14-17, BoneWeights 18-21 (sizeof 88, Geometry/MeshTypes.hpp).
        Buffer(0) MorphBaseVertices
        {
            uint u_Base[];
        };

        // SkinnedMesh::GetMorphDeltas: target k's delta of vertex i at [(k * VertexCount + i) * 2] (position)
        // and [... + 1] (normal).
        Buffer(1) MorphDeltas
        {
            vec4 u_Deltas[];
        };

        // x = VertexCount, y = the number of active targets, then per active target (target index, weight bits).
        Buffer(2) MorphWeights
        {
            uvec4 u_Header;
            uvec2 u_Active[];
        };

        Buffer(3) MorphOutput
        {
            uint u_Out[];
        };

        void main()
        {
            const uint vertex      = gl_GlobalInvocationID.x;
            const uint vertexCount = u_Header.x;
            if (vertex >= vertexCount)
                return;

            const uint first = vertex * 22u;
            for (uint w = 0u; w < 22u; ++w)
                u_Out[first + w] = u_Base[first + w];

            vec3 position = vec3(uintBitsToFloat(u_Base[first + 0u]), uintBitsToFloat(u_Base[first + 1u]),
                                 uintBitsToFloat(u_Base[first + 2u]));
            vec3 normal   = vec3(uintBitsToFloat(u_Base[first + 3u]), uintBitsToFloat(u_Base[first + 4u]),
                                 uintBitsToFloat(u_Base[first + 5u]));
            for (uint a = 0u; a < u_Header.y; ++a)
            {
                const uint  target = u_Active[a].x;
                const float weight = uintBitsToFloat(u_Active[a].y);
                const uint  delta  = (target * vertexCount + vertex) * 2u;
                position += weight * u_Deltas[delta].xyz;
                normal   += weight * u_Deltas[delta + 1u].xyz;
            }
            const float length2 = dot(normal, normal);
            if (length2 > 1e-12)
                normal *= inversesqrt(length2);

            u_Out[first + 0u] = floatBitsToUint(position.x);
            u_Out[first + 1u] = floatBitsToUint(position.y);
            u_Out[first + 2u] = floatBitsToUint(position.z);
            u_Out[first + 3u] = floatBitsToUint(normal.x);
            u_Out[first + 4u] = floatBitsToUint(normal.y);
            u_Out[first + 5u] = floatBitsToUint(normal.z);
        }
    }
}
