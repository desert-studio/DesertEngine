// DesertAsset {"Kind":"Shader","Guid":"2359ee2c998c43d68dfaeed7c97a113f","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 2/7 — FREE (UE UpdateCacheForUsedProbes, LumenRadianceCacheUpdate.usf): one thread per
// indirection entry. An entry whose probe records another world cell (the clipmap scrolled past it) or that no
// pixel read (as its coverage or as the fallback) for RadianceCache::kProbeUnusedFramesBeforeFree frames returns its
// slot to the free list. With Reset it instead initialises the state: every entry empty, every slot free. Each free
// is counted by why (RC_STAT_*, the schedule header Mark cleared).
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
            // After a frame whose reads found the atlas full, a probe unread for the pixels' rotation frees its slot.
            const bool pressed =
                 Reset == 0u && radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex - 1u )] > 0;
            // This frame's failed allocations, counted by Allocate after this pass (the last frame's are the other
            // parity's word, read above).
            if ( entry == 0u )
            {
                radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex )] = 0;
                radianceCacheSchedule[RC_SCHEDULE_STATS + RC_STAT_PRESSED] = pressed ? 1u : 0u;
            }
            if ( Reset != 0u )
            {
                radianceCacheIndirection[entry] = RADIANCE_CACHE_INVALID_PROBE;
                radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_COVERAGE )] = 0u;
                radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_FALLBACK )] = 0u;
                if ( entry < MaxProbes )
                    radianceCacheFreeList[1u + entry] = int( entry );
                if ( entry == 0u )
                {
                    radianceCacheFreeList[0] = int( MaxProbes );
                    radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex + 1u )] = 0;
                }
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
            const uint  keep    = pressed ? min( UnusedFrames, RADIANCE_CACHE_PRESSURE_KEEP_FRAMES ) : UnusedFrames;
            // Read by either kind of reader (RadianceCacheState.glslh, RadianceCacheLastUsedUB): the later stamp.
            const uint  lastUsed = max( radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_COVERAGE )],
                                        radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_FALLBACK )] );
            const uint  age     = FrameIndex - lastUsed;
            const bool  unused  = age > keep;
            if ( stale || unused )
            {
                const uint why = stale ? RC_STAT_FREED_STALE
                                       : ( age > UnusedFrames ? RC_STAT_FREED_UNUSED : RC_STAT_FREED_PRESSURE );
                atomicAdd( radianceCacheSchedule[RC_SCHEDULE_STATS + why], 1u );
                radianceCacheIndirection[entry] = RADIANCE_CACHE_INVALID_PROBE;
                // ONLY THE OWNER FREES THE SLOT. The probe's cell names the one entry it was allocated to (the
                // toroidal index of that cell, which a scroll keeps). An entry whose probe names another entry
                // holds a second reference to a slot that entry owns: pushing it would put a live slot on the
                // free list, the next allocation would take it from under its owner, the owner's cell would go
                // stale the frame after and push it again - a churn that never ends once one slot is doubled
                // (GPU-TURN-2: the turn freed 3921 "stale" probes a frame under a camera that did not move, so
                // its probes never lived to be traced). Such an entry only lets go of the reference.
                const bool owns = uint( owner.w ) < radianceCache.shape.x &&
                     RadianceCacheIndirectionIndex( radianceCache, owner.xyz, uint( owner.w ) ) == entry;
                if ( owns )
                {
                    const int slot = atomicAdd( radianceCacheFreeList[0], 1 );
                    // A full list means it already holds a doubled slot: the push would land on the failed-
                    // allocation words or past the buffer, where a pop reads slot 0. It is dropped instead.
                    if ( slot < int( MaxProbes ) )
                        radianceCacheFreeList[1 + slot] = int( probe );
                    else
                        atomicAdd( radianceCacheFreeList[0], -1 );
                }
            }
        }
    }
}
