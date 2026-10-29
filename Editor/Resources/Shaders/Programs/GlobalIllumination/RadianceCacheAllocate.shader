// DesertAsset {"Kind":"Shader","Guid":"046591383d9b482fb773f89291c0a037","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 3/6 — ALLOCATE AND RANK (UE AllocateUsedProbes + the priority histogram of
// SelectMaxPriorityBucket, LumenRadianceCacheUpdate.usf): one thread per indirection entry a pixel read this frame.
// An empty one takes a slot from the free list (none left: it stays empty, and its pixels read the probes around
// it). Every read probe is ranked into a priority bucket — never traced, then LIGHTS CHANGED (the hash of the
// lights its world light grid cell lists, and of the sun, differs from the one it was traced with; not in UE),
// then by the age of its last trace — and counted in the histogram the selection cuts at the budget.
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

        void main()
        {
            const uvec3 id    = gl_GlobalInvocationID;
            const uint  grid  = radianceCache.shape.y;
            const uint  width = grid * radianceCache.shape.x;
            if ( id.x >= width || id.y >= grid || id.z >= grid || Reset != 0u )
                return;
            const uint entry = id.x + width * ( id.y + grid * id.z );
            if ( radianceCacheLastUsed[entry] != FrameIndex )
                return;
            const uint  clipmap = id.x / grid;
            const ivec3 world   = RadianceCacheWorldCellOfEntry( radianceCache, ivec3( id.x % grid, id.y, id.z ), clipmap );

            uint value = radianceCacheIndirection[entry];
            if ( value == RADIANCE_CACHE_INVALID_PROBE )
            {
                const int top = atomicAdd( radianceCacheFreeList[0], -1 );
                if ( top <= 0 )
                {
                    atomicAdd( radianceCacheFreeList[0], 1 );
                    return;
                }
                const uint probe = uint( radianceCacheFreeList[top] );
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
                bucket = RC_PRIORITY_NEW;
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
