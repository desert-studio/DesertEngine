// DesertAsset {"Kind":"Shader","Guid":"e912e352bed746eab7c7aed8d0cda4a8","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 1/5 — MARK AND FREE (UE MarkRadianceProbesUsedByScreenProbes, LumenScreenProbeGather.usf): every
// cell a pixel's interpolated read will touch (RadianceCacheIrradianceAt: the eight cells around its biased position,
// in each clipmap its dither may pick) is stamped with this frame. Until screen probes exist (GI-FINAL-GATHER) the
// readers are the G-buffer's pixels themselves; one pixel of each 2x2 block per frame, the four in turn, so every
// pixel marks within four frames — inside the eight a probe stays allocated unread.
// The other readers are the REFLECTION CAPTURES' points (ReflectionCaptureDiffuse.shader reads the cache there in
// the six axis directions): one thread stamps them every frame, since a capture hangs in the air where no pixel reads.
// The third are the off-screen readers' REQUESTS (RadianceCacheMarkRequests.glslh: a refraction path's end lit in
// place): the last MarkRequestRows group rows stamp last frame's occupied slots and clear them.
// THE FREE (UE UpdateCacheForUsedProbes, LumenRadianceCacheUpdate.usf), in the same dispatch: the group rows past the
// pixels' run one thread per PROBE SLOT (FPS-RADIANCE-CACHE: was its own pass, one thread per indirection entry -
// 48^3 x 4 = 442k threads for at most 16k live slots, and one more pass on the async chain). A live slot (the entry
// of its recorded cell holds it: it owns that entry) gives its slot back to the free list when the clipmap scrolled
// past its cell (stale) or no reader stamped its entry for longer than the keep (unused). The age is measured up to
// the LAST frame: this frame's stamps are written beside it, so a stamp it reads now counts as age 0 and one it does
// not see yet is the frame it would have aged from anyway - the decision the entry pass made the frame before, one
// frame later, still before this frame's Allocate takes slots. Each free is counted by why (RC_STAT_*).
Shader "RadianceCacheMark"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/RadianceCacheState.glslh>
        #include <Common/GIFrame.glslh>
        #include <Common/ReconstructPosition.glslh>

        ReadBuffer(6) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform sampler PointClampSampler;
        Uniform(7) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(8) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        ReadBuffer(9) RadianceCacheMarkPointsUB
        {
            uvec4 MarkPointHeader; // x = the number of points
            vec4  MarkPointsData[];    // xyz = world cm
        };
        // The cache's request table (WorldRadianceCache: RadianceCacheFrame::MarkRequests), written last frame by the
        // readers off screen; read and cleared here.
        #define RADIANCE_CACHE_MARK_REQUESTS_BINDING 16
        #include <Common/RadianceCacheMarkRequests.glslh>

        // A pixel's stamps, one BOX KEY per RC_MARK_LEVEL_* (the coverage's two dither ends, the fallback),
        // RC_MARK_NONE where it stamps none - kept per thread for the group's de-duplication below. A level's eight
        // cells are the box {lo, hi} per axis of its clipmap's grid, lo = clamp(base), hi = clamp(base + 1), base
        // the read's floor(coord - 0.5); the key packs clamp(base, -1, grid - 1) + 1 (0..grid, 9 bits an axis) and
        // the clipmap (bits 27..29), so two equal keys stamp the same eight words (the level fixes the kind) and the
        // key never reaches RC_MARK_NONE (bit 31 clear). WorldRadianceCache refuses a grid past 511 or a clipmap
        // count past 7 (kMarkKeyMaxGrid).
        #define RC_MARK_LEVEL_FINE     0u
        #define RC_MARK_LEVEL_COARSE   1u
        #define RC_MARK_LEVEL_FALLBACK 2u
        #define RC_MARK_LEVELS         3u
        #define RC_MARK_NONE           0xFFFFFFFFu
        shared uint s_Key[RC_MARK_LEVELS][64];
        // FPS-RADIANCE-CACHE-BRDF: the group's readers' normals as BRDF bins (RadianceCacheBrdfBin), ORed into every
        // coverage entry the group stamps (RC_ENTRY_BRDF_BINS, Select's supersampling). One mask for the group's
        // 16x16 pixels: a cell is written by one thread of the group for all its readers (the de-duplication below),
        // and a superset only supersamples a square more.
        shared uint s_Bins[2];

        uint MarkKey( vec3 p, vec3 N, vec3 V, uint clipmap )
        {
            const vec3  biased = RadianceCacheReadPoint( radianceCache, p, N, V, clipmap );
            const ivec3 base   = ivec3( floor( RadianceCacheCoordFloat( radianceCache, biased, clipmap ) - 0.5 ) );
            const uvec3 b      = uvec3( clamp( base, ivec3( -1 ), ivec3( int( radianceCache.shape.y ) - 1 ) ) + 1 );
            return b.x | ( b.y << 9 ) | ( b.z << 18 ) | ( clipmap << 27 );
        }

        // The key's box: per axis lo = clamp(base) and hi = clamp(base + 1), local cells of its clipmap.
        void MarkKeyBox( uint key, out ivec3 lo, out ivec3 hi )
        {
            const ivec3 s = ivec3( key & 511u, ( key >> 9 ) & 511u, ( key >> 18 ) & 511u );
            lo            = max( s - 1, ivec3( 0 ) );
            hi            = min( s, ivec3( int( radianceCache.shape.y ) - 1 ) );
        }

        // Whether local cell @p cell lies in the box of @p key (a neighbour's key of the same level and clipmap).
        bool MarkKeyHolds( uint key, ivec3 cell )
        {
            ivec3 lo, hi;
            MarkKeyBox( key, lo, hi );
            return ( cell.x == lo.x || cell.x == hi.x ) && ( cell.y == lo.y || cell.y == hi.y ) &&
                   ( cell.z == lo.z || cell.z == hi.z );
        }

        // Per axis @p b where @p pick, else @p a.
        ivec3 MarkSelect( ivec3 a, ivec3 b, bvec3 pick )
        {
            return ivec3( pick.x ? b.x : a.x, pick.y ? b.y : a.y, pick.z ? b.z : a.z );
        }

        // THE CLIPMAP SELECT ONCE (UE GetRadianceProbeClipmap): the dither's two ends - the finest clipmap whose edge
        // fade exceeds 0 and the finest whose fade exceeds 0.9999 - from one walk over the clipmaps, each fade
        // computed once; RadianceCacheClipmapAt( p, 0.0 ) and ( p, 0.9999 ) walked them twice. x = fine, y = coarse
        // (shape.x where none).
        uvec2 MarkDitherEnds( vec3 p )
        {
            const uint count = radianceCache.shape.x;
            uvec2      ends  = uvec2( count );
            for ( uint clipmap = 0u; clipmap < count; ++clipmap )
            {
                const float fade = RadianceCacheClipmapEdgeFade( radianceCache, p, clipmap );
                if ( ends.x == count && fade > 0.0 )
                    ends.x = clipmap;
                if ( fade > 0.9999 )
                {
                    ends.y = clipmap;
                    break;
                }
            }
            return ends;
        }

        // x mod grid for x of any sign (RadianceCacheIndirectionIndex's ((x % g) + g) % g, one remainder).
        ivec3 MarkWrap( ivec3 x, int grid )
        {
            const ivec3 r = x % grid;
            return r + ivec3( lessThan( r, ivec3( 0 ) ) ) * grid;
        }

        // The capture points' stamps (MarkPoint), written directly: a few points, no neighbours to share with.
        void MarkClipmap( vec3 p, vec3 N, vec3 V, uint clipmap, uint kind )
        {
            // The read's own point (RadianceCacheReadPoint): the irradiance reads and the screen probes' radiance
            // read interpolate exactly these eight cells for this pixel.
            const vec3  biased = RadianceCacheReadPoint( radianceCache, p, N, V, clipmap );
            const ivec3 base   = ivec3( floor( RadianceCacheCoordFloat( radianceCache, biased, clipmap ) - 0.5 ) );
            const ivec3 corner = RadianceCacheCornerCell( radianceCache, clipmap );
            const int   grid   = int( radianceCache.shape.y );
            for ( uint i = 0u; i < 8u; ++i )
            {
                const ivec3 local = clamp( base + ivec3( i & 1u, ( i >> 1 ) & 1u, ( i >> 2 ) & 1u ), ivec3( 0 ),
                                           ivec3( grid - 1 ) );
                const uint entry = RadianceCacheIndirectionIndex( radianceCache, corner + local, clipmap );
                radianceCacheLastUsed[RadianceCacheStampIndex( entry, kind )] = FrameIndex;
            }
        }

        // A point read in the six axis directions with the bias along the normal (V = N), at both ends of the
        // clipmap dither: every cell its read can touch.
        // One axis of a point read in the six axis directions with the bias along the normal (V = N), at both ends
        // of the clipmap dither: every cell that read can touch.
        void MarkPointAxis( vec3 p, uint axis )
        {
            vec3 N = vec3( 0.0 );
            N[axis / 2u] = ( axis & 1u ) == 0u ? 1.0 : -1.0;
            const uvec2 ends   = MarkDitherEnds( p );
            const uint  fine   = ends.x;
            const uint  coarse = ends.y;
            if ( fine < radianceCache.shape.x )
                MarkClipmap( p, N, N, fine, RC_STAMP_COVERAGE );
            if ( coarse != fine && coarse < radianceCache.shape.x )
                MarkClipmap( p, N, N, coarse, RC_STAMP_COVERAGE );
        }

        // FPS-SP-LIST-ATOMICS (UE BuildListsCS group reservation): the frees' counts and their pushes onto the free
        // list are added once per group - one count atomic per freed slot and one push per slot, all on the same few
        // words, serialised: a scroll or a turn frees thousands of probes a frame.
        shared uint s_Freed[3]; // the group's frees by why: RC_STAT_FREED_STALE + i
        shared uint s_Push;     // the group's slots pushed
        shared int  s_PushBase; // the list's count before them: the first one's slot

        // One group's frees (every thread of it calls this, the barriers): @p why the thread's slot's RC_STAT_FREED_*
        // or ~0u, @p push whether it pushes @p probe.
        void FlushFrees( const uint why, const bool push, const uint probe )
        {
            const uint t = gl_LocalInvocationIndex;
            if ( t < 3u )
                s_Freed[t] = 0u;
            if ( t == 0u )
                s_Push = 0u;
            barrier();
            if ( why != ~0u )
                atomicAdd( s_Freed[why - RC_STAT_FREED_STALE], 1u );
            uint local = 0u;
            if ( push )
                local = atomicAdd( s_Push, 1u );
            barrier();
            if ( t == 0u && s_Push != 0u )
            {
                const int count = int( s_Push );
                const int base  = atomicAdd( radianceCacheFreeList[0], count );
                // A full list means it already holds a doubled slot: a push that would land on the failed-
                // allocation words or past the buffer, where a pop reads slot 0, is dropped instead.
                const int dropped = clamp( base + count - int( MaxProbes ), 0, count );
                if ( dropped > 0 )
                    atomicAdd( radianceCacheFreeList[0], -dropped );
                s_PushBase = base;
            }
            if ( t < 3u && s_Freed[t] != 0u )
                atomicAdd( radianceCacheSchedule[RC_SCHEDULE_STATS + RC_STAT_FREED_STALE + t], s_Freed[t] );
            barrier();
            if ( push )
            {
                const int slot = s_PushBase + int( local );
                if ( slot < int( MaxProbes ) )
                    radianceCacheFreeList[1 + slot] = int( probe );
            }
        }

        // The free of probe slot @p slot (every thread of a free group calls it: FlushFrees' barriers).
        void FreeSlot( const uint slot, const bool firstGroup )
        {
            // After a frame whose reads found the atlas full, a probe unread for the pixels' rotation frees its slot.
            const bool pressed =
                 radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex - 1u )] > 0;
            // This frame's failed allocations, counted by Allocate after this pass (the last frame's are the other
            // parity's word, read above).
            if ( firstGroup && gl_LocalInvocationIndex == 0u )
            {
                radianceCacheFreeList[RadianceCacheFailedAllocationsWord( MaxProbes, FrameIndex )] = 0;
                radianceCacheSchedule[RC_SCHEDULE_STATS + RC_STAT_PRESSED] = pressed ? 1u : 0u;
            }
            uint        why     = ~0u; // the slot's RC_STAT_FREED_*, ~0u: kept (or free, or past the slots)
            bool        push    = false;
            const ivec4 cell    = slot < MaxProbes ? radianceCacheProbes[slot].cell : ivec4( 0, 0, 0, -1 );
            const uint  clipmap = uint( cell.w );
            if ( clipmap < radianceCache.shape.x )
            {
                // ONLY THE OWNER FREES THE SLOT. The probe's cell names the one entry it was allocated to (the
                // toroidal index of that cell, which a scroll keeps); the slot is live while that entry holds it. A
                // free slot (its entry emptied, or holding another slot since) is no entry's and is never pushed
                // again (GPU-TURN-2: a slot pushed while live churned for ever).
                const uint entry = RadianceCacheIndirectionIndex( radianceCache, cell.xyz, clipmap );
                if ( ( radianceCacheIndirection[entry] & ~RADIANCE_CACHE_PROBE_UNTRACED ) == slot )
                {
                    // Stale: the clipmap's window (its corner and grid) no longer holds the cell.
                    const ivec3 local = cell.xyz - RadianceCacheCornerCell( radianceCache, clipmap );
                    const bool  stale = any( lessThan( local, ivec3( 0 ) ) ) ||
                                       any( greaterThanEqual( local, ivec3( int( radianceCache.shape.y ) ) ) );
                    const uint  keep =
                         pressed ? min( UnusedFrames, RADIANCE_CACHE_PRESSURE_KEEP_FRAMES ) : UnusedFrames;
                    // Read by either kind of reader (RadianceCacheState.glslh, RadianceCacheLastUsedUB): the later
                    // stamp; one of this frame's (written beside this read) is age 0.
                    const uint lastUsed =
                         max( radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_COVERAGE )],
                              radianceCacheLastUsed[RadianceCacheStampIndex( entry, RC_STAMP_FALLBACK )] );
                    const uint age = lastUsed == FrameIndex ? 0u : ( FrameIndex - 1u ) - lastUsed;
                    if ( stale || age > keep )
                    {
                        why = stale ? RC_STAT_FREED_STALE
                                    : ( age > UnusedFrames ? RC_STAT_FREED_UNUSED : RC_STAT_FREED_PRESSURE );
                        radianceCacheIndirection[entry] = RADIANCE_CACHE_INVALID_PROBE;
                        push                            = true;
                    }
                }
            }
            FlushFrees( why, push, slot );
        }

        // THE REQUESTS (RadianceCacheMarkRequests.glslh): the occupied slot @p slot is stamped as a pixel at its point
        // would be - the eight cells around the read point in each clipmap of the dither (MarkClipmap, the read's own
        // point: V = N = the request's bias direction gives RadianceCacheReadPoint's point) - and, around the point
        // itself (zero bias), the coarser clipmaps GIHitBounceIrradiance falls back to; then it is cleared.
        void MarkRequest( const uint slot )
        {
            vec3 p, bias;
            if ( slot >= RadianceCacheMarkRequestSlots() || !RadianceCacheMarkRequestRead( slot, p, bias ) )
                return;
            const uvec2 ends   = MarkDitherEnds( p );
            const uint  fine   = ends.x;
            const uint  coarse = ends.y;
            if ( fine < radianceCache.shape.x )
                MarkClipmap( p, bias, bias, fine, RC_STAMP_COVERAGE );
            if ( coarse != fine && coarse < radianceCache.shape.x )
            {
                MarkClipmap( p, bias, bias, coarse, RC_STAMP_COVERAGE );
                // A read in the fine end falls back to the coarse end (GIHitBounceIrradiance, unbiased point).
                MarkClipmap( p, vec3( 0.0 ), vec3( 0.0 ), coarse, RC_STAMP_FALLBACK );
            }
            // The next coarser clipmap, as a pixel's RC_MARK_LEVEL_FALLBACK: stamped apart from the coverage, so its
            // new probes do not take the coverage's budget.
            if ( coarse + 1u < radianceCache.shape.x )
                MarkClipmap( p, vec3( 0.0 ), vec3( 0.0 ), coarse + 1u, RC_STAMP_FALLBACK );
            RadianceCacheMarkRequestClear( slot );
        }

        void main()
        {
            // The group rows past the pixels' (WorldRadianceCache: Mark's dispatch) free the probe slots, one a
            // thread; the last MarkRequestRows rows walk the request table, one slot a thread. A frame that
            // initialises the state (Reset, RadianceCacheAllocate.shader) frees nothing.
            const uint pixelRows   = max( 1u, ( ( Height + 1u ) / 2u + 7u ) / 8u );
            const uint requestRow0 = gl_NumWorkGroups.y - MarkRequestRows;
            if ( gl_WorkGroupID.y >= requestRow0 )
            {
                const uint group = ( gl_WorkGroupID.y - requestRow0 ) * gl_NumWorkGroups.x + gl_WorkGroupID.x;
                MarkRequest( group * 64u + gl_LocalInvocationIndex );
                return;
            }
            if ( gl_WorkGroupID.y >= pixelRows )
            {
                if ( Reset != 0u ) // a push constant: the whole group returns here, before any barrier
                    return;
                const uint group = ( gl_WorkGroupID.y - pixelRows ) * gl_NumWorkGroups.x + gl_WorkGroupID.x;
                FreeSlot( group * 64u + gl_LocalInvocationIndex, group == 0u );
                return;
            }
            // The capture points, one (point, axis) per thread of the dispatch (GPU-SCREENPROBE-6: one thread
            // walked every point's 6 axes in series while its group waited).
            const uint pointThread = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 8u;
            if ( pointThread < MarkPointHeader.x * 6u )
                MarkPointAxis( MarkPointsData[pointThread / 6u].xyz, pointThread % 6u );

            if ( gl_LocalInvocationIndex < 2u )
                s_Bins[gl_LocalInvocationIndex] = 0u;
            barrier();
            for ( uint level = 0u; level < RC_MARK_LEVELS; ++level )
                s_Key[level][gl_LocalInvocationIndex] = RC_MARK_NONE;
            const uvec2 pixel = gl_GlobalInvocationID.xy * 2u + uvec2( FrameIndex & 1u, ( FrameIndex >> 1 ) & 1u );
            const bool  inside = pixel.x < Width && pixel.y < Height;
            const vec3  normal = inside ? texelFetch( GBufferBTexture, ivec2( pixel ), 0 ).rgb : vec3( 0.0 );
            if ( dot( normal, normal ) > 0.001 ) // geometry (the G-buffer is cleared to zero)
            {
                const vec2 uv = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
                const float depth = texelFetch( GBufferDepthTexture, ivec2( pixel ), 0 ).r;
                const vec3  p     = ReconstructWorldPosition( uv, depth, giFrame.invViewProjection );
                const vec3 N  = normalize( normal );
                const vec3 V  = normalize( giFrame.camera.xyz - p );
                // The two ends of the dither: the finest clipmap any pixel here may read and the coarsest - the
                // COVERAGE, the clipmaps a read here interpolates itself (RadianceCacheCoverageAt).
                const uvec2 ends   = MarkDitherEnds( p );
                const uint  fine   = ends.x;
                const uint  coarse = ends.y;
                const uint  bin    = RadianceCacheBrdfBin( N );
                atomicOr( s_Bins[bin / 32u], 1u << ( bin % 32u ) );
                if ( fine < radianceCache.shape.x )
                    s_Key[RC_MARK_LEVEL_FINE][gl_LocalInvocationIndex] = MarkKey( p, N, V, fine );
                if ( coarse != fine && coarse < radianceCache.shape.x )
                    s_Key[RC_MARK_LEVEL_COARSE][gl_LocalInvocationIndex] = MarkKey( p, N, V, coarse );
                // THE FALLBACK: the clipmap past the coarse end, where a screen probe's radiance read looks
                // while its own clipmap's eight cells hold no traced probe (RadianceCacheRadianceAt). Stamped
                // apart from the coverage (RC_STAMP_FALLBACK): its new probes take what the coverage's new probes
                // leave of the budget (RC_PRIORITY_NEW_FALLBACK), so they never hold back the cells the reads
                // interpolate themselves. Only when the screen probes read the cache (MarkFallback): the
                // irradiance readers never read it.
                if ( MarkFallback != 0u && coarse + 1u < radianceCache.shape.x )
                    s_Key[RC_MARK_LEVEL_FALLBACK][gl_LocalInvocationIndex] = MarkKey( p, N, V, coarse + 1u );
            }
            barrier();

            // ONE WRITE PER CELL AND RUN OF NEIGHBOURS (GPU-SCREENPROBE-6, FPS-RC-MARK): neighbouring pixels read
            // the same cells, so a cell of a level is written unless an EARLIER neighbour of the group (left,
            // upper-left, upper, upper-right: a lower invocation index) holds it in its own box of the same level
            // and clipmap - that one writes it, or skips it for the same reason, and the chain of ever lower
            // indices ends at a thread with no such neighbour, which writes. A box whose clamped axis collapses
            // (lo == hi) holds each cell twice; only its first copy is weighed. A neighbour with the whole key
            // skips the level at once. The words are computed only for the cells written: one wrap of the box's
            // low corner per axis (the high one is the next entry round the torus), no per-cell
            // RadianceCacheIndirectionIndex. The set stamped is the one the eight-words-per-level walk stamped.
            const uint x = gl_LocalInvocationID.x;
            const uint y = gl_LocalInvocationID.y;
            const uint t = gl_LocalInvocationIndex;
            const int   grid = int( radianceCache.shape.y );
            const uvec2 bins = uvec2( s_Bins[0], s_Bins[1] );
            for ( uint level = 0u; level < RC_MARK_LEVELS; ++level )
            {
                const uint key = s_Key[level][t];
                if ( key == RC_MARK_NONE )
                    continue;
                // The earlier neighbours' keys of this level and clipmap; RC_MARK_NONE where there is none.
                uint neighbours[4] = uint[4]( RC_MARK_NONE, RC_MARK_NONE, RC_MARK_NONE, RC_MARK_NONE );
                if ( x > 0u )
                    neighbours[0] = s_Key[level][t - 1u];
                if ( y > 0u )
                {
                    neighbours[1] = s_Key[level][t - 8u];
                    if ( x > 0u )
                        neighbours[2] = s_Key[level][t - 9u];
                    if ( x < 7u )
                        neighbours[3] = s_Key[level][t - 7u];
                }
                bool whole = false;
                for ( uint n = 0u; n < 4u; ++n )
                {
                    if ( neighbours[n] == key )
                        whole = true;
                    if ( ( neighbours[n] >> 27 ) != ( key >> 27 ) )
                        neighbours[n] = RC_MARK_NONE; // another clipmap (or none): holds none of these cells
                }
                if ( whole )
                    continue;

                const uint  clipmap = key >> 27;
                const uint  kind    = level == RC_MARK_LEVEL_FALLBACK ? RC_STAMP_FALLBACK : RC_STAMP_COVERAGE;
                ivec3       lo, hi;
                MarkKeyBox( key, lo, hi );
                const ivec3 corner = RadianceCacheCornerCell( radianceCache, clipmap );
                const ivec3 wrapLo = MarkWrap( corner + lo, grid );
                const ivec3 next   = wrapLo + 1;
                const ivec3 wrapHi =
                     MarkSelect( wrapLo, MarkSelect( next, ivec3( 0 ), equal( next, ivec3( grid ) ) ),
                                 notEqual( hi, lo ) );
                const bvec3 collapsed = equal( hi, lo );
                const uint  width  = radianceCache.shape.y * radianceCache.shape.x;
                for ( uint i = 0u; i < 8u; ++i )
                {
                    const bvec3 up = bvec3( ( i & 1u ) != 0u, ( i & 2u ) != 0u, ( i & 4u ) != 0u );
                    if ( ( up.x && collapsed.x ) || ( up.y && collapsed.y ) || ( up.z && collapsed.z ) )
                        continue; // the copy of a cell with that bit clear
                    const ivec3 cell = MarkSelect( lo, hi, up );
                    bool        held = false;
                    for ( uint n = 0u; n < 4u; ++n )
                        if ( neighbours[n] != RC_MARK_NONE && MarkKeyHolds( neighbours[n], cell ) )
                            held = true;
                    if ( held )
                        continue;
                    const uvec3 m     = uvec3( MarkSelect( wrapLo, wrapHi, up ) );
                    const uint  entry = m.x + clipmap * radianceCache.shape.y + width * ( m.y + radianceCache.shape.y * m.z );
                    radianceCacheLastUsed[RadianceCacheStampIndex( entry, kind )] = FrameIndex;
                    // The read directions (RC_ENTRY_BRDF_BINS): only the coverage's (a fallback-only probe traces
                    // at a quarter, unsupersampled), and only the bits the entry lacks (a read is cheaper than an
                    // atomic, and a surface's bins are set from its first frame until its probe's next trace).
                    if ( kind == RC_STAMP_COVERAGE )
                    {
                        const uint w = RadianceCacheStampIndex( entry, RC_ENTRY_BRDF_BINS );
                        if ( ( radianceCacheLastUsed[w] & bins.x ) != bins.x )
                            atomicOr( radianceCacheLastUsed[w], bins.x );
                        if ( ( radianceCacheLastUsed[w + 1u] & bins.y ) != bins.y )
                            atomicOr( radianceCacheLastUsed[w + 1u], bins.y );
                    }
                }
            }
        }
    }
}
