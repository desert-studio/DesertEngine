// DesertAsset {"Kind":"Shader","Guid":"054097880a2a4c90a7dd0e118151fb56","Versions":{"SHDR":1},"Dependencies":[]}
// "SHADOWS: DISTANCE FIELD SUN", THE TILE LISTS' OFFSETS: the exclusive prefix sum of the tiles' list lengths
// (DistanceFieldShadowCull.shader's count pass), as the near field's (MeshSDFCullGridOffsets.shader) - each tile's
// header gets the first index of its list, the lists following the headers tile after tile. One workgroup: each
// thread sums a run of tiles, the runs' sums are scanned in shared memory, each thread writes its run's offsets.
// When the lists need more entries than the buffer has room for (counts.w) the frame's lists are REFUSED - every
// tile left empty, no object traced this frame - never cut short - and the feedback tells the host what they
// needed, so the next frame's buffer has the room (DistanceFieldShadowRenderer::ReadFeedback, as the near field's
// GlobalDistanceFieldView::ReadNearFieldFeedback).
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
        // DistanceFieldShadowFeedbackUints: the entries the lists needed, the room they had, the longest list, the
        // stamp of this cull (the host ignores another cull's bytes).
        Buffer(2) DistanceFieldShadowFeedbackUB
        {
            uint Feedback[];
        };
        PushConstant DistanceFieldShadowOffsetsPush
        {
            uint Stamp;
        };

        const uint kThreads = 256u;

        shared uint s_Sum[kThreads];
        shared uint s_Longest;

        void main()
        {
            const uint t     = gl_LocalInvocationIndex;
            const uint tiles = ShadowParams.counts.z;
            const uint run   = (tiles + kThreads - 1u) / kThreads;
            const uint begin = min(t * run, tiles);
            const uint end   = min(begin + run, tiles);

            if (t == 0u)
                s_Longest = 0u;
            barrier();
            uint sum     = 0u;
            uint longest = 0u;
            for (uint c = begin; c < end; ++c)
            {
                const uint n = TileData[c * DistanceFieldShadowHeaderStride + 1u];
                sum += n;
                longest = max(longest, n);
            }
            s_Sum[t] = sum;
            atomicMax(s_Longest, longest);
            barrier();
            // Inclusive scan of the runs' sums (Hillis-Steele).
            for (uint offset = 1u; offset < kThreads; offset <<= 1u)
            {
                const uint before = t >= offset ? s_Sum[t - offset] : 0u;
                barrier();
                s_Sum[t] += before;
                barrier();
            }
            const uint needed = s_Sum[kThreads - 1u];
            const bool fits   = needed <= ShadowParams.counts.w;
            if (t == 0u)
            {
                Feedback[0] = needed;
                Feedback[1] = ShadowParams.counts.w;
                Feedback[2] = s_Longest;
                Feedback[3] = Stamp;
            }

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
