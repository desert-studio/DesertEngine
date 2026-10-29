// DesertAsset {"Kind":"Shader","Guid":"580822f8ec6b4df9b53ae7b26711ea67","Versions":{"SHDR":1},"Dependencies":[]}
// THE NEAR FIELD'S OBJECT CULL, STEP 3 (UE LumenMeshSDFCulling.usf ComputeCulledObjectsStartOffsetCS, written from
// memory): the exclusive prefix sum of the cells' list lengths (MeshSDFCullGrid.shader's count pass) - each cell's
// header gets the first index of its list, the lists following the headers cell after cell. One workgroup: each
// thread sums a run of cells, the runs' sums are scanned in shared memory, each thread then writes its run's offsets.
// When the lists need more entries than the grid buffer has room for (MeshSDFCullParams.counts.y) the frame's lists
// are REFUSED - every cell left empty, the near field not traced this frame - never cut short, and the feedback
// (MeshSDFCullFeedbackUints) tells the CPU how many they needed, so the next frames' buffer holds them
// (DistanceField::GlobalDistanceField::ReadNearFieldFeedback, which logs the refusal).
Shader "MeshSDFCullGridOffsets"
{
    Compute
    {
        LocalSize(256, 1, 1);

        #include <Common/MeshSDFGrid.glslh>

        ReadBuffer(0) MeshSDFCullParamsUB
        {
            MeshSDFCullParams CullParams;
        };
        Buffer(1) MeshSDFGridCellsUB
        {
            uint GridCellsData[];
        };
        Buffer(2) MeshSDFCullFeedbackUB
        {
            uint FeedbackData[];
        };

        const uint kThreads = 256u;

        shared uint s_Sum[kThreads];
        shared uint s_Longest;

        void main()
        {
            const uint t     = gl_LocalInvocationIndex;
            const uint cells = CullParams.grid.cells.w;
            const uint run   = (cells + kThreads - 1u) / kThreads;
            const uint begin = min(t * run, cells);
            const uint end   = min(begin + run, cells);
            if (t == 0u)
                s_Longest = 0u;
            barrier();

            uint sum     = 0u;
            uint longest = 0u;
            for (uint c = begin; c < end; ++c)
            {
                const uint n = GridCellsData[c * MeshSDFGridHeaderStride + 1u];
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
            const uint room   = CullParams.counts.y;
            const bool fits   = needed <= room;

            uint first = cells * MeshSDFGridHeaderStride + s_Sum[t] - sum;
            for (uint c = begin; c < end; ++c)
            {
                const uint header = c * MeshSDFGridHeaderStride;
                const uint n      = GridCellsData[header + 1u];
                GridCellsData[header] = fits ? first : 0u;
                if (!fits)
                    GridCellsData[header + 1u] = 0u;
                first += n;
            }
            if (t == 0u)
            {
                FeedbackData[0] = needed;
                FeedbackData[1] = room;
                FeedbackData[2] = s_Longest;
                FeedbackData[3] = CullParams.counts.z;
            }
        }
    }
}
