// DesertAsset {"Kind":"Shader","Guid":"0bd3048ca6f2479ab80974bf1f9f46e6","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 4/7 — SELECT (UE SelectMaxPriorityBucket + the trace list, LumenRadianceCacheUpdate.usf):
// the histogram is cut at the budget (ProbesPerFrame, radianceCache.atlas.z) from the most urgent bucket down; every
// probe read this frame in a bucket above the cut is traced, the cut bucket fills the rest first come. The new
// probes of the coverage rank above the fallback's (RC_PRIORITY_*), so the fallback is traced from the remainder.
// FPS-RC-BUDGET: the age buckets (an event-less refresh) take at most RefreshBudget of what the events leave.
Shader "RadianceCacheSelect"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/RadianceCacheState.glslh>

        // RC_COUNT_WORDS words, RC_COUNT_* (Common/RadianceCacheState.glslh): this frame's update in counts, read back
        // by WorldRadianceCache (the profile line "RadianceCache update").
        Buffer(6) RadianceCacheUpdateCountsUB
        {
            uint radianceCacheUpdateCounts[];
        };

        shared uint s_Threshold;
        shared uint s_Quota;

        void main()
        {
            if ( gl_LocalInvocationIndex == 0u )
            {
                const uint budget = radianceCache.atlas.z;
                uint       taken  = 0u;
                // FPS-RC-BUDGET: the cut's limit. The event buckets (new, lights changed) take the whole budget;
                // the age buckets below them only what the events leave, and at most RefreshBudget of it - a
                // converged probe's re-trace is a refresh nobody waits for, so a view whose read probes come due
                // faster than that is refreshed over more frames instead of filling the budget every frame.
                uint limit        = budget;
                s_Threshold       = 0u;
                s_Quota           = 0xFFFFFFFFu;
                for ( int bucket = int( RC_PRIORITY_BUCKETS ) - 1; bucket >= 0; --bucket )
                {
                    if ( uint( bucket ) == RC_PRIORITY_LIGHTS_CHANGED - 1u )
                        limit = min( budget, taken + RefreshBudget );
                    const uint count = radianceCacheSchedule[bucket];
                    if ( taken + count >= limit )
                    {
                        s_Threshold = uint( bucket );
                        s_Quota     = limit - taken;
                        taken       = limit;
                        break;
                    }
                    taken += count;
                }
                // Mark, Update and Allocate are done: the frame's counts, once (the first group).
                if ( gl_WorkGroupID.x == 0u )
                {
                    uint stamped     = 0u;
                    uint neverTraced = 0u;
                    uint aged        = 0u;
                    for ( uint bucket = 0u; bucket < RC_PRIORITY_BUCKETS; ++bucket )
                    {
                        stamped += radianceCacheSchedule[bucket];
                        if ( bucket >= RC_PRIORITY_NEW_COVERAGE )
                            neverTraced += radianceCacheSchedule[bucket];
                        if ( bucket < RC_PRIORITY_LIGHTS_CHANGED )
                            aged += radianceCacheSchedule[bucket];
                    }
                    radianceCacheUpdateCounts[RC_COUNT_STAMPED]      = stamped;
                    radianceCacheUpdateCounts[RC_COUNT_NEVER_TRACED] = neverTraced;
                    // The cut's total: every bucket above it, and the cut bucket's quota (or all, under the limit).
                    radianceCacheUpdateCounts[RC_COUNT_TRACED]         = taken;
                    radianceCacheUpdateCounts[RC_COUNT_LIGHTS_CHANGED] = radianceCacheSchedule[RC_PRIORITY_LIGHTS_CHANGED];
                    radianceCacheUpdateCounts[RC_COUNT_AGED]           = aged;
                    radianceCacheUpdateCounts[RC_COUNT_BUDGET]       = budget;
                    for ( uint i = 0u; i < RC_STAT_WORDS; ++i )
                        radianceCacheUpdateCounts[RC_COUNT_EVENTS + i] = radianceCacheSchedule[RC_SCHEDULE_STATS + i];
                    radianceCacheUpdateCounts[RC_COUNT_LIVE] = MaxProbes - uint( max( radianceCacheFreeList[0], 0 ) );
                    radianceCacheUpdateCounts[RC_COUNT_MAX_PROBES] = MaxProbes;
                    for ( uint c = 0u; c < RADIANCE_CACHE_MAX_CLIPMAPS; ++c )
                    {
                        radianceCacheUpdateCounts[RC_COUNT_NEVER_TRACED_BY_CLIPMAP + c] =
                             radianceCacheSchedule[RadianceCacheNewProbeBucket( true, c )];
                        radianceCacheUpdateCounts[RC_COUNT_FALLBACK_BY_CLIPMAP + c] =
                             radianceCacheSchedule[RadianceCacheNewProbeBucket( false, c )];
                    }
                }
            }
            barrier();
            const uint probe = gl_GlobalInvocationID.x;
            if ( probe >= MaxProbes )
                return;
            if ( radianceCacheProbes[probe].frames.x != FrameIndex )
                return;
            const uint bucket = radianceCacheProbes[probe].priority.x;
            // Read this frame but due no trace (RC_PRIORITY_NOT_DUE, RadianceCacheAllocate): not in the histogram.
            if ( bucket >= RC_PRIORITY_BUCKETS )
                return;
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
