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
// drawn count; the camera's phase-1 drawn count and phase 2's appended count are GrassCull.shader's (the camera always
// has a list of its own since GRASS-CULL-UNIFY-1: the full list is the view-independent field about the field eye),
// read back with the bucket counts FramesInFlight frames later. The cascades' lists - their records, their counters'
// reset and their drawn counts in Feedback - are GrassCull.shader's too (CascadeCull / FinalizeCascades, every frame).
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

        // GrassRenderer.cpp GrassVarietyTableGpu: the camera's side planes, the cascades' boxes and the wind origin
        // (GrassCull's), then the rows.
        ReadBuffer(0) Varieties
        {
            vec4            VarietyCameraPlanes[4];
            vec4            VarietyCascadePlanes[48];
            vec4            VarietyCasterWindOrigin;
            GrassVarietyRow VarietyRows[];
        };

        const uint kRowThreads = 64u;

        // Mirrors Graphic::System::GrassFinalizePush (GrassRenderer.cpp).
        PushConstant PushConstants
        {
            uvec4 Mode; // x = 0 after the generation, 1 before a generation (the reset); y = 0; z = the first row;
                        // w = the rows
        };

        // Mode 0, row @p r: the full list's records, its counts into Feedback, its drawn count into Args word 3 (the
        // camera cull's threads until the next generation).
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
            args.Words[3] = drawn; // this frame's drawn count (the stats read Feedback's copy of it)
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
