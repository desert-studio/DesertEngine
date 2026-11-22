// DesertAsset {"Kind":"Shader","Guid":"0bd3048ca6f2479ab80974bf1f9f46e6","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 3/5 — SELECT, PLACE AND THE TRACE TILES (UE SelectMaxPriorityBucket + the trace list,
// LumenRadianceCacheUpdate.usf, then the probe placement and GenerateProbeTraceTiles, LumenRadianceCache.usf), one
// thread per probe slot:
//   SELECT  the histogram is cut at the budget (ProbesPerFrame, radianceCache.atlas.z) from the most urgent bucket
//           down; every probe read this frame in a bucket above the cut is traced, the cut bucket fills the rest first
//           come. The new probes of the coverage rank above the fallback's (RC_PRIORITY_*), so the fallback is traced
//           from the remainder. FPS-RC-BUDGET: the age buckets (an event-less refresh) take at most RefreshBudget of
//           what the events leave.
//   TILES   each listed probe's 8x8 squares of traced texels go to the trace tile list (RadianceCacheTraceTileList):
//           all of them at full resolution, a quarter of them for a DOWNSAMPLED probe - one ray per 2x2 texels -,
//           which Trace then launches no group for (FPS-RADIANCE-CACHE: every probe launched a full probe's squares
//           and a downsampled one ended three of four of its groups at once).
//   PLACE   each listed probe is moved out of geometry by the tracer (GIMoveOutOfGeometry; not in UE: a world-grid
//           probe inside a wall would see only the wall's back faces; DDGI relocates its probes the same way) - up to
//           a quarter cell of free space around the probe, at most 0.45 cell from the cell centre per axis (the probe
//           stays the cell's) - and its offset is written to the probe, where Trace reads its ray origin and every
//           reader its interpolation position (RadianceCache.glslh). Once a probe, by the thread that listed it
//           (FPS-RADIANCE-CACHE: was a pass of its own between this one and Trace on the async chain).
// The last group done writes Trace's and Filter's indirect arguments from the final counts. The program is compiled
// per tracer (GlobalIllumination::TracerVariant), as Trace's; it lights no hit (GI_HIT_LIGHTING_NONE), so it declares
// no point-clamped state (DeclareTracerReads Skip).
Shader "RadianceCacheSelect"
{
    Compute
    {
        LocalSize(64, 1, 1);

        #include <Common/RadianceCacheState.glslh>
        #include <Common/GIFrame.glslh>

        // 32..42: the tracer's own resources (Common/GITracer.glslh), the bindings Trace gives them.
        #define GI_HIT_LIGHTING_NONE 1
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>

        // RC_COUNT_WORDS words, RC_COUNT_* (Common/RadianceCacheState.glslh): this frame's update in counts, read back
        // by WorldRadianceCache (the profile line "RadianceCache update").
        Buffer(6) RadianceCacheUpdateCountsUB
        {
            uint radianceCacheUpdateCounts[];
        };
        // FPS-RC-COST: Trace's and Filter's indirect dispatches, sized by this frame's lists: [0..2] Trace (the trace
        // tiles in rows of RC_TRACE_TILE_ROW groups), [3..5] Filter (the listed probes, 1, 1). A frame that lists
        // fewer probes than the budget launches no group past them.
        Buffer(7) RadianceCacheDispatchArgsUB
        {
            uint radianceCacheDispatchArgs[];
        };
        ReadBuffer(8) GIFrameUB
        {
            GIFrame giFrame;
        };

        // DOWNSAMPLED PROBES (UE r.Lumen.RadianceCache.DownsampleDistanceFromCamera, GenerateProbeTraceTiles in
        // LumenRadianceCache.usf): a probe whose cell centre is further than this from the camera, cm, traces at half
        // its resolution - one ray per 2x2 texels, jittered over all four, written to each of them with its own
        // history - so it costs a quarter of a near probe's rays. Its converged radiance is the 2x2 box of the
        // full-resolution one; the irradiance and moments (Filter) integrate the same. A probe crossing the distance
        // keeps its atlas texels: only the next traces' rays change.
        #define RC_TRACE_DOWNSAMPLE_DISTANCE 4000.0
        // A trace tile row of Trace's indirect dispatch (x); y = the rows (a full update's tiles pass 65535).
        #define RC_TRACE_TILE_ROW 1024u

        shared uint s_Threshold;
        shared uint s_Quota;
        shared uint s_CutCount;  // the group's probes in the cut bucket
        shared uint s_CutBase;   // how many of the cut bucket's quota the groups before took
        shared uint s_ListCount; // the group's traced probes
        shared uint s_ListBase;  // their first slot in the trace list
        shared uint s_TileCount; // the group's trace tiles
        shared uint s_TileBase;  // their first word in the trace tile list

        void main()
        {
            const uint t = gl_LocalInvocationIndex;
            if ( t == 0u )
            {
                s_CutCount  = 0u;
                s_CutBase   = 0u;
                s_ListCount = 0u;
                s_ListBase  = 0u;
                s_TileCount = 0u;
                s_TileBase  = 0u;
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
                // Mark and Allocate are done: the frame's counts, once (the first group).
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
            const uint probe   = gl_GlobalInvocationID.x;
            bool       above   = false; // in a bucket above the cut: traced
            bool       atCut   = false; // in the cut bucket: traced while its quota lasts
            bool       covered = false; // a coverage reader stamped it this frame (RadianceCacheAllocate priority.y)
            if ( probe < MaxProbes && radianceCacheProbes[probe].frames.x == FrameIndex )
            {
                const uvec4 priority = radianceCacheProbes[probe].priority;
                // Read this frame but due no trace (RC_PRIORITY_NOT_DUE, RadianceCacheAllocate): not in the histogram.
                if ( priority.x < RC_PRIORITY_BUCKETS )
                {
                    above   = priority.x > s_Threshold;
                    atCut   = priority.x == s_Threshold;
                    covered = priority.y != 0u;
                }
            }
            // FPS-SP-LIST-ATOMICS (UE BuildListsCS group reservation): the cut bucket's quota, the trace list and the
            // trace tiles are taken once per group (the group's count in shared memory, one global atomicAdd, each
            // thread its own index past the group's base) - was one global atomic per cut-bucket probe and one per
            // traced probe, every one on the same two words. Which of the cut bucket's probes win the quota and the
            // list's order are first come; RadianceCacheTrace reads each tile on its own.
            uint cutLocal = 0u;
            if ( atCut )
                cutLocal = atomicAdd( s_CutCount, 1u );
            barrier();
            if ( t == 0u && s_CutCount != 0u )
                s_CutBase = atomicAdd( radianceCacheSchedule[RC_SCHEDULE_TAKEN], s_CutCount );
            barrier();
            const bool traced    = above || ( atCut && s_CutBase + cutLocal < s_Quota );
            uint       listLocal = 0u;
            if ( traced )
                listLocal = atomicAdd( s_ListCount, 1u );
            barrier();
            if ( t == 0u && s_ListCount != 0u )
                s_ListBase = atomicAdd( radianceCacheSchedule[RC_SCHEDULE_COUNT], s_ListCount );
            barrier();
            const uint budget = radianceCache.atlas.z;
            const uint slot   = s_ListBase + listLocal;
            const bool listed = traced && slot < budget;

            // The listed probe's traced resolution: half (downsampled) past the distance, or when only the fallback
            // read it this frame - a cell the reads interpolate only while their own clipmap's cells are untraced
            // (RadianceCacheRadianceAt), so its far-field radiance stands in for a few frames and no texel of it is
            // read up close; full otherwise.
            vec3 centre      = vec3( 0.0 );
            uint clipmap     = 0u;
            bool downsampled = false;
            uint tiles       = 0u;
            if ( listed )
            {
                const ivec4 cell = radianceCacheProbes[probe].cell;
                clipmap          = uint( cell.w );
                centre           = RadianceCacheWorldCellCentre( radianceCache, cell.xyz, clipmap );
                downsampled      = !covered || distance( giFrame.camera.xyz, centre ) > RC_TRACE_DOWNSAMPLE_DISTANCE;
                const uint side  = ( RadianceCacheTraceResolution( radianceCache.shape.z, downsampled ) + 7u ) / 8u;
                tiles            = side * side;
            }
            uint tileLocal = 0u;
            if ( tiles != 0u )
                tileLocal = atomicAdd( s_TileCount, tiles );
            barrier();
            if ( t == 0u && s_TileCount != 0u )
                s_TileBase = atomicAdd( radianceCacheSchedule[RC_SCHEDULE_TILES], s_TileCount );
            barrier();
            if ( listed )
            {
                radianceCacheSchedule[RC_SCHEDULE_LIST + slot] = probe;
                const uint first = RadianceCacheTraceTileList( budget ) + s_TileBase + tileLocal;
                for ( uint tile = 0u; tile < tiles; ++tile )
                    radianceCacheSchedule[first + tile] = RadianceCacheTraceTileWord( probe, downsampled, tile );
            }

            // The last group done (its count, and every group's before it, final): Trace's and Filter's arguments.
            if ( t == 0u )
            {
                memoryBarrierBuffer(); // this group's counts land before its "done"
                const uint done = atomicAdd( radianceCacheSchedule[RC_SCHEDULE_SELECT_DONE], 1u );
                if ( done + 1u == gl_NumWorkGroups.x )
                {
                    memoryBarrierBuffer();
                    const uint count     = min( atomicAdd( radianceCacheSchedule[RC_SCHEDULE_COUNT], 0u ), budget );
                    const uint tileCount = atomicAdd( radianceCacheSchedule[RC_SCHEDULE_TILES], 0u );
                    radianceCacheDispatchArgs[0] = min( tileCount, RC_TRACE_TILE_ROW );
                    radianceCacheDispatchArgs[1] = ( tileCount + RC_TRACE_TILE_ROW - 1u ) / RC_TRACE_TILE_ROW;
                    radianceCacheDispatchArgs[2] = 1u;
                    radianceCacheDispatchArgs[3] = count;
                    radianceCacheDispatchArgs[4] = 1u;
                    radianceCacheDispatchArgs[5] = 1u;
                }
            }

            // PLACE: out of geometry, once a listed probe (the field walk last: no barrier waits on it).
            if ( listed )
            {
                const float cellSize = RadianceCacheCellSize( radianceCache, clipmap );
                const vec3  p        = GIMoveOutOfGeometry( centre, 0.25 * cellSize, 0.45 * cellSize );
                radianceCacheProbes[probe].offset = vec4( p - centre, 0.0 );
            }
        }
    }
}
