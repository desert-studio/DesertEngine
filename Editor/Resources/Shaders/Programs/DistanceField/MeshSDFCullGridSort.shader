// DesertAsset {"Kind":"Shader","Guid":"12b14a3cac4d4c0b8e0b9e1b8b6d7637","Versions":{"SHDR":1},"Dependencies":[]}
// THE NEAR FIELD'S OBJECT CULL, STEP 5: each froxel's list written CLOSEST FIRST. MeshSDFCullGrid.shader's scatter
// left the list in the candidates' order (step 1's atomic append: arbitrary, different every frame) as (object, key)
// pairs, the key the squared distance from the froxel's centre to the object's box. One workgroup per cell ranks
// every entry - its rank the number of entries whose (key, object) is smaller, unique since an object is listed once
// per cell - and writes the object at that rank into the cell's list in the grid buffer. Any length: the keys come
// through shared memory a tile at a time.
//
// UE (LumenSoftwareRayTracing.ush ConeTraceMeshSDFsAndInterpolateFromCards) walks its compacted lists in append
// order. The order never changes the hit - the closest wins, every object is traced up to the closest hit so far
// and the foliage dither is seeded per object - but it sets the cost: a near hit found first clips every later
// object's ray to an empty box test (GITraceMeshObject), where in append order the far objects are marched first.
Shader "MeshSDFCullGridSort"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/MeshSDFGrid.glslh>

        ReadBuffer(0) MeshSDFCullParamsUB
        {
            MeshSDFCullParams u_Cull;
        };
        Buffer(1) MeshSDFGridCellsUB
        {
            uint u_GridCells[];
        };
        // MeshSDFCullGrid.shader's scatter: (object index, key bits) per entry, at the entry's index less the headers.
        ReadBuffer(2) MeshSDFGridUnsortedUB
        {
            uvec2 u_Unsorted[];
        };

        const uint kTile = 64u;

        shared uvec2 s_Pair[kTile];

        // (key, object) of @p a before that of @p b. The keys are non-negative floats: their bits order as they do.
        bool Before(uvec2 a, uvec2 b)
        {
            return a.y < b.y || (a.y == b.y && a.x < b.x);
        }

        void main()
        {
            // One workgroup per cell; the dispatch is 2D when the cells exceed one dimension's group count.
            const uint cells = u_Cull.grid.cells.w;
            const uint cell  = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x;
            if (cell >= cells)
                return;
            const uint header = cell * MeshSDFGridHeaderStride;
            const uint length = u_GridCells[header + 1u];
            if (length == 0u)
                return;
            const uint first = u_GridCells[header];
            const uint src   = first - cells * MeshSDFGridHeaderStride;
            const uint t     = gl_LocalInvocationIndex;

            for (uint iBase = 0u; iBase < length; iBase += kTile)
            {
                const uint  i    = iBase + t;
                const bool  mine = i < length;
                const uvec2 pair = mine ? u_Unsorted[src + i] : uvec2(0u);
                uint rank = 0u;
                for (uint jBase = 0u; jBase < length; jBase += kTile)
                {
                    const uint j = jBase + t;
                    if (j < length)
                        s_Pair[t] = u_Unsorted[src + j];
                    barrier();
                    const uint count = min(kTile, length - jBase);
                    for (uint k = 0u; k < count; ++k)
                        rank += Before(s_Pair[k], pair) ? 1u : 0u;
                    barrier();
                }
                if (mine)
                    u_GridCells[first + rank] = pair.x;
            }
        }
    }
}
