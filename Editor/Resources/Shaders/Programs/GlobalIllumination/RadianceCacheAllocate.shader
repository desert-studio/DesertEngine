// DesertAsset {"Kind":"Shader","Guid":"046591383d9b482fb773f89291c0a037","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 3/6 — ALLOCATE AND RANK (UE AllocateUsedProbes + the priority histogram of
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
        ReadBuffer(7) PointLightsUB
        {
            PointLight pointLights[];
        };
        ReadBuffer(8) SpotLightsUB
        {
            SpotLight spotLights[];
        };
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
        // with what it emits and where.
        uint LightHash( vec3 centre )
        {
            uint hash = 2166136261u;
            hash      = HashVec3( hash, giFrame.sunTravel.xyz * giFrame.sunTravel.w );
            hash      = HashVec3( hash, giFrame.sunRadiance.rgb * giFrame.sunTravel.w );
            hash      = HashVec3( hash, giFrame.light1Travel.xyz * giFrame.light1Travel.w );
            hash      = HashVec3( hash, giFrame.light1Radiance.rgb * ( 1.0 + giFrame.light1Radiance.w ) * giFrame.light1Travel.w );
            const LightGridWorldRange range = LightGridWorldLightsAt( lightGrid, centre );
            for ( uint k = 0u; k < range.count; ++k )
            {
                const uint index = LightGridWorldLight( range, k );
                hash             = HashMix( hash, index );
                if ( index < lightGrid.counts.x )
                {
                    const PointLight light = pointLights[index];
                    hash = HashVec3( HashVec3( hash, light.position ), light.color * light.intensity );
                    hash = HashMix( HashMix( hash, floatBitsToUint( light.radius ) ), uint( light.falloff ) );
                }
                else
                {
                    const SpotLight light = spotLights[index - lightGrid.counts.x];
                    hash = HashVec3( HashVec3( hash, light.position ), light.color * light.intensity );
                    hash = HashVec3( hash, light.direction );
                    hash = HashMix( HashMix( hash, floatBitsToUint( light.range ) ), floatBitsToUint( light.cosOuter ) );
                }
            }
            return hash;
        }

        // Pops a free slot for a reader's empty entry, or RADIANCE_CACHE_INVALID_PROBE when the list is empty. A
        // popped slot whose recorded cell's entry still holds it is a doubled copy (Update now frees only the
        // slots an entry owns, so none is pushed any more; the copies already on the list are spent here, and a
        // count past MaxProbes is spent the same way): it is skipped and not restored, so the list heals.
        #define RC_ALLOCATE_POP_ATTEMPTS 4u
        uint PopFreeSlot()
        {
            for ( uint attempt = 0u; attempt < RC_ALLOCATE_POP_ATTEMPTS; ++attempt )
            {
                const int top = atomicAdd( radianceCacheFreeList[0], -1 );
                if ( top <= 0 )
                {
                    atomicAdd( radianceCacheFreeList[0], 1 );
                    return RADIANCE_CACHE_INVALID_PROBE;
                }
                if ( top > int( MaxProbes ) )
                    continue; // the failed-allocation words or past the buffer: not a slot
                const uint candidate = uint( radianceCacheFreeList[top] );
                if ( candidate >= MaxProbes )
                    continue;
                const ivec4 cell = radianceCacheProbes[candidate].cell;
                if ( uint( cell.w ) >= radianceCache.shape.x )
                    return candidate;
                const uint holder =
                     radianceCacheIndirection[RadianceCacheIndirectionIndex( radianceCache, cell.xyz, uint( cell.w ) )];
                if ( ( holder & ~RADIANCE_CACHE_PROBE_UNTRACED ) != candidate )
                    return candidate;
            }
            return RADIANCE_CACHE_INVALID_PROBE;
        }

        void main()
        {
            const uvec3 id    = gl_GlobalInvocationID;
            const uint  grid  = radianceCache.shape.y;
            const uint  width = grid * radianceCache.shape.x;
            if ( id.x >= width || id.y >= grid || id.z >= grid || Reset != 0u )
                return;
            const uint entry = id.x + width * ( id.y + grid * id.z );
            // Which readers stamped it this frame (RadianceCacheMark): a coverage reader, or only the fallback.
            const bool covered = radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_COVERAGE )] == FrameIndex;
            if ( !covered && radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_FALLBACK )] != FrameIndex )
                return;
            const uint  clipmap = id.x / grid;
            const ivec3 world   = RadianceCacheWorldCellOfEntry( radianceCache, ivec3( id.x % grid, id.y, id.z ), clipmap );

            uint value = radianceCacheIndirection[entry];
            if ( value == RADIANCE_CACHE_INVALID_PROBE )
            {
                const uint probe = PopFreeSlot();
                if ( probe == RADIANCE_CACHE_INVALID_PROBE )
                {
                    // The atlas is full: the next frame's Update frees the probes nobody reads now.
                    atomicAdd( radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex )], 1 );
                    atomicAdd( radianceCacheSchedule[RC_SCHEDULE_STATS + RC_STAT_FAILED], 1u );
                    return;
                }
                atomicAdd( radianceCacheSchedule[RC_SCHEDULE_STATS + RC_STAT_ALLOCATED], 1u );
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
            else if ( hash != frames.z )
                bucket = RC_PRIORITY_LIGHTS_CHANGED;
            else
                bucket = min( RC_PRIORITY_LIGHTS_CHANGED - 1u, uint( log2( float( max( FrameIndex - frames.y, 1u ) ) ) * 3.0 ) );
            radianceCacheProbes[probe].frames   = uvec4( FrameIndex, frames.y, frames.z, hash );
            radianceCacheProbes[probe].priority = uvec4( bucket, 0u, 0u, 0u );
            atomicAdd( radianceCacheSchedule[bucket], 1u );
        }
    }
}
