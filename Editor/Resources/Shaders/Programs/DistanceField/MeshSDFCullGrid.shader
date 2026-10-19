// DesertAsset {"Kind":"Shader","Guid":"fc20876241954ba6b7f37c22816490a9","Versions":{"SHDR":1},"Dependencies":[]}
// THE NEAR FIELD'S OBJECT CULL, STEP 2 (UE LumenMeshSDFCulling.usf CullMeshSDFObjectsToViewGridCS, written from
// memory): one thread per froxel of the grid (Common/MeshSDFGrid.glslh) keeps, of step 1's candidates, the objects
// whose world box lies within DF_MESH_SDF_TRACE_DISTANCE of the froxel's world box - every object a ray starting in
// the froxel reaches in its near field - at most DF_MESH_SDF_GRID_MAX_OBJECTS_PER_CELL of them, the closest to the
// froxel's centre (ties by object index: the list does not depend on step 1's append order), in that order. The
// candidates pass through shared memory 64 at a time. UE compacts the lists; here each cell owns a fixed block.
Shader "MeshSDFCullGrid"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/MeshSDFGrid.glslh>

        ReadBuffer(0) MeshSDFCullParamsUB
        {
            MeshSDFCullParams u_Cull;
        };
        ReadBuffer(1) DFObjects
        {
            vec4 u_DFObjects[];
        };
        ReadBuffer(2) MeshSDFCulledObjectsUB
        {
            uint u_Culled[];
        };
        // Per cell MeshSDFGridCellStride uints: the count, then the object indices.
        Buffer(3) MeshSDFGridCellsUB
        {
            uint u_GridCells[];
        };

        const uint kTile = 64u;
        const uint kCap  = uint(DF_MESH_SDF_GRID_MAX_OBJECTS_PER_CELL);

        shared vec4 s_Min[kTile]; // xyz = the box's minimum, w = the object index (bits)
        shared vec3 s_Max[kTile];

        // The world box of cell @p cell's froxel: its pixel rectangle at the slice's two depths.
        void CellBox(uint cell, out vec3 boxMin, out vec3 boxMax)
        {
            const MeshSDFGrid grid = u_Cull.grid;
            const uvec3 c   = uvec3(cell % grid.cells.x, (cell / grid.cells.x) % grid.cells.y,
                                    cell / (grid.cells.x * grid.cells.y));
            const vec2  px0 = vec2(c.xy) * grid.zDistribution.w;
            const vec2  px1 = min(vec2(c.xy + 1u) * grid.zDistribution.w, grid.screen.xy);
            const vec2  n0  = px0 * grid.screen.zw * 2.0 - 1.0;
            const vec2  n1  = px1 * grid.screen.zw * 2.0 - 1.0;
            boxMin = vec3(3.0e38);
            boxMax = vec3(-3.0e38);
            for (int s = 0; s < 2; ++s)
            {
                const float depth = max(MeshSDFGridSliceDepth(grid, float(c.z + uint(s))), 1.0);
                const float vz    = (depth - u_Cull.clipDepth.w) / u_Cull.clipDepth.z;
                const float zc    = u_Cull.clipDepth.x * vz + u_Cull.clipDepth.y;
                for (int k = 0; k < 4; ++k)
                {
                    const vec2 ndc   = vec2((k & 1) != 0 ? n1.x : n0.x, (k & 2) != 0 ? n1.y : n0.y);
                    const vec4 world = u_Cull.clipToWorld * vec4(ndc * depth, zc, depth);
                    const vec3 p     = world.xyz / world.w;
                    boxMin = min(boxMin, p);
                    boxMax = max(boxMax, p);
                }
            }
        }

        void main()
        {
            const uint cell  = gl_GlobalInvocationID.x;
            const bool valid = cell < u_Cull.grid.cells.w;
            vec3 boxMin = vec3(0.0);
            vec3 boxMax = vec3(0.0);
            if (valid)
                CellBox(cell, boxMin, boxMax);
            const vec3  centre = 0.5 * (boxMin + boxMax);
            const float reach2 = DF_MESH_SDF_TRACE_DISTANCE * DF_MESH_SDF_TRACE_DISTANCE;

            float keys[DF_MESH_SDF_GRID_MAX_OBJECTS_PER_CELL];
            uint  ids[DF_MESH_SDF_GRID_MAX_OBJECTS_PER_CELL];
            uint  n     = 0u;
            const uint total = u_Culled[0];
            for (uint first = 0u; first < total; first += kTile)
            {
                const uint i = first + gl_LocalInvocationIndex;
                if (i < total)
                {
                    const uint o    = u_Culled[1u + i];
                    const uint base = o * uint(DF_OBJECT_STRIDE);
                    s_Min[gl_LocalInvocationIndex] = vec4(u_DFObjects[base + uint(DF_OBJECT_ROW_BOUNDS_MIN)].xyz, uintBitsToFloat(o));
                    s_Max[gl_LocalInvocationIndex] = u_DFObjects[base + uint(DF_OBJECT_ROW_BOUNDS_MAX)].xyz;
                }
                barrier();
                if (valid)
                {
                    const uint count = min(kTile, total - first);
                    for (uint k = 0u; k < count; ++k)
                    {
                        const vec3 lo  = s_Min[k].xyz;
                        const vec3 hi  = s_Max[k];
                        const vec3 gap = max(max(lo - boxMax, boxMin - hi), vec3(0.0));
                        if (dot(gap, gap) > reach2)
                            continue;
                        const vec3  toCentre = max(max(lo - centre, centre - hi), vec3(0.0));
                        const float key      = dot(toCentre, toCentre);
                        const uint  id       = floatBitsToUint(s_Min[k].w);
                        if (n == kCap && (key > keys[kCap - 1u] || (key == keys[kCap - 1u] && id > ids[kCap - 1u])))
                            continue;
                        uint j = n < kCap ? n++ : kCap - 1u;
                        while (j > 0u && (keys[j - 1u] > key || (keys[j - 1u] == key && ids[j - 1u] > id)))
                        {
                            keys[j] = keys[j - 1u];
                            ids[j]  = ids[j - 1u];
                            --j;
                        }
                        keys[j] = key;
                        ids[j]  = id;
                    }
                }
                barrier();
            }
            if (!valid)
                return;
            const uint block = cell * MeshSDFGridCellStride;
            u_GridCells[block] = n;
            for (uint i = 0u; i < n; ++i)
                u_GridCells[block + 1u + i] = ids[i];
        }
    }
}
