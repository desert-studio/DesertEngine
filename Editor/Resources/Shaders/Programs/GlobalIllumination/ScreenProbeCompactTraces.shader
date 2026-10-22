// DesertAsset {"Kind":"Shader","Guid":"c997ffdf0d4f403c970e5060b83bc6bb","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE TRACE, 2/3 — COMPACTION (UE LumenScreenProbeTracing.usf ScreenProbeCompactTracesCS, in order
// as its wave-ops path, + SetupCompactedTracesIndirectArgsCS): the texels whose ray is still open, listed densely, so the world legs
// (ScreenProbeTrace.shader, one thread per listed ray) run no lane for a ray an earlier leg answered. One workgroup per
// probe, one thread per texel (Common/ScreenProbeTracing.glslh's trace hit buffer); the list keeps a probe's rays
// together and in texel order (one group appends them as one run), so neighbouring threads of the world legs trace neighbouring directions
// of one probe. Two compactions a frame (the push constant):
//   near field   the rays with no hit that have not reached the radiance cache and are not past u_MaxDistance (the
//                near field's end: UE CompactionMaxTraceDistance = MaxMeshSDFTraceDistance);
//   far field    every ray with no hit (UE bCompactForSkyApply: the cache's radiance and the sky apply to all of
//                them).
// The indirect arguments are written here rather than by UE's separate one-thread pass: the list's group count is the
// largest end any probe's run reached (atomicMax), so the dispatch needs no pass of its own.
Shader "ScreenProbeCompactTraces"
{
    Compute
    {

        #include <Common/ScreenProbes.glslh>
        // One thread per traced texel of a probe (SP_RES², the layout permutation).
        LocalSize(SP_TRACED_TEXELS, SP_TRACED_TEXELS, 1);

        ReadBuffer(0) ScreenProbeParamsUB
        {
            ScreenProbeParams screenProbeParams;
        };
        ReadBuffer(1) ScreenProbeIndexUB
        {
            uint screenProbeIndex[];
        };
        ReadBuffer(2) ScreenProbesUB
        {
            ScreenProbe screenProbes[];
        };
        ReadBuffer(3) ScreenProbeTraceHitUB
        {
            uint screenProbeTraceHit[];
        };
        // The world leg's dispatch: x = its workgroups (SP_COMPACT_GROUP_SIZE rays each), y = z = 1 (uploaded).
        Buffer(4) ScreenProbeCompactArgsUB
        {
            uvec4 screenProbeCompactArgs;
        };
        // The rays listed (UE CompactedTraceTexelAllocator; 0 uploaded).
        Buffer(5) ScreenProbeCompactAllocatorUB
        {
            uint screenProbeCompactAllocator;
        };
        Buffer(6) ScreenProbeCompactTexelsUB
        {
            uint screenProbeCompactTexels[]; // probe * SP_RES² + texel
        };

        PushConstant ScreenProbeCompactPush
        {
            float u_MaxDistance; // a ray answered this far is not listed (cm)
            uint  u_FarField;    // 1: list every ray with no hit, the cache's reached ones too
        };

        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #define SP_COMPACT_GROUP_SIZE 64u
        #include <Common/ScreenProbeTracing.glslh>

        #define SP_COMPACT_TEXELS ( SP_RES * SP_RES )
        // The probe's open-ray prefix sum, ping-ponged between two halves (Hillis-Steele: log2(SP_RES²) steps, each
        // thread reading only the step before's half, so no step reads a value the same step writes).
        shared uint s_Scan[2u * SP_COMPACT_TEXELS];
        shared uint s_Start;

        void main()
        {
            const uint probe = gl_WorkGroupID.x;
            const uint texel = gl_LocalInvocationIndex;
            const uint slot  = probe * SP_RES * SP_RES + texel;
            bool       open  = false;
            if ( SPProbeTraced( probe ) )
            {
                const uint encoded = screenProbeTraceHit[slot];
                open = ( encoded & SP_TRACE_HIT_BIT ) == 0u &&
                       ( u_FarField != 0u || ( encoded & SP_TRACE_REACHED_BIT ) == 0u ) &&
                       SPTraceHitDistance( encoded ) < u_MaxDistance;
            }
            const uint own = open ? 1u : 0u;
            s_Scan[texel]  = own;
            barrier();

            // In texel order (UE's wave-ops path is ordered): neighbouring threads of the world legs trace
            // neighbouring directions. A groupshared inclusive prefix sum (UE ScreenProbeCompactTracesCS's
            // GroupShared compaction), log2(SP_RES²) steps instead of a per-thread walk over every texel before it.
            uint src = 0u;
            for ( uint stride = 1u; stride < SP_COMPACT_TEXELS; stride <<= 1u )
            {
                uint sum = s_Scan[src * SP_COMPACT_TEXELS + texel];
                if ( texel >= stride )
                    sum += s_Scan[src * SP_COMPACT_TEXELS + texel - stride];
                s_Scan[( src ^ 1u ) * SP_COMPACT_TEXELS + texel] = sum;
                barrier();
                src ^= 1u;
            }
            const uint inclusive = s_Scan[src * SP_COMPACT_TEXELS + texel];
            const uint offset    = inclusive - own; // the open rays of the texels before this one
            if ( texel == SP_COMPACT_TEXELS - 1u )
            {
                const uint count = inclusive;
                s_Start          = count > 0u ? atomicAdd( screenProbeCompactAllocator, count ) : 0u;
                if ( count > 0u )
                    atomicMax( screenProbeCompactArgs.x,
                               ( s_Start + count + SP_COMPACT_GROUP_SIZE - 1u ) / SP_COMPACT_GROUP_SIZE );
            }
            barrier();
            if ( open )
                screenProbeCompactTexels[s_Start + offset] = slot;
        }
    }
}
