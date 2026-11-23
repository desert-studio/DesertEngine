// DesertAsset {"Kind":"Shader","Guid":"ad033fdac25047f6ab434930db8c20b1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "FoliageWindRayTracing"
{
    Compute
    {
        // "RayTracingScene: Foliage Wind" (RayTracingFoliage.cpp; RT-FOLIAGE-BLAS). UE: a WPO primitive's ray-tracing
        // geometry evaluated with its world-position offset (r.RayTracing.Geometry.SupportsWPO) within a distance
        // cutoff. One binding block and one dispatch per swaying ISM / foliage instance the frame's refit budget
        // took: every vertex of the instance's RT-LOD BLAS input is copied from the mesh's own vertex buffer with
        // its position moved by THE RASTER'S WIND - Common/FoliageWind.glslh FoliageWindOffset at the instance's
        // origin, with this view's gust (cameraUB.WindGust) and local air (LocalWindXZ at the origin), the very
        // call Mesh/Surface/Vertex_Instanced.glslh InstancedWorldPosition makes - and brought back into the
        // instance's mesh space (the TLAS instance carries the model matrix, as for the rest-pose BLAS). The RT
        // scene's "RayTracingScene.DynamicGeometry" pass then refits the instance's BLAS from these vertices.
        //
        // Output, per instance: Desert::Vertex (14 words: position 0-2, normal 3-5, tangent 6-8, bitangent 9-11,
        // uv 12-13; Geometry/MeshTypes.hpp) for every vertex of the mesh, in the mesh's order, so the RT-LOD's
        // segments address them unchanged; and, when the target was (re)made, the RT-LOD's triangle indices,
        // segment after segment (FoliageWindCopies), unchanged values (the build adds each segment's FirstVertex).
        // Normals stay the rest pose's (a sway of a few degrees; the hit reads them for shading only).
        LocalSize(64, 1, 1);

        #include <Common/LocalWind.glslh>
        #include <Common/FoliageWind.glslh>

        // RayTracingFoliage.cpp FoliageWindJobGpu, kFoliageWindJobRows uvec4 per job:
        //   [0] source vertex address lo/hi (the mesh's vertex 0), source index address lo/hi (the mesh's index 0)
        //   [1] x = vertex count, y = first copy, z = copy count, w = index count of the whole job
        //   [2..5] the instance's model matrix, column by column (float bits)
        //   [6] windA (dir.x, dir.z, Strength cm, Height cm)   [7] windB (Speed Hz, Seconds, 0, 0)
        ReadBuffer(1) FoliageWindJobs
        {
            uvec4 JobRows[];
        };

        // One per segment of a job's BLAS: x = the segment's first index in the source index buffer (uint units),
        // y = its first index in the target's index buffer, z = its index count.
        ReadBuffer(2) FoliageWindCopies
        {
            uvec4 Copies[];
        };

        Buffer(3) FoliageWindVertexData
        {
            uint OutVertices[];
        };

        Buffer(4) FoliageWindIndexData
        {
            uint OutIndices[];
        };

        // Mirrors ShaderPush::FoliageWindRayTracing (generated): x = the job, y = 1 when the indices are written
        // too (the target was made or remade this frame; a refit keeps the indices its build read).
        PushConstant PushConstants
        {
            uvec4 Job;
        };

        layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer FoliageWindSourceWords
        {
            uint Words[];
        };

        const uint kFoliageWindJobRows    = 8u;
        const uint kFoliageWindVertexWords = 14u;

        void main()
        {
            const uint  base       = Job.x * kFoliageWindJobRows;
            const uvec4 sources    = JobRows[base + 0u];
            const uvec4 counts     = JobRows[base + 1u];
            const uint  invocation = gl_GlobalInvocationID.x;

            if (invocation < counts.x)
            {
                FoliageWindSourceWords       vertices = FoliageWindSourceWords(uvec2(sources.x, sources.y));
                const uint                   first    = invocation * kFoliageWindVertexWords;
                const mat4 model = mat4(uintBitsToFloat(JobRows[base + 2u]), uintBitsToFloat(JobRows[base + 3u]),
                                        uintBitsToFloat(JobRows[base + 4u]), uintBitsToFloat(JobRows[base + 5u]));
                const vec4 windA = uintBitsToFloat(JobRows[base + 6u]);
                const vec4 windB = uintBitsToFloat(JobRows[base + 7u]);
                const vec3 local = vec3(uintBitsToFloat(vertices.Words[first + 0u]),
                                        uintBitsToFloat(vertices.Words[first + 1u]),
                                        uintBitsToFloat(vertices.Words[first + 2u]));
                const vec3 origin = vec3(model[3]);
                // THE raster's offset (InstancedWorldPosition = model * local + this), then into mesh space.
                const vec3 offset = FoliageWindOffset(local, origin, windA, windB, cameraUB.WindGust,
                                                      LocalWindXZ(origin, false));
                const vec3 moved  = local + inverse(mat3(model)) * offset;
                OutVertices[first + 0u] = floatBitsToUint(moved.x);
                OutVertices[first + 1u] = floatBitsToUint(moved.y);
                OutVertices[first + 2u] = floatBitsToUint(moved.z);
                for (uint word = 3u; word < kFoliageWindVertexWords; ++word)
                    OutVertices[first + word] = vertices.Words[first + word];
            }

            if (Job.y == 0u || invocation >= counts.w)
                return;
            FoliageWindSourceWords indices = FoliageWindSourceWords(uvec2(sources.z, sources.w));
            uint                         before  = 0u;
            for (uint copy = 0u; copy < counts.z; ++copy)
            {
                const uvec4 range = Copies[counts.y + copy];
                if (invocation < before + range.z)
                {
                    const uint at = invocation - before;
                    OutIndices[range.y + at] = indices.Words[range.x + at];
                    return;
                }
                before += range.z;
            }
        }
    }
}
