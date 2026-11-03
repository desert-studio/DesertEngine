// DesertAsset {"Kind":"Shader","Guid":"0bd3048ca6f2479ab80974bf1f9f46e6","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 4/6 — SELECT (UE SelectMaxPriorityBucket + the trace list, LumenRadianceCacheUpdate.usf):
// the histogram is cut at the budget (ProbesPerFrame, radianceCache.atlas.z) from the most urgent bucket down; every
// probe read this frame in a bucket above the cut is traced, the cut bucket fills the rest first come.
// A NEW probe (never traced) read this frame is always traced this frame, past the budget when the view brings more
// of them than it (a turn, a walk): an untraced probe answers no reader, so every screen probe ray that reaches it
// traces on to the sky (ScreenProbeTrace's cache-miss leg) each frame until the budget gets to it - its own trace
// is cheaper than those rays. The budget cuts the REFRESH of traced probes only (UE cuts new probes too).
// The trace list holds up to every probe slot; RadianceCacheTraceArgsUB is the Trace and Filter passes' dispatch.
Shader "RadianceCacheSelect"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/RadianceCacheState.glslh>

        // The Trace and Filter passes' indirect dispatch (x = the listed probes, y = z = 1).
        Buffer(6) RadianceCacheTraceArgsUB
        {
            uint radianceCacheTraceArgs[];
        };

        shared uint s_Threshold;
        shared uint s_Quota;

        void main()
        {
            if ( gl_LocalInvocationIndex == 0u )
            {
                const uint budget = max( radianceCache.atlas.z, radianceCacheSchedule[RC_PRIORITY_NEW] );
                uint       taken  = 0u;
                s_Threshold       = 0u;
                s_Quota           = 0xFFFFFFFFu;
                for ( int bucket = int( RC_PRIORITY_BUCKETS ) - 1; bucket >= 0; --bucket )
                {
                    const uint count = radianceCacheSchedule[bucket];
                    if ( taken + count >= budget )
                    {
                        s_Threshold = uint( bucket );
                        s_Quota     = budget - taken;
                        break;
                    }
                    taken += count;
                }
            }
            barrier();
            const uint probe = gl_GlobalInvocationID.x;
            if ( probe >= MaxProbes )
                return;
            if ( radianceCacheProbes[probe].frames.x != FrameIndex )
                return;
            const uint bucket = radianceCacheProbes[probe].priority.x;
            bool       traced = bucket > s_Threshold;
            if ( bucket == s_Threshold )
                traced = atomicAdd( radianceCacheSchedule[RC_SCHEDULE_TAKEN], 1u ) < s_Quota;
            if ( !traced )
                return;
            const uint slot = atomicAdd( radianceCacheSchedule[RC_SCHEDULE_COUNT], 1u );
            if ( slot < MaxProbes )
            {
                radianceCacheSchedule[RC_SCHEDULE_LIST + slot] = probe;
                atomicAdd( radianceCacheTraceArgs[0], 1u );
            }
        }
    }
}
