// DesertAsset {"Kind":"Shader","Guid":"fc20876241954ba6b7f37c22816490a9","Versions":{"SHDR":1},"Dependencies":[]}
// THE NEAR FIELD'S OBJECT CULL, STEPS 2 AND 4 (UE LumenMeshSDFCulling.usf CullMeshSDFObjectsToViewGridCS and
// CompactCulledObjectsCS, written from memory): one thread per froxel of the grid (Common/MeshSDFGrid.glslh) walks
// step 1's candidates and keeps every object whose world box lies within DF_MESH_SDF_TRACE_DISTANCE of the froxel's
// world box - every object a ray starting in the froxel reaches in its near field, no cap. Dispatched twice:
// CullPass 0 COUNTS them into the cell's header, MeshSDFCullGridOffsets.shader (step 3) sums the counts into each
// list's first index, CullPass 1 SCATTERS the same objects, in the candidates' order, with their sort keys (the squared
// distance from the froxel's centre to the object's box) into the unsorted pairs at the list's place, and
// MeshSDFCullGridSort.shader (step 5) writes each list closest first, as the capped lists were. UE appends
// (object, cell) pairs in its count pass and compacts those; recounting here needs no pair buffer sized by a guess,
// and the second walk costs what the first did (the boxes come through shared memory 64 at a time either way).
Shader "MeshSDFCullGrid"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/MeshSDFGrid.glslh>

        ReadBuffer(0) MeshSDFCullParamsUB
        {
            MeshSDFCullParams CullParams;
        };
        ReadBuffer(1) DFObjects
        {
            vec4 DistanceFieldObjects[];
        };
        ReadBuffer(2) MeshSDFCulledObjectsUB
        {
            uint CulledData[];
        };
        // The cells' headers (MeshSDFGridHeaderStride uints each: the list's first index, its length), then the lists.
        Buffer(3) MeshSDFGridCellsUB
        {
            uint GridCellsData[];
        };
        // Scatter: (object index, sort key bits) per list entry, at the entry's index less the headers.
        Buffer(4) MeshSDFGridUnsortedUB
        {
            uvec2 UnsortedData[];
        };
        PushConstant MeshSDFCullGridPush
        {
            uint CullPass; // 0 = count into the headers, 1 = scatter into the lists
        };

        const uint kTile = 64u;

        shared vec4 s_Min[kTile]; // xyz = the box's minimum, w = the object index (bits)
        shared vec3 s_Max[kTile];

        // The world box of cell @p cell's froxel: its pixel rectangle at the slice's two depths.
        void CellBox(uint cell, out vec3 boxMin, out vec3 boxMax)
        {
            const MeshSDFGrid grid = CullParams.grid;
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
                const float vz    = (depth - CullParams.clipDepth.w) / CullParams.clipDepth.z;
                const float zc    = CullParams.clipDepth.x * vz + CullParams.clipDepth.y;
                for (int k = 0; k < 4; ++k)
                {
                    const vec2 ndc   = vec2((k & 1) != 0 ? n1.x : n0.x, (k & 2) != 0 ? n1.y : n0.y);
                    const vec4 world = CullParams.clipToWorld * vec4(ndc * depth, zc, depth);
                    const vec3 p     = world.xyz / world.w;
                    boxMin = min(boxMin, p);
                    boxMax = max(boxMax, p);
                }
            }
        }

        void main()
        {
            const uint cell  = gl_GlobalInvocationID.x;
            const bool valid = cell < CullParams.grid.cells.w;
            vec3 boxMin = vec3(0.0);
            vec3 boxMax = vec3(0.0);
            if (valid)
                CellBox(cell, boxMin, boxMax);
            const vec3  centre  = 0.5 * (boxMin + boxMax);
            const float reach2  = DF_MESH_SDF_TRACE_DISTANCE * DF_MESH_SDF_TRACE_DISTANCE;
            const uint  header  = cell * MeshSDFGridHeaderStride;
            const bool  scatter = CullPass != 0u;
            // Scatter: the list step 3 placed (empty when it refused the frame's lists).
            const uint first  = scatter && valid ? GridCellsData[header] - CullParams.grid.cells.w * MeshSDFGridHeaderStride : 0u;
            const uint length = scatter && valid ? GridCellsData[header + 1u] : 0u;

            uint       n     = 0u;
            const uint total = CulledData[0];
            for (uint base = 0u; base < total; base += kTile)
            {
                const uint i = base + gl_LocalInvocationIndex;
                if (i < total)
                {
                    const uint o      = CulledData[1u + i];
                    const uint record = o * uint(DF_OBJECT_STRIDE);
                    s_Min[gl_LocalInvocationIndex] = vec4(DistanceFieldObjects[record + uint(DF_OBJECT_ROW_BOUNDS_MIN)].xyz, uintBitsToFloat(o));
                    s_Max[gl_LocalInvocationIndex] = DistanceFieldObjects[record + uint(DF_OBJECT_ROW_BOUNDS_MAX)].xyz;
                }
                barrier();
                if (valid && (!scatter || n < length))
                {
                    const uint count = min(kTile, total - base);
                    for (uint k = 0u; k < count; ++k)
                    {
                        const vec3 gap = max(max(s_Min[k].xyz - boxMax, boxMin - s_Max[k]), vec3(0.0));
                        if (dot(gap, gap) > reach2)
                            continue;
                        if (scatter)
                        {
                            if (n == length)
                                break;
                            const vec3 toCentre = max(max(s_Min[k].xyz - centre, centre - s_Max[k]), vec3(0.0));
                            UnsortedData[first + n] = uvec2(floatBitsToUint(s_Min[k].w), floatBitsToUint(dot(toCentre, toCentre)));
                        }
                        ++n;
                    }
                }
                barrier();
            }
            if (valid && !scatter)
            {
                GridCellsData[header]      = 0u;
                GridCellsData[header + 1u] = n;
            }
        }
    }
}
