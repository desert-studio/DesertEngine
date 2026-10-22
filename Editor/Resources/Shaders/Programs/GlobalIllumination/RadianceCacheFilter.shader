// DesertAsset {"Kind":"Shader","Guid":"7c06cceec4484446b5d95089bd3dfc91","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 6/6 — FILTER (UE's irradiance pass of the radiance cache, LumenRadianceCache.usf
// ConvertRadianceProbeToIrradianceCS / FixupBordersAndGenerateMips): one workgroup per probe traced this frame. Its
// radiance becomes the cosine-convolved irradiance probe the readers sample, and the mean and mean square of its
// rays' hit distances in a cos^50 lobe (DDGI's visibility moments, not in UE); all three atlases get their
// octahedral borders. Then the probe is published: its indirection entry loses the untraced bit, so readers use it.
Shader "RadianceCacheFilter"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/RadianceCacheState.glslh>

        layout(binding = 6, rgba16f) restrict uniform image2D RadianceCacheRadianceTexture;
        layout(binding = 7, rgba16f) restrict writeonly uniform image2D RadianceCacheIrradianceOutTexture;
        layout(binding = 8, rg16f) restrict writeonly uniform image2D RadianceCacheDepthOutTexture;

        // Traced resolution up to 32 (Scalability GlobalIllumination.RadianceCacheProbeResolution's range is 8..64:
        // a larger probe loads in slices below).
        #define RC_FILTER_MAX_TEXELS 1024u
        // DDGI's depth sharpness: the lobe the distance moments are averaged over.
        #define RC_DISTANCE_LOBE_EXPONENT 50.0

        shared vec4 s_Radiance[RC_FILTER_MAX_TEXELS];
        shared vec3 s_Irradiance[64];
        shared vec2 s_Distance[64];

        void main()
        {
            const uint count = min( radianceCacheSchedule[RC_SCHEDULE_COUNT], radianceCache.atlas.z );
            if ( gl_WorkGroupID.x >= count )
                return;
            const uint  probe   = radianceCacheSchedule[RC_SCHEDULE_LIST + gl_WorkGroupID.x];
            const uint  res     = radianceCache.shape.z;
            const uint  texels  = res * res;
            const ivec2 origin  = RadianceCacheProbeAtlasOrigin( radianceCache, probe, radianceCache.shape.w ) + 1;
            const uint  clipmap = uint( radianceCacheProbes[probe].cell.w );
            const float maxDistance = 2.0 * RadianceCacheCellSize( radianceCache, clipmap );
            const uint  irrRes  = radianceCache.atlas.y;
            const uint  depRes  = radianceCache.atlas.w;

            vec3  irradianceSum = vec3( 0.0 );
            float cosineSum     = 0.0;
            vec2  momentSum  = vec2( 0.0 );
            float lobeSum    = 0.0;
            const uint  local = gl_LocalInvocationIndex;
            const vec3  irrDirection = RadianceCacheOctDecode( ( vec2( local % irrRes, local / irrRes ) + 0.5 ) / float( irrRes ) * 2.0 - 1.0 );
            const vec3  depDirection = RadianceCacheOctDecode( ( vec2( local % depRes, local / depRes ) + 0.5 ) / float( depRes ) * 2.0 - 1.0 );

            for ( uint first = 0u; first < texels; first += RC_FILTER_MAX_TEXELS )
            {
                const uint slice = min( RC_FILTER_MAX_TEXELS, texels - first );
                for ( uint i = local; i < slice; i += 64u )
                {
                    const uint k = first + i;
                    s_Radiance[i] = imageLoad( RadianceCacheRadianceTexture, origin + ivec2( k % res, k / res ) );
                }
                barrier();
                for ( uint i = 0u; i < slice; ++i )
                {
                    const uint k         = first + i;
                    const vec3 direction = RadianceCacheOctDecode( ( vec2( k % res, k / res ) + 0.5 ) / float( res ) * 2.0 - 1.0 );
                    const vec4 sample_   = s_Radiance[i];
                    const float c = dot( irrDirection, direction );
                    if ( c > 0.0 )
                    {
                        irradianceSum += sample_.rgb * c;
                        cosineSum        += c;
                    }
                    const float l = dot( depDirection, direction );
                    if ( l > 0.0 )
                    {
                        const float w = pow( l, RC_DISTANCE_LOBE_EXPONENT );
                        const float d = min( sample_.a, maxDistance );
                        momentSum += w * vec2( d, d * d );
                        lobeSum   += w;
                    }
                }
                barrier();
            }
            // (1/pi) * integral of L cos over the hemisphere = sum(L cos) / sum(cos) for evenly spread directions.
            if ( local < irrRes * irrRes )
                s_Irradiance[local] = irradianceSum / max( cosineSum, 1e-4 );
            if ( local < depRes * depRes )
                s_Distance[local] = lobeSum > 0.0 ? momentSum / lobeSum : vec2( maxDistance, maxDistance * maxDistance );

            // Radiance borders (its interior is as traced).
            const int finalRes = int( res ) + 2;
            for ( uint i = local; i < uint( finalRes * finalRes ); i += 64u )
            {
                const ivec2 b = ivec2( i % uint( finalRes ), i / uint( finalRes ) ) - 1;
                const ivec2 s = RadianceCacheOctBorderSource( b, int( res ) );
                if ( s != b )
                    imageStore( RadianceCacheRadianceTexture, origin + b, imageLoad( RadianceCacheRadianceTexture, origin + s ) );
            }
            barrier();

            const ivec2 irrOrigin = RadianceCacheProbeAtlasOrigin( radianceCache, probe, irrRes + 2u ) + 1;
            const int   irrFinal  = int( irrRes ) + 2;
            for ( uint i = local; i < uint( irrFinal * irrFinal ); i += 64u )
            {
                const ivec2 b = ivec2( i % uint( irrFinal ), i / uint( irrFinal ) ) - 1;
                const ivec2 s = RadianceCacheOctBorderSource( b, int( irrRes ) );
                imageStore( RadianceCacheIrradianceOutTexture, irrOrigin + b, vec4( s_Irradiance[s.x + s.y * int( irrRes )], 1.0 ) );
            }
            const ivec2 depOrigin = RadianceCacheProbeAtlasOrigin( radianceCache, probe, depRes + 2u ) + 1;
            const int   depFinal  = int( depRes ) + 2;
            for ( uint i = local; i < uint( depFinal * depFinal ); i += 64u )
            {
                const ivec2 b = ivec2( i % uint( depFinal ), i / uint( depFinal ) ) - 1;
                const ivec2 s = RadianceCacheOctBorderSource( b, int( depRes ) );
                imageStore( RadianceCacheDepthOutTexture, depOrigin + b, vec4( s_Distance[s.x + s.y * int( depRes )], 0.0, 0.0 ) );
            }

            if ( local == 0u )
            {
                const RadianceCacheProbe state = radianceCacheProbes[probe];
                radianceCacheProbes[probe].frames = uvec4( state.frames.x, FrameIndex, state.frames.w, state.frames.w );
                radianceCacheIndirection[RadianceCacheIndirectionIndex( radianceCache, state.cell.xyz, clipmap )] = probe;
            }
        }
    }
}
