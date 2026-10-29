// DesertAsset {"Kind":"Shader","Guid":"2359ee2c998c43d68dfaeed7c97a113f","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 2/6 — FREE (UE UpdateCacheForUsedProbes, LumenRadianceCacheUpdate.usf): one thread per
// indirection entry. An entry whose probe records another world cell (the clipmap scrolled past it) or that no
// pixel read for RadianceCache::kProbeUnusedFramesBeforeFree frames returns its slot to the free list. With Reset
// it instead initialises the state: every entry empty, every slot free. Also clears this frame's schedule header.
Shader "RadianceCacheUpdate"
{
    Compute
    {
        LocalSize(4, 4, 4);

        #include <Common/RadianceCacheState.glslh>

        void main()
        {
            const uvec3 id    = gl_GlobalInvocationID;
            const uint  grid  = radianceCache.shape.y;
            const uint  width = grid * radianceCache.shape.x;
            if ( id.x >= width || id.y >= grid || id.z >= grid )
                return;
            const uint entry = id.x + width * ( id.y + grid * id.z );
            if ( entry < RC_SCHEDULE_LIST )
                radianceCacheSchedule[entry] = 0u;
            if ( Reset != 0u )
            {
                radianceCacheIndirection[entry] = RADIANCE_CACHE_INVALID_PROBE;
                radianceCacheLastUsed[entry]    = 0u;
                if ( entry < MaxProbes )
                    radianceCacheFreeList[1u + entry] = int( entry );
                if ( entry == 0u )
                    radianceCacheFreeList[0] = int( MaxProbes );
                return;
            }
            const uint value = radianceCacheIndirection[entry];
            if ( value == RADIANCE_CACHE_INVALID_PROBE )
                return;
            const uint  probe   = value & ~RADIANCE_CACHE_PROBE_UNTRACED;
            const uint  clipmap = id.x / grid;
            const ivec3 world   = RadianceCacheWorldCellOfEntry( radianceCache, ivec3( id.x % grid, id.y, id.z ), clipmap );
            const ivec4 owner   = radianceCacheProbes[probe].cell;
            const bool  stale   = any( notEqual( owner.xyz, world ) ) || uint( owner.w ) != clipmap;
            const bool  unused  = FrameIndex - radianceCacheLastUsed[entry] > UnusedFrames;
            if ( stale || unused )
            {
                const int slot = atomicAdd( radianceCacheFreeList[0], 1 );
                radianceCacheFreeList[1 + slot] = int( probe );
                radianceCacheIndirection[entry] = RADIANCE_CACHE_INVALID_PROBE;
            }
        }
    }
}
