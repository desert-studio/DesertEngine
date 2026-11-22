// DesertAsset {"Kind":"Shader","Guid":"0bd3048ca6f2479ab80974bf1f9f46e6","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 3/5 — SELECT, PLACE AND THE TRACE TILES (UE SelectMaxPriorityBucket + the trace list,
// LumenRadianceCacheUpdate.usf, then the probe placement and GenerateProbeTraceTiles, LumenRadianceCache.usf), one
// thread per probe slot:
//   SELECT  the histogram is cut at the budget (ProbesPerFrame, radianceCache.atlas.z) from the most urgent bucket
//           down; every probe read this frame in a bucket above the cut is traced, the cut bucket fills the rest first
//           come. The new probes of the coverage rank above the fallback's (RC_PRIORITY_*), so the fallback is traced
//           from the remainder. FPS-RC-BUDGET: the age buckets (an event-less refresh) take at most RefreshBudget of
//           what the events leave.
//   TILES   each listed probe's trace tiles go to the trace tile list (RadianceCacheTraceTileList, UE
//           GenerateProbeTraceTiles; FPS-RADIANCE-CACHE-BRDF): a near probe traces at HALF resolution, and only the
//           squares its readers' BRDF faces are SUPERSAMPLED to full resolution; a far or fallback-only probe traces
//           at a QUARTER. Trace launches one group per tile, so no group traces a ray nobody needs at that rate.
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

        // THE TRACE LEVELS (UE GenerateProbeTraceTiles, LumenRadianceCache.usf; RADIANCE_CACHE_TRACE_LEVEL_*). A lower
        // level's ray is jittered over its block of texels and written to each of them with its own history, so the
        // converged radiance is the block's box of the full-resolution one; the irradiance and moments (Filter)
        // integrate the same, and a probe changing level keeps its atlas texels - only the next traces' rays change.
        //   QUARTER  a probe whose cell centre is further than RC_TRACE_DOWNSAMPLE_DISTANCE from the camera, cm (UE
        //            r.Lumen.RadianceCache.DownsampleDistanceFromCamera), or that only the fallback read this frame
        //            (a stand-in while its coverage's cells are untraced; no texel of it is read up close).
        //   HALF     every other probe: UE's base trace resolution, half the probe's.
        //   FULL     a square of a HALF probe nearer than RC_TRACE_SUPERSAMPLE_DISTANCE (UE
        //            SupersampleDistanceFromCamera) whose centre direction a reader's normal faces by more than
        //            RC_TRACE_SUPERSAMPLE_BRDF_THRESHOLD (UE SupersampleTileBRDFThreshold: the reader's cosine lobe,
        //            the diffuse BRDF its screen probes' rays and its radiance reads follow, at the square's centre).
        //            The readers' normals are the entry's BRDF bins (RC_ENTRY_BRDF_BINS, Mark), each taken at its
        //            bin's centre; UE projects them to an SH per probe instead and evaluates it at the tile centre.
        #define RC_TRACE_DOWNSAMPLE_DISTANCE        4000.0
        #define RC_TRACE_SUPERSAMPLE_DISTANCE       2000.0
        #define RC_TRACE_SUPERSAMPLE_BRDF_THRESHOLD 0.1
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

            // The listed probe's trace level (RC_TRACE_* above) and, for a HALF probe, its supersampled squares.
            const uint res     = radianceCache.shape.z;
            const uint squares = RadianceCacheTraceTilesPerSide( res, RADIANCE_CACHE_TRACE_LEVEL_FULL );
            const uint halves  = RadianceCacheTraceTilesPerSide( res, RADIANCE_CACHE_TRACE_LEVEL_HALF );
            vec3       centre  = vec3( 0.0 );
            uint       clipmap = 0u;
            uint       level   = RADIANCE_CACHE_TRACE_LEVEL_QUARTER;
            uvec2      supersampled = uvec2( 0u ); // bit y * squares + x: the squares traced at FULL
            uint       tiles   = 0u;
            if ( listed )
            {
                const ivec4 cell     = radianceCacheProbes[probe].cell;
                clipmap              = uint( cell.w );
                centre               = RadianceCacheWorldCellCentre( radianceCache, cell.xyz, clipmap );
                const float toCamera = distance( giFrame.camera.xyz, centre );
                // The read directions since the last trace: this trace answers them, the next collects its own.
                const uint  entry    = RadianceCacheIndirectionIndex( radianceCache, cell.xyz, clipmap );
                const uint  binsWord = RadianceCacheStampIndex( entry, RC_ENTRY_BRDF_BINS );
                uvec2       bins     = uvec2( radianceCacheLastUsed[binsWord], radianceCacheLastUsed[binsWord + 1u] );
                if ( any( notEqual( bins, uvec2( 0u ) ) ) )
                {
                    radianceCacheLastUsed[binsWord]      = 0u;
                    radianceCacheLastUsed[binsWord + 1u] = 0u;
                }
                if ( covered && toCamera <= RC_TRACE_DOWNSAMPLE_DISTANCE )
                {
                    level = RADIANCE_CACHE_TRACE_LEVEL_HALF;
                    if ( toCamera > RC_TRACE_SUPERSAMPLE_DISTANCE )
                        bins = uvec2( 0u );
                    // Each reader normal's bin against each square's centre direction (only the bins set: a
                    // surface's readers fill a few).
                    for ( uint word = 0u; word < 2u; ++word )
                    {
                        uint remaining = bins[word];
                        while ( remaining != 0u )
                        {
                            const uint bit = uint( findLSB( remaining ) );
                            remaining &= remaining - 1u;
                            const vec3 N = RadianceCacheBrdfBinNormal( word * 32u + bit );
                            for ( uint s = 0u; s < squares * squares; ++s )
                            {
                                // The square's centre texel (an edge square is cut at the probe's edge).
                                const uvec2 lo = uvec2( s % squares, s / squares ) * 8u;
                                const vec2  c  = ( vec2( lo ) + 0.5 * vec2( min( uvec2( 8u ), uvec2( res ) - lo ) ) ) /
                                               float( res );
                                if ( dot( N, RadianceCacheOctDecode( c * 2.0 - 1.0 ) ) >
                                     RC_TRACE_SUPERSAMPLE_BRDF_THRESHOLD )
                                    supersampled |= RadianceCacheMaskBit( s );
                            }
                        }
                    }
                    // A FULL tile per supersampled square, a HALF tile per 2x2 squares not all supersampled.
                    tiles = bitCount( supersampled.x ) + bitCount( supersampled.y );
                    for ( uint h = 0u; h < halves * halves; ++h )
                        if ( RadianceCacheHalfTileSkip( supersampled, h, halves, squares ) !=
                             RadianceCacheHalfTileSquares( h, halves, squares ) )
                            ++tiles;
                }
                else
                {
                    const uint side = RadianceCacheTraceTilesPerSide( res, RADIANCE_CACHE_TRACE_LEVEL_QUARTER );
                    tiles           = side * side;
                }
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
                uint word = RadianceCacheTraceTileList( budget ) + s_TileBase + tileLocal;
                if ( level == RADIANCE_CACHE_TRACE_LEVEL_HALF )
                {
                    // The same tiles, in the order counted: the HALF ones, then the FULL squares.
                    for ( uint h = 0u; h < halves * halves; ++h )
                    {
                        const uint skip = RadianceCacheHalfTileSkip( supersampled, h, halves, squares );
                        if ( skip != RadianceCacheHalfTileSquares( h, halves, squares ) )
                            radianceCacheSchedule[word++] =
                                 RadianceCacheTraceTileWord( probe, RADIANCE_CACHE_TRACE_LEVEL_HALF, skip, h );
                    }
                    for ( uint s = 0u; s < squares * squares; ++s )
                        if ( RadianceCacheMaskHas( supersampled, s ) )
                            radianceCacheSchedule[word++] =
                                 RadianceCacheTraceTileWord( probe, RADIANCE_CACHE_TRACE_LEVEL_FULL, 0u, s );
                }
                else
                {
                    for ( uint tile = 0u; tile < tiles; ++tile )
                        radianceCacheSchedule[word++] =
                             RadianceCacheTraceTileWord( probe, RADIANCE_CACHE_TRACE_LEVEL_QUARTER, 0u, tile );
                }
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
