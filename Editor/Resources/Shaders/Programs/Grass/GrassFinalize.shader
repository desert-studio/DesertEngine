// DesertAsset {"Kind":"Shader","Guid":"9af5b17398544149b3546d72bc857b7b","Versions":{"SHDR":1},"Dependencies":[]}
// After GrassGenerate: each bucket's appended count, clamped to its capacity, becomes the instanceCount of the
// records that draw it and its first instance their firstInstance (GpuInstances.hpp: bucket L for the submeshes'
// records of a mesh's level L - one bucket without a LOD chain - and bucket s for record s of every set of a blade
// strip's records, one set per shadow LOD, the row's kGrassRowBlade), the UNCLAMPED counts (the generator's atomic
// counts past the capacity) go to Feedback for the renderer to size the next capacities from (GrassRenderer,
// GPU-count feedback). The counts STAY in Args until the next generation (GRASS-VIS-24: the camera cull of every
// later frame reads them - which slots hold a kept instance - and its dispatch is their sum, Args word 3); the
// reset mode (Mode.x = 1, before a generation) zeroes them.
// Drawn counts (GRASS-VIS-7, GrassRenderer::GetDrawnCounts): after the written mark, Feedback holds the full list's
// drawn count, the camera's phase-1 drawn count (the full list's when the camera has no list of its own, Mode.y = 0;
// GrassCull.shader's camera finalize writes it otherwise) and phase 2's appended count (0 until GrassCull's post
// finalize writes it), read back with the bucket counts FramesInFlight frames later.
// The cascades' lists (GPU-COST-21, the row's Info.w = the cascade views that have one, list k the k-th set bit):
// each list's copy of Args' layout in CascadeArgs gets its records and counts the same way, and Feedback, after
// those three counts, one drawn count per cascade VIEW (0 for a view without a list), so the read-back names the
// cascade whatever lists a later frame has. A list's bucket holds two parts (GRASS-SHADOW-1, GrassGenerate.shader):
// its movers from its first slot - the records of the list's copy of Args draw them - and its rest-pose casters from
// its last slot, counted after the list's rest twin of the records, which draw them; the drawn count is both.
// GRASS-CULL-2 (UE GPUScene: one table, one dispatch for every primitive): ONE dispatch per pass for all the
// generating varieties - thread i finalizes row Mode.z + i (and every 64th after it) of the variety table
// (Common/GrassVarietyRows.glslh), whose buffers it reads and writes by device address. The camera's list is
// GrassCull.shader's.
Shader "GrassFinalize"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/GrassVarietyRows.glslh>

        // GrassRenderer.cpp GrassVarietyTableGpu: the camera's side planes (GrassCull's), then the rows.
        ReadBuffer(0) Varieties
        {
            vec4            VarietyCameraPlanes[4];
            GrassVarietyRow VarietyRows[];
        };

        // The cascade views Feedback counts (GrassRenderer kGrassCascadeLists, SceneResources::kMaxCascadeViews).
        const uint kCascadeViews = 8u;
        // LocalSize: the rows the group finalizes at once.
        const uint kRowThreads = 64u;

        // Mirrors Graphic::System::GrassFinalizePush (GrassRenderer.cpp).
        PushConstant PushConstants
        {
            uvec4 Mode; // x = 0 after the generation, 1 before a generation (the reset); y = 1 when the camera has a
                        // list of its own (its Feedback words are GrassCull's); z = the first row; w = the rows
        };

        // Row @p r's cascade lists: each list's records and counts as Args', its drawn count into Feedback by the
        // view it is for.
        void FinalizeCascades( uint r, uint buckets, uint records, bool blade )
        {
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            GrassWords cascade  = GrassWords( VarietyRows[r].CascadeArgs );
            for ( uint v = 0u; v < kCascadeViews; ++v )
                feedback.Words[buckets + 4u + v] = 0u;
            // One list (GrassRenderer VarietyGpu::CascadeArgWords): its copy of Args' layout, the rest twin's
            // records, one rest count per bucket.
            uint args  = 4u + records * 5u + buckets * 4u;
            uint words = args + records * 5u + buckets;
            uint mask  = VarietyRows[r].Info.w;
            for ( uint k = 0u; mask != 0u; ++k )
            {
                uint view = uint( findLSB( mask ) );
                mask &= mask - 1u;
                uint base  = k * words;
                uint table = base + 4u + records * 5u;
                uint rests = base + args + records * 5u;
                for ( uint s = 0u; s < records; ++s )
                {
                    uint bucket = GrassBucketOfRecord( s, records, buckets, blade );
                    uint at     = table + bucket * 4u;
                    uint room   = cascade.Words[at + 2u];
                    uint rest   = min( cascade.Words[rests + bucket], room );
                    cascade.Words[base + 4u + s * 5u + 1u] = min( cascade.Words[at], room );
                    cascade.Words[base + 4u + s * 5u + 4u] = cascade.Words[at + 1u];
                    cascade.Words[base + args + s * 5u + 1u] = rest;
                    cascade.Words[base + args + s * 5u + 4u] = cascade.Words[at + 1u] + room - rest;
                }
                uint drawn = 0u;
                for ( uint b = 0u; b < buckets; ++b )
                {
                    uint at = table + b * 4u;
                    drawn += min( cascade.Words[at], cascade.Words[at + 2u] ) +
                             min( cascade.Words[rests + b], cascade.Words[at + 2u] );
                    cascade.Words[at]        = 0u;
                    cascade.Words[rests + b] = 0u;
                }
                cascade.Words[base + 3u]            = drawn;
                feedback.Words[buckets + 4u + view] = drawn;
            }
        }

        // Mode 0, row @p r: the full list's records, its counts into Feedback, its drawn count into Args word 3 (the
        // camera cull's threads until the next generation), the cascades' lists.
        void Finalize( uint r )
        {
            GrassWords args     = GrassWords( VarietyRows[r].Args );
            GrassWords feedback = GrassWords( VarietyRows[r].Feedback );
            bool       blade    = ( VarietyRows[r].Info.z & kGrassRowBlade ) != 0u;
            uint       buckets  = args.Words[0];
            uint       records  = args.Words[2];
            uint       table    = 4u + records * 5u;
            for ( uint s = 0u; s < records; ++s )
            {
                uint at = table + GrassBucketOfRecord( s, records, buckets, blade ) * 4u;
                args.Words[4u + s * 5u + 1u] = min( args.Words[at], args.Words[at + 2u] );
                args.Words[4u + s * 5u + 4u] = args.Words[at + 1u];
            }
            uint drawn = 0u;
            for ( uint b = 0u; b < buckets; ++b )
            {
                drawn += min( args.Words[table + b * 4u], args.Words[table + b * 4u + 2u] );
                feedback.Words[b] = args.Words[table + b * 4u];
            }
            feedback.Words[buckets]      = 1u;
            feedback.Words[buckets + 1u] = drawn;
            if ( Mode.y == 0u )
            {
                feedback.Words[buckets + 2u] = drawn;
                feedback.Words[buckets + 3u] = 0u;
            }
            args.Words[3] = drawn; // this frame's drawn count (the stats read Feedback's copy of it)
            FinalizeCascades( r, buckets, records, blade );
        }

        // Mode 1, row @p r: Args' kept counts zeroed before the generation counts anew.
        void Reset( uint r )
        {
            GrassWords args    = GrassWords( VarietyRows[r].Args );
            uint       tableAt = 4u + args.Words[2] * 5u;
            for ( uint b = 0u; b < args.Words[0]; ++b )
                args.Words[tableAt + b * 4u] = 0u;
        }

        void main()
        {
            for ( uint i = gl_LocalInvocationIndex; i < Mode.w; i += kRowThreads )
            {
                if ( Mode.x == 1u )
                    Reset( Mode.z + i );
                else
                    Finalize( Mode.z + i );
            }
        }
    }
}
