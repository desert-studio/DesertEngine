// DesertAsset {"Kind":"Shader","Guid":"046591383d9b482fb773f89291c0a037","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 2/5 — ALLOCATE AND RANK (UE AllocateUsedProbes + the priority histogram of
// SelectMaxPriorityBucket, LumenRadianceCacheUpdate.usf): one thread per indirection entry a pixel read this frame.
// An empty one takes a slot from the free list (none left: it stays empty, and its pixels read the probes around
// it). Every read probe is ranked into a priority bucket (RC_PRIORITY_*, Common/RadianceCacheState.glslh) — never
// traced and stamped by a coverage reader, then never traced and stamped only by the fallback (each finest clipmap
// first), then LIGHTS CHANGED (the hash of the lights its world light grid cell lists, and of the sun, differs from
// the one it was traced with; not in UE), then by the age of its last trace — and counted in the histogram the
// selection cuts at the budget.
Shader "RadianceCacheAllocate"
{
    Compute
    {
        LocalSize(4, 4, 4);

        #include <Common/RadianceCacheState.glslh>
        #include <Common/GIFrame.glslh>
        #include <Mesh/LightSources.glslh>
        #include <Mesh/LightGrid.glslh>

        ReadBuffer(6) GIFrameUB
        {
            GIFrame giFrame;
        };
        #define POINT_LIGHTS_UB_BINDING 7
        #include <Mesh/PointLightsUB.glslh>
        #define SPOT_LIGHTS_UB_BINDING 8
        #include <Mesh/SpotLightsUB.glslh>
        ReadBuffer(9) LightGridParamsUB
        {
            LightGridParams lightGrid;
        };
        ReadBuffer(10) LightGridWorldCellsUB
        {
            uint lightGridWorldCells[];
        };
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>

        uint HashMix( uint hash, uint value )
        {
            return ( hash ^ value ) * 16777619u; // FNV-1a
        }
        uint HashVec3( uint hash, vec3 v )
        {
            return HashMix( HashMix( HashMix( hash, floatBitsToUint( v.x ) ), floatBitsToUint( v.y ) ), floatBitsToUint( v.z ) );
        }

        // Everything that lights the probe's cell directly: the sun, atmosphere light 1, and each light the world grid lists there
        // with what it emits and where. FPS-RC-BUDGET: the lights are summed as a SET - each light's own hash of what it
        // is, added - so neither the order the grid build lists a cell's lights in nor the light's index in this
        // frame's light buffers (both may change from frame to frame with nothing lit differently) re-ranks every
        // probe near a light as LIGHTS CHANGED, which traced it every frame with the dirty history.
        uint LightHash( vec3 centre )
        {
            uint hash = 2166136261u;
            hash      = HashVec3( hash, giFrame.sunTravel.xyz * giFrame.sunTravel.w );
            hash      = HashVec3( hash, giFrame.sunRadiance.rgb * giFrame.sunTravel.w );
            hash      = HashVec3( hash, giFrame.light1Travel.xyz * giFrame.light1Travel.w );
            hash      = HashVec3( hash, giFrame.light1Radiance.rgb * ( 1.0 + giFrame.light1Radiance.w ) * giFrame.light1Travel.w );
            const LightGridWorldRange range = LightGridWorldLightsAt( lightGrid, centre );
            uint                      lights = 0u;
            for ( uint k = 0u; k < range.count; ++k )
            {
                const uint index = LightGridWorldLight( range, k );
                uint       own;
                if ( index < lightGrid.counts.x )
                {
                    const PointLight light = pointLights[index];
                    own = HashVec3( HashVec3( 2166136261u, light.position ), light.color * light.intensity );
                    own = HashMix( HashMix( own, floatBitsToUint( light.radius ) ), uint( light.falloff ) );
                }
                else
                {
                    const SpotLight light = spotLights[index - lightGrid.counts.x];
                    own = HashVec3( HashVec3( 0x811C9DC6u, light.position ), light.color * light.intensity ); // a spot's seed
                    own = HashVec3( own, light.direction );
                    own = HashMix( HashMix( own, floatBitsToUint( light.range ) ), floatBitsToUint( light.cosOuter ) );
                }
                lights += own; // commutative: the set, not the list
            }
            return HashMix( HashMix( hash, lights ), range.count );
        }

        // Whether a slot popped off the free list at @p top is a slot to take. A popped slot whose recorded cell's
        // entry still holds it is a doubled copy (Mark's free part frees only the slots an entry owns, so none is pushed
        // any more; the copies already on the list are spent here, and a count past MaxProbes is spent the same
        // way): it is skipped and not restored, so the list heals.
        uint FreeSlotAt( const int top )
        {
            if ( top > int( MaxProbes ) )
                return RADIANCE_CACHE_INVALID_PROBE; // the failed-allocation words or past the buffer: not a slot
            const uint candidate = uint( radianceCacheFreeList[top] );
            if ( candidate >= MaxProbes )
                return RADIANCE_CACHE_INVALID_PROBE;
            const ivec4 cell = radianceCacheProbes[candidate].cell;
            if ( uint( cell.w ) >= radianceCache.shape.x )
                return candidate;
            const uint ownerEntry = RadianceCacheIndirectionIndex( radianceCache, cell.xyz, uint( cell.w ) );
            const uint holder     = radianceCacheIndirection[ownerEntry];
            if ( ( holder & ~RADIANCE_CACHE_PROBE_UNTRACED ) != candidate )
                return candidate;
            return RADIANCE_CACHE_INVALID_PROBE;
        }

        // The group's share of the priority histogram, added to the schedule's once per group and bucket
        // (GPU-SCREENPROBE-6: one global atomic per read probe serialised on the histogram's few busy words).
        shared uint s_Histogram[RC_PRIORITY_BUCKETS];
        // FPS-SP-LIST-ATOMICS (UE BuildListsCS group reservation): the free-list pops and the allocation counts the
        // same way - one pop of the group's empty entries a round, one add per count a group. Was a pop (or more)
        // per new probe on the list's one top word plus two count atomics per new probe, every one serialised at
        // one L2 slice: a camera turn allocates thousands of probes a frame.
        #define RC_ALLOCATE_POP_ATTEMPTS 4u
        shared uint s_PopNeed;   // the group's entries still wanting a slot this round
        shared int  s_PopTop;    // the list's count the round's reservation started from (its first pop's slot)
        shared uint s_PopTaken;  // of s_PopNeed, how many the list held
        shared uint s_Allocated; // the group's allocations
        shared uint s_Failed;    // the group's entries left empty (the atlas full)

        // The entry this thread allocates and ranks; stamped: read this frame, in the grid, no reset.
        struct RankedEntry
        {
            bool  stamped;
            bool  covered;
            uint  entry;
            uint  clipmap;
            ivec3 world;
            uint  value;
        };

        RankedEntry ReadEntry()
        {
            RankedEntry e;
            e.stamped       = false;
            e.covered       = false;
            e.entry         = 0u;
            e.clipmap       = 0u;
            e.world         = ivec3( 0 );
            e.value         = RADIANCE_CACHE_INVALID_PROBE;
            const uvec3 id    = gl_GlobalInvocationID;
            const uint  grid  = radianceCache.shape.y;
            const uint  width = grid * radianceCache.shape.x;
            if ( id.x >= width || id.y >= grid || id.z >= grid )
                return e;
            e.entry = id.x + width * ( id.y + grid * id.z );
            // Which readers stamped it this frame (RadianceCacheMark): a coverage reader, or only the fallback.
            e.covered = radianceCacheLastUsed[RadianceCacheStampIndex( e.entry, RC_STAMP_COVERAGE )] == FrameIndex;
            if ( !e.covered && radianceCacheLastUsed[RadianceCacheStampIndex( e.entry, RC_STAMP_FALLBACK )] != FrameIndex )
                return e;
            e.stamped = true;
            e.clipmap = id.x / grid;
            e.world   = RadianceCacheWorldCellOfEntry( radianceCache, ivec3( id.x % grid, id.y, id.z ), e.clipmap );
            e.value   = radianceCacheIndirection[e.entry];
            return e;
        }

        // Pops a free slot for each of the group's empty read entries, RADIANCE_CACHE_INVALID_PROBE when the list
        // ran out (or every attempt popped a doubled copy). Called by every thread of the group (its barriers):
        // each round the threads still wanting a slot are counted in shared memory, one thread takes that many
        // off the list at once (restoring the part the list did not hold) and each takes its own pop of them.
        uint PopFreeSlots( const bool wants )
        {
            const uint t      = gl_LocalInvocationIndex;
            bool       needs  = wants;
            uint       result = RADIANCE_CACHE_INVALID_PROBE;
            for ( uint attempt = 0u; attempt < RC_ALLOCATE_POP_ATTEMPTS; ++attempt )
            {
                barrier(); // the last round's reads of s_PopNeed / s_PopTop / s_PopTaken are done
                if ( t == 0u )
                    s_PopNeed = 0u;
                barrier();
                uint local = 0u;
                if ( needs )
                    local = atomicAdd( s_PopNeed, 1u );
                barrier();
                const uint need = s_PopNeed; // uniform: nobody writes it before the next round's first barrier
                if ( need == 0u )
                    break;
                if ( t == 0u )
                {
                    const int  top   = atomicAdd( radianceCacheFreeList[0], -int( need ) );
                    const uint taken = uint( clamp( top, 0, int( need ) ) );
                    if ( taken < need )
                        atomicAdd( radianceCacheFreeList[0], int( need - taken ) ); // what the list did not hold
                    s_PopTop   = top;
                    s_PopTaken = taken;
                }
                barrier();
                if ( needs )
                {
                    if ( local >= s_PopTaken )
                        needs = false; // the list is empty: the entry stays empty
                    else
                    {
                        result = FreeSlotAt( s_PopTop - int( local ) ); // pop number local: top - local >= 1
                        needs  = result == RADIANCE_CACHE_INVALID_PROBE;
                    }
                }
            }
            return result;
        }

        // Allocates (with the slot PopFreeSlots took for it) and ranks this thread's entry: its probe's priority
        // bucket, RC_PRIORITY_BUCKETS when it ranks none (out of the grid, the reset, unread this frame, or no slot
        // left).
        uint AllocateAndRank( const RankedEntry e, const uint popped )
        {
            if ( !e.stamped )
                return RC_PRIORITY_BUCKETS;
            const bool  covered = e.covered;
            const uint  entry   = e.entry;
            const uint  clipmap = e.clipmap;
            const ivec3 world   = e.world;

            uint value = e.value;
            if ( value == RADIANCE_CACHE_INVALID_PROBE )
            {
                const uint probe = popped;
                if ( probe == RADIANCE_CACHE_INVALID_PROBE )
                {
                    // The atlas is full: the next frame's Mark frees the probes nobody reads now.
                    atomicAdd( s_Failed, 1u );
                    return RC_PRIORITY_BUCKETS;
                }
                atomicAdd( s_Allocated, 1u );
                radianceCacheProbes[probe].cell     = ivec4( world, int( clipmap ) );
                radianceCacheProbes[probe].frames   = uvec4( 0u );
                radianceCacheProbes[probe].offset   = vec4( 0.0 );
                radianceCacheProbes[probe].priority = uvec4( 0u );
                value                               = probe | RADIANCE_CACHE_PROBE_UNTRACED;
                radianceCacheIndirection[entry]     = value;
            }
            const uint  probe  = value & ~RADIANCE_CACHE_PROBE_UNTRACED;
            const uvec4 frames = radianceCacheProbes[probe].frames;
            const uint  hash   = LightHash( RadianceCacheWorldCellCentre( radianceCache, world, clipmap ) );
            uint        bucket;
            if ( frames.y == 0u )
                bucket = RadianceCacheNewProbeBucket( covered, clipmap ); // the coverage's first, then the fallback's
            else if ( hash != frames.z || frames.y < LightingFrame )
                bucket = RC_PRIORITY_LIGHTS_CHANGED; // FPS-RC-BUDGET: the old sun is an event, not a refresh
            else
            {
                // FPS-RC-COST: no event - the probe is re-traced only once it is RefreshMinFrames old (a probe traced
                // before the last full update holds the old sun: ranked LIGHTS CHANGED above). Otherwise it is read
                // as it is and takes no share of the budget (RC_PRIORITY_NOT_DUE, skipped by Select).
                const uint age = FrameIndex - frames.y;
                if ( age < RefreshMinFrames )
                    bucket = RC_PRIORITY_NOT_DUE;
                else
                    bucket = min( RC_PRIORITY_LIGHTS_CHANGED - 1u, uint( log2( float( max( age, 1u ) ) ) * 3.0 ) );
            }
            radianceCacheProbes[probe].frames   = uvec4( FrameIndex, frames.y, frames.z, hash );
            radianceCacheProbes[probe].priority = uvec4( bucket, covered ? 1u : 0u, 0u, 0u );
            return bucket;
        }

        void main()
        {
            // RESET: the state is (re)initialised instead - every entry empty and unstamped, every slot free - by the
            // update's one pass that runs per indirection entry (Mark's free part frees nothing that frame).
            if ( Reset != 0u ) // a push constant: the whole dispatch returns here, before any barrier
            {
                const uvec3 id    = gl_GlobalInvocationID;
                const uint  grid  = radianceCache.shape.y;
                const uint  width = grid * radianceCache.shape.x;
                if ( id.x >= width || id.y >= grid || id.z >= grid )
                    return;
                const uint entry                = id.x + width * ( id.y + grid * id.z );
                radianceCacheIndirection[entry] = RADIANCE_CACHE_INVALID_PROBE;
                radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_COVERAGE )] = 0u;
                radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_FALLBACK )] = 0u;
                radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_ENTRY_BRDF_BINS )]      = 0u;
                radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_ENTRY_BRDF_BINS ) + 1u] = 0u;
                if ( entry < MaxProbes )
                    radianceCacheFreeList[1u + entry] = int( entry );
                if ( entry == 0u )
                {
                    radianceCacheFreeList[0] = int( MaxProbes );
                    radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex )]      = 0;
                    radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex + 1u )] = 0;
                }
                return;
            }
            const uint t = gl_LocalInvocationIndex;
            if ( t < RC_PRIORITY_BUCKETS )
                s_Histogram[t] = 0u;
            if ( t == 0u )
            {
                s_Allocated = 0u;
                s_Failed    = 0u;
            }
            const RankedEntry e      = ReadEntry();
            const uint        popped = PopFreeSlots( e.stamped && e.value == RADIANCE_CACHE_INVALID_PROBE ); // barriers first
            const uint        bucket = AllocateAndRank( e, popped );
            if ( bucket < RC_PRIORITY_BUCKETS )
                atomicAdd( s_Histogram[bucket], 1u );
            barrier();
            if ( t < RC_PRIORITY_BUCKETS && s_Histogram[t] != 0u )
                atomicAdd( radianceCacheSchedule[t], s_Histogram[t] );
            if ( t == 0u && s_Failed != 0u )
            {
                atomicAdd( radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex )], int( s_Failed ) );
                atomicAdd( radianceCacheSchedule[RC_SCHEDULE_STATS + RC_STAT_FAILED], s_Failed );
            }
            if ( t == 0u && s_Allocated != 0u )
                atomicAdd( radianceCacheSchedule[RC_SCHEDULE_STATS + RC_STAT_ALLOCATED], s_Allocated );
        }
    }
}
