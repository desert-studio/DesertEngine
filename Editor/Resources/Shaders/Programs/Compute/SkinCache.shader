// DesertAsset {"Kind":"Shader","Guid":"aff3df0d8fe6417dbbeff507c4e32b73","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkinCache"
{
    Compute
    {
        // "SkinCache: Positions" (RayTracingSkinCache.cpp). Port of UE GpuSkinCacheComputeShader.usf (SkinCacheUpdateBatchCS)
        // reduced to what the ray-tracing BLAS reads: one invocation per vertex of a skinned mesh skins its bind-pose
        // position by the primitive's palette and writes it, in MESH space (the TLAS instance carries the world), as
        // one float3 per vertex in the mesh's own vertex order (RayTracing::kDeformedPositionLayout). The skin is the
        // same four-influence sum Mesh/Surface/Vertex_Skinned.glslh evaluates, from the same palette (the view's
        // ObjectBones at the row's BoneOffset), so a ray and the raster see one pose.
        LocalSize(64, 1, 1);

        // The view's ObjectBones: both frames' palettes of every skinned primitive, end to end (ObjectMotion.glslh).
        Buffer(0) SkinCacheBones
        {
            mat4 u_Palettes[];
        };

        // Desert::SkinnedVertex, std430 as 22 words: Position 0-2, Normal 3-5, Tangent 6-8, Bitangent 9-11,
        // TexCoord 12-13, BoneIDs 14-17, BoneWeights 18-21 (sizeof 88, Geometry/MeshTypes.hpp).
        Buffer(1) SkinCacheVertices
        {
            uint u_Vertices[];
        };

        // x = vertex count, y = the first bone of this frame's palette in SkinCacheBones (the row's BoneOffset).
        Buffer(2) SkinCacheDispatch
        {
            uvec4 u_Dispatch;
        };

        Buffer(3) SkinCachePositions
        {
            float u_Positions[];
        };

        void main()
        {
            const uint vertex = gl_GlobalInvocationID.x;
            if (vertex >= u_Dispatch.x)
                return;

            const uint  first    = vertex * 22u;
            const vec3  position = vec3(uintBitsToFloat(u_Vertices[first + 0u]), uintBitsToFloat(u_Vertices[first + 1u]),
                                        uintBitsToFloat(u_Vertices[first + 2u]));
            const uvec4 bones    = uvec4(u_Vertices[first + 14u], u_Vertices[first + 15u], u_Vertices[first + 16u],
                                         u_Vertices[first + 17u]);
            const vec4  weights  = vec4(uintBitsToFloat(u_Vertices[first + 18u]), uintBitsToFloat(u_Vertices[first + 19u]),
                                        uintBitsToFloat(u_Vertices[first + 20u]), uintBitsToFloat(u_Vertices[first + 21u]));

            const uint b    = u_Dispatch.y;
            const mat4 skin = u_Palettes[b + bones.x] * weights.x + u_Palettes[b + bones.y] * weights.y +
                              u_Palettes[b + bones.z] * weights.z + u_Palettes[b + bones.w] * weights.w;
            const vec3 posed = (skin * vec4(position, 1.0)).xyz;

            u_Positions[vertex * 3u + 0u] = posed.x;
            u_Positions[vertex * 3u + 1u] = posed.y;
            u_Positions[vertex * 3u + 2u] = posed.z;
        }
    }
}
