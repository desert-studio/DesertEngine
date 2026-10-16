// DesertAsset {"Kind":"Shader","Guid":"aff3df0d8fe6417dbbeff507c4e32b73","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkinCache"
{
    Compute
    {
        // "SkinCache: Skin" (RayTracingSkinCache.cpp). Port of UE GpuSkinCacheComputeShader.usf (SkinCacheUpdateBatchCS):
        // one invocation per vertex of a skinned mesh skins its bind-pose position and tangent frame by the primitive's
        // palette and writes them, in MESH space, into the entry's streams (RayTracingSkinCache.hpp layout): current
        // positions [0,3V) (the BLAS input), previous positions [3V,6V), normal/tangent/bitangent [6V,15V). The
        // raster view passes fetch them (Mesh/Surface/Vertex_Skinned.glslh); the TLAS instance carries the world. The skin is the
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

        // x = vertex count V, y = the first bone of this frame's palette in SkinCacheBones (the row's BoneOffset),
        // z = 1 when the current positions hold last frame's pose (rotate them into the previous positions).
        Buffer(2) SkinCacheDispatch
        {
            uvec4 u_Dispatch;
        };

        Buffer(3) SkinCacheStreams
        {
            float u_Streams[];
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
            // Unnormalised, exactly what Vertex_Skinned's skin3 * a_Normal is: the view pass applies its normal
            // matrix and normalises after the fetch.
            const mat3 skin3 = mat3(skin);
            const vec3 normal    = skin3 * vec3(uintBitsToFloat(u_Vertices[first + 3u]), uintBitsToFloat(u_Vertices[first + 4u]),
                                                uintBitsToFloat(u_Vertices[first + 5u]));
            const vec3 tangent   = skin3 * vec3(uintBitsToFloat(u_Vertices[first + 6u]), uintBitsToFloat(u_Vertices[first + 7u]),
                                                uintBitsToFloat(u_Vertices[first + 8u]));
            const vec3 bitangent = skin3 * vec3(uintBitsToFloat(u_Vertices[first + 9u]), uintBitsToFloat(u_Vertices[first + 10u]),
                                                uintBitsToFloat(u_Vertices[first + 11u]));

            const uint count = u_Dispatch.x;
            const uint cur   = vertex * 3u;
            const uint prev  = count * 3u + vertex * 3u;
            const uint frame = count * 6u + vertex * 9u;
            // This invocation alone touches its vertex's slots: read last frame's pose before overwriting it.
            const vec3 previous = u_Dispatch.z != 0u ? vec3(u_Streams[cur + 0u], u_Streams[cur + 1u], u_Streams[cur + 2u])
                                                     : posed;
            u_Streams[prev + 0u] = previous.x;
            u_Streams[prev + 1u] = previous.y;
            u_Streams[prev + 2u] = previous.z;
            u_Streams[cur + 0u]  = posed.x;
            u_Streams[cur + 1u]  = posed.y;
            u_Streams[cur + 2u]  = posed.z;
            u_Streams[frame + 0u] = normal.x;
            u_Streams[frame + 1u] = normal.y;
            u_Streams[frame + 2u] = normal.z;
            u_Streams[frame + 3u] = tangent.x;
            u_Streams[frame + 4u] = tangent.y;
            u_Streams[frame + 5u] = tangent.z;
            u_Streams[frame + 6u] = bitangent.x;
            u_Streams[frame + 7u] = bitangent.y;
            u_Streams[frame + 8u] = bitangent.z;
        }
    }
}
