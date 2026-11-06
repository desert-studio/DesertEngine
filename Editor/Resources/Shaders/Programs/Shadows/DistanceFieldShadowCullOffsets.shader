// DesertAsset {"Kind":"Shader","Guid":"054097880a2a4c90a7dd0e118151fb56","Versions":{"SHDR":1},"Dependencies":[]}
// "SHADOWS: DISTANCE FIELD SUN", THE TILE LISTS' OFFSETS: the exclusive prefix sum of the tiles' list lengths
// (DistanceFieldShadowCull.shader's count pass), as the near field's (MeshSDFCullGridOffsets.shader) - each tile's
// header gets the first index of its list, the lists following the headers tile after tile. One workgroup: each
// thread sums a run of tiles, the runs' sums are scanned in shared memory, each thread writes its run's offsets.
// When the lists need more entries than the buffer has room for (counts.w) the frame's lists are REFUSED - every
// tile left empty, no object traced this frame - never cut short.
Shader "DistanceFieldShadowCullOffsets"
{
    Compute
    {
        LocalSize(256, 1, 1);

        #include <Common/DistanceFieldShadow.glslh>

        ReadBuffer(0) DistanceFieldShadowParamsUB
        {
            DistanceFieldShadowParams ShadowParams;
        };
        Buffer(1) DistanceFieldShadowTilesUB
        {
            uint TileData[];
        };

        const uint kThreads = 256u;

        shared uint s_Sum[kThreads];

        void main()
        {
            const uint t     = gl_LocalInvocationIndex;
            const uint tiles = ShadowParams.counts.z;
            const uint run   = (tiles + kThreads - 1u) / kThreads;
            const uint begin = min(t * run, tiles);
            const uint end   = min(begin + run, tiles);

            uint sum = 0u;
            for (uint c = begin; c < end; ++c)
                sum += TileData[c * DistanceFieldShadowHeaderStride + 1u];
            s_Sum[t] = sum;
            barrier();
            // Inclusive scan of the runs' sums (Hillis-Steele).
            for (uint offset = 1u; offset < kThreads; offset <<= 1u)
            {
                const uint before = t >= offset ? s_Sum[t - offset] : 0u;
                barrier();
                s_Sum[t] += before;
                barrier();
            }
            const bool fits = s_Sum[kThreads - 1u] <= ShadowParams.counts.w;

            uint first = tiles * DistanceFieldShadowHeaderStride + s_Sum[t] - sum;
            for (uint c = begin; c < end; ++c)
            {
                const uint header = c * DistanceFieldShadowHeaderStride;
                const uint n      = TileData[header + 1u];
                TileData[header] = fits ? first : 0u;
                if (!fits)
                    TileData[header + 1u] = 0u;
                first += n;
            }
        }
    }
}
