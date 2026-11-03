// DesertAsset {"Kind":"Shader","Guid":"e912e352bed746eab7c7aed8d0cda4a8","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 1/6 — MARK (UE MarkRadianceProbesUsedByScreenProbes, LumenScreenProbeGather.usf): every
// cell a pixel's interpolated read will touch (RadianceCacheIrradianceAt: the eight cells around its biased position,
// in each clipmap its dither may pick) is stamped with this frame. Until screen probes exist (GI-FINAL-GATHER) the
// readers are the G-buffer's pixels themselves; one pixel of each 2x2 block per frame, the four in turn, so every
// pixel marks within four frames — inside the eight a probe stays allocated unread.
// With screen probes (MarkRadianceReads) a pixel is also where a probe sits, and the probe's far field reads the
// cache's RADIANCE (RadianceCacheRadianceAt, ScreenProbeTrace.shader) around its UNBIASED position in the clipmap
// its coverage picked: those eight cells are stamped as well, at both ends of the same dither (UE
// MarkRadianceProbesUsedByScreenProbes marks the probe's own position, the point its read interpolates around).
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

        // The largest dither a reader passes (a float below 1: the screen probes' 16-bit noise, the pixels'
        // interleaved gradient noise): the coarsest clipmap any of them can pick at a point.
        const float kCoarsestDither = 0.99999994;

        // The eight cells of an interpolated read whose lower corner cell is @p base (both reads' loops).
        void MarkCells( ivec3 base, uint clipmap )
        {
            const ivec3 corner = RadianceCacheCornerCell( radianceCache, clipmap );
            const int   grid   = int( radianceCache.shape.y );
            for ( uint i = 0u; i < 8u; ++i )
            {
                const ivec3 local = clamp( base + ivec3( i & 1u, ( i >> 1 ) & 1u, ( i >> 2 ) & 1u ), ivec3( 0 ),
                                           ivec3( grid - 1 ) );
                radianceCacheLastUsed[RadianceCacheIndirectionIndex( radianceCache, corner + local, clipmap )] =
                     FrameIndex;
            }
        }

        // The irradiance read's lower corner (RadianceCacheIrradianceAtImpl: around @p p biased off the surface).
        ivec3 IrradianceReadBase( vec3 p, vec3 N, vec3 V, uint clipmap )
        {
            const float cell   = RadianceCacheCellSize( radianceCache, clipmap );
            const vec3  biased = p + ( N * 0.2 + V * 0.8 ) * ( 0.3 * cell );
            return ivec3( floor( RadianceCacheCoordFloat( radianceCache, biased, clipmap ) - 0.5 ) );
        }

        void MarkClipmap( vec3 p, vec3 N, vec3 V, uint clipmap )
        {
            MarkCells( IrradianceReadBase( p, N, V, clipmap ), clipmap );
        }

        // A pixel: the irradiance read's eight cells and, where a screen probe there reads the radiance, that
        // read's (RadianceCacheRadianceAt: around the unbiased position) when they are other cells.
        void MarkPixelClipmap( vec3 p, vec3 N, vec3 V, uint clipmap )
        {
            const ivec3 irradiance = IrradianceReadBase( p, N, V, clipmap );
            MarkCells( irradiance, clipmap );
            if ( MarkRadianceReads == 0u )
                return;
            const ivec3 radiance = ivec3( floor( RadianceCacheCoordFloat( radianceCache, p, clipmap ) - 0.5 ) );
            if ( any( notEqual( radiance, irradiance ) ) )
                MarkCells( radiance, clipmap );
        }

        // A point read in the six axis directions with the bias along the normal (V = N), at both ends of the
        // clipmap dither: every cell its read can touch.
        void MarkPoint( vec3 p )
        {
            const vec3 axes[6] = vec3[6]( vec3( 1, 0, 0 ), vec3( -1, 0, 0 ), vec3( 0, 1, 0 ), vec3( 0, -1, 0 ),
                                          vec3( 0, 0, 1 ), vec3( 0, 0, -1 ) );
            const uint fine   = RadianceCacheClipmapAt( radianceCache, p, 0.0 );
            const uint coarse = RadianceCacheClipmapAt( radianceCache, p, kCoarsestDither );
            for ( int i = 0; i < 6; ++i )
            {
                if ( fine < radianceCache.shape.x )
                    MarkClipmap( p, axes[i], axes[i], fine );
                if ( coarse != fine && coarse < radianceCache.shape.x )
                    MarkClipmap( p, axes[i], axes[i], coarse );
            }
        }

        void main()
        {
            if ( gl_GlobalInvocationID.x == 0u && gl_GlobalInvocationID.y == 0u )
                for ( uint i = 0u; i < MarkPointHeader.x; ++i )
                    MarkPoint( MarkPointsData[i].xyz );
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
            // The two ends of the dither: the finest clipmap any pixel here may read and the coarsest.
            const uint fine   = RadianceCacheClipmapAt( radianceCache, p, 0.0 );
            const uint coarse = RadianceCacheClipmapAt( radianceCache, p, kCoarsestDither );
            if ( fine < radianceCache.shape.x )
                MarkPixelClipmap( p, N, V, fine );
            if ( coarse != fine && coarse < radianceCache.shape.x )
                MarkPixelClipmap( p, N, V, coarse );
        }
    }
}
