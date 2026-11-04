// DesertAsset {"Kind":"Shader","Guid":"e912e352bed746eab7c7aed8d0cda4a8","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 1/6 — MARK (UE MarkRadianceProbesUsedByScreenProbes, LumenScreenProbeGather.usf): every
// cell a pixel's interpolated read will touch (RadianceCacheIrradianceAt: the eight cells around its biased position,
// in each clipmap its dither may pick) is stamped with this frame. Until screen probes exist (GI-FINAL-GATHER) the
// readers are the G-buffer's pixels themselves; one pixel of each 2x2 block per frame, the four in turn, so every
// pixel marks within four frames — inside the eight a probe stays allocated unread.
// The other readers are the REFLECTION CAPTURES' points (ReflectionCaptureDiffuse.shader reads the cache there in
// the six axis directions): one thread stamps them every frame, since a capture hangs in the air where no pixel reads.
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

        // @p kind: RC_STAMP_COVERAGE for a clipmap the read interpolates itself, RC_STAMP_FALLBACK for the level a
        // screen probe's radiance read falls back to (RadianceCacheAllocate ranks the two apart).
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
        void MarkPoint( vec3 p )
        {
            const vec3 axes[6] = vec3[6]( vec3( 1, 0, 0 ), vec3( -1, 0, 0 ), vec3( 0, 1, 0 ), vec3( 0, -1, 0 ),
                                          vec3( 0, 0, 1 ), vec3( 0, 0, -1 ) );
            const uint fine   = RadianceCacheClipmapAt( radianceCache, p, 0.0 );
            const uint coarse = RadianceCacheClipmapAt( radianceCache, p, 0.9999 );
            for ( int i = 0; i < 6; ++i )
            {
                if ( fine < radianceCache.shape.x )
                    MarkClipmap( p, axes[i], axes[i], fine, RC_STAMP_COVERAGE );
                if ( coarse != fine && coarse < radianceCache.shape.x )
                    MarkClipmap( p, axes[i], axes[i], coarse, RC_STAMP_COVERAGE );
            }
        }

        void main()
        {
            if ( gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u )
            {
                // This frame's schedule header - the histogram, the counts and the update's counters - cleared by
                // the update's first pass, so Update counts its frees into it and Allocate its slots.
                for ( uint i = 0u; i < RC_SCHEDULE_LIST; ++i )
                    radianceCacheSchedule[i] = 0u;
                for ( uint i = 0u; i < MarkPointHeader.x; ++i )
                    MarkPoint( MarkPointsData[i].xyz );
            }
            const uvec2 pixel = gl_GlobalInvocationID.xy * 2u + uvec2( FrameIndex & 1u, ( FrameIndex >> 1 ) & 1u );
            if ( pixel.x >= Width || pixel.y >= Height )
                return;
            const vec3 normal = texelFetch( GBufferBTexture, ivec2( pixel ), 0 ).rgb;
            if ( dot( normal, normal ) <= 0.001 ) // no geometry (the G-buffer is cleared to zero)
                return;
            const vec2  uv    = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p     = ReconstructWorldPosition( uv, texelFetch( GBufferDepthTexture, ivec2( pixel ), 0 ).r,
                                                          giFrame.invViewProjection );
            const vec3  N     = normalize( normal );
            const vec3  V     = normalize( giFrame.camera.xyz - p );
            // The two ends of the dither: the finest clipmap any pixel here may read and the coarsest - the COVERAGE,
            // the clipmaps a read here interpolates itself (RadianceCacheCoverageAt).
            const uint fine   = RadianceCacheClipmapAt( radianceCache, p, 0.0 );
            const uint coarse = RadianceCacheClipmapAt( radianceCache, p, 0.9999 );
            if ( fine < radianceCache.shape.x )
                MarkClipmap( p, N, V, fine, RC_STAMP_COVERAGE );
            if ( coarse != fine && coarse < radianceCache.shape.x )
                MarkClipmap( p, N, V, coarse, RC_STAMP_COVERAGE );
            // THE FALLBACK: the clipmap past the coarse end, where a screen probe's radiance read looks while its own
            // clipmap's eight cells hold no traced probe (RadianceCacheRadianceAt). Stamped apart from the coverage
            // (RC_STAMP_FALLBACK): its new probes take what the coverage's new probes leave of the budget
            // (RC_PRIORITY_NEW_FALLBACK), so they never hold back the cells the reads interpolate themselves.
            // Only when the screen probes read the cache (MarkFallback): the irradiance readers never read it.
            if ( MarkFallback != 0u && coarse + 1u < radianceCache.shape.x )
                MarkClipmap( p, N, V, coarse + 1u, RC_STAMP_FALLBACK );
        }
    }
}
