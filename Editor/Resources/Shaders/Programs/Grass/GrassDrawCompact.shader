// DesertAsset {"Kind":"Shader","Guid":"4e1d8a6bef2e412ea92dc85ac8fe22a4","Versions":{"SHDR":1},"Dependencies":[]}
// RENDER-JOB-CPU-3, THE DRAW GATHER OF THE COMPACTED BINS (Engine/Graphic/GpuInstances.hpp; UE
// FInstanceCullingContext: one indirect argument stream per merged mesh draw command, built on the GPU from the
// views' culled instances). A bin is one (pass, phase, cell template, mesh) of the GPU-instanced queue; each entry
// of this frame's table is a run of one producer list's records (GpuDrawCompactEntry: its Args, a phase's or a
// cascade's copy, by device address) with the address of the draw row its draws read. Modes (Compact.w):
//   0 CLEAR: Counts[b] = 0 for every bin.
//   1 GATHER: thread (k, entry) copies record k of the entry - Records + k x Stride, a VkDrawIndexedIndirectCommand
//     as the producer's cull wrote it (instanceCount 0 included) - to slot FirstSlot + k of Commands, the entry's
//     draw row address to the same slot of DrawRows, and the entry's first thread adds its Count to its bin's
//     drawn count. The slots are the CPU's (MeshRenderer lays the entries out in the per-variety path's draw
//     order), so a bin draws its records in the order the per-variety draws did and the picture stays the same;
//     a record the cull left empty is an empty draw of the GPU's front end, no CPU call.
// "в UE так": the compaction appends with atomics (order free). "лучше так" here: fixed slots, the draw order of
// the per-variety path kept (depth-equal ties and the visibility buffer's tie-break resolve as before), the count
// a sum; an appending compaction that drops the empty records is a mode of this program when the order stops
// mattering.
Shader "GrassDrawCompact"
{
    Compute
    {
        LocalSize(64, 1, 1);

        // GpuInstances.hpp GpuDrawCompactEntry (std430, 32 bytes).
        struct CompactEntry
        {
            uvec2 Records; // the run's first record
            uvec2 DrawRow; // its draws' GpuInstanceDrawRow
            uint  Count;   // records in the run
            uint  Stride;  // bytes between them (a multiple of four)
            uint  Bin;
            uint  FirstSlot; // the entry's first slot of Commands / DrawRows
        };

        ReadBuffer(0) CompactTable
        {
            CompactEntry CompactEntries[];
        };

        // GpuDrawBinRefs: kGpuDrawBinCommandBytes per slot.
        Buffer(1) CompactCommands
        {
            uint CommandWords[];
        };

        // GpuDrawBinRefs: per slot the address of its draw row.
        Buffer(2) CompactDrawRows
        {
            uvec2 DrawRowSlots[];
        };

        // GpuDrawBinRefs: per bin its drawn count (vkCmdDrawIndexedIndirectCount's count).
        Buffer(3) CompactCounts
        {
            uint BinCounts[];
        };

        // Mirrors Graphic::System::GrassDrawCompactPush.
        PushConstant PushConstants
        {
            uvec4 Compact; // x = entries, y = the longest entry's Count, z = bins, w = the mode
        };

        layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer CompactRecordWords
        {
            uint Words[];
        };

        const uint kModeClear  = 0u;
        const uint kModeGather = 1u;

        const uint kCommandWords = 5u; // VkDrawIndexedIndirectCommand

        void main()
        {
            if ( Compact.w == kModeClear )
            {
                if ( gl_GlobalInvocationID.x < Compact.z )
                    BinCounts[gl_GlobalInvocationID.x] = 0u;
                return;
            }
            // Gather: x walks an entry's records, y the entries.
            const uint e = gl_WorkGroupID.y;
            const uint k = gl_GlobalInvocationID.x;
            if ( e >= Compact.x )
                return;
            const CompactEntry entry = CompactEntries[e];
            if ( k >= entry.Count )
                return;
            if ( k == 0u )
                atomicAdd( BinCounts[entry.Bin], entry.Count );
            const CompactRecordWords record = CompactRecordWords( entry.Records );
            const uint               at     = ( k * entry.Stride ) / 4u;
            const uint               slot   = entry.FirstSlot + k;
            for ( uint w = 0u; w < kCommandWords; ++w )
                CommandWords[slot * kCommandWords + w] = record.Words[at + w];
            DrawRowSlots[slot] = entry.DrawRow;
        }
    }
}
