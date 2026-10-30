// DesertAsset {"Kind":"Shader","Guid":"0bd3048ca6f2479ab80974bf1f9f46e6","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 4/6 — SELECT (UE SelectMaxPriorityBucket + the trace list, LumenRadianceCacheUpdate.usf):
// the histogram is cut at the budget (ProbesPerFrame, radianceCache.atlas.z) from the most urgent bucket down; every
// probe read this frame in a bucket above the cut is traced, the cut bucket fills the rest first come.
Shader "RadianceCacheSelect"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/RadianceCacheState.glslh>

        shared uint s_Threshold;
        shared uint s_Quota;

        void main()
        {
            if ( gl_LocalInvocationIndex == 0u )
            {
                const uint budget = radianceCache.atlas.z;
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
            if ( slot < radianceCache.atlas.z )
                radianceCacheSchedule[RC_SCHEDULE_LIST + slot] = probe;
        }
    }
}
