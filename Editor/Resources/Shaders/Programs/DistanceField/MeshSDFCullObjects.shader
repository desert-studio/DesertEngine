// DesertAsset {"Kind":"Shader","Guid":"6abadd029a4f4c45964aa1ab4e1fee4d","Versions":{"SHDR":1},"Dependencies":[]}
// THE NEAR FIELD'S OBJECT CULL, STEP 1 (UE LumenMeshSDFCulling.usf CullMeshSDFObjectsForViewCS, written from memory):
// the placed objects whose world box comes within DF_MESH_SDF_TRACE_DISTANCE of the view's frustum out to
// DF_MESH_SDF_GRID_MAX_DISTANCE, and whose field has a resident mip, appended to one list - the candidates of
// MeshSDFCullGrid.shader. One workgroup walks every object: the list's count needs no clear and no second dispatch,
// and the order the threads append in is irrelevant (the grid keeps each cell's closest by distance, then index).
Shader "MeshSDFCullObjects"
{
    Compute
    {
        LocalSize(256, 1, 1);

        #include <Common/MeshSDFGrid.glslh>

        ReadBuffer(0) MeshSDFCullParamsUB
        {
            MeshSDFCullParams u_Cull;
        };
        ReadBuffer(1) DFObjects
        {
            vec4 u_DFObjects[];
        };
        ReadBuffer(2) DFAssetData
        {
            vec4 u_DFAssetData[];
        };
        // The count, then that many object indices.
        Buffer(3) MeshSDFCulledObjectsUB
        {
            uint u_Culled[];
        };

        shared uint s_Count;

        void main()
        {
            if (gl_LocalInvocationIndex == 0u)
                s_Count = 0u;
            barrier();
            for (uint o = gl_LocalInvocationIndex; o < u_Cull.counts.x; o += 256u)
            {
                const uint base  = o * uint(DF_OBJECT_STRIDE);
                const uint asset = floatBitsToUint(u_DFObjects[base + 4u].x);
                if (floatBitsToUint(u_DFAssetData[asset * uint(DF_ASSET_STRIDE)].x) == 0u)
                    continue; // no resident mip: the trace has nothing to march
                const vec3 lo     = u_DFObjects[base + uint(DF_OBJECT_ROW_BOUNDS_MIN)].xyz;
                const vec3 hi     = u_DFObjects[base + uint(DF_OBJECT_ROW_BOUNDS_MAX)].xyz;
                const vec3 centre = 0.5 * (lo + hi);
                const vec3 half_  = 0.5 * (hi - lo);
                bool       near   = true;
                for (int p = 0; p < 6 && near; ++p)
                {
                    const vec4 plane = u_Cull.planes[p];
                    near = dot(plane.xyz, centre) + plane.w + dot(abs(plane.xyz), half_) >= -DF_MESH_SDF_TRACE_DISTANCE;
                }
                if (near)
                    u_Culled[1u + atomicAdd(s_Count, 1u)] = o;
            }
            barrier();
            if (gl_LocalInvocationIndex == 0u)
                u_Culled[0] = s_Count;
        }
    }
}
