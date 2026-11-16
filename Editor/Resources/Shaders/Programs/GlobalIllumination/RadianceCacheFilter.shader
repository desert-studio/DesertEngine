// DesertAsset {"Kind":"Shader","Guid":"7c06cceec4484446b5d95089bd3dfc91","Versions":{"SHDR":1},"Dependencies":[]}
// THE RADIANCE CACHE UPDATE, 7/7 — FILTER (UE's irradiance pass of the radiance cache, LumenRadianceCache.usf
// ConvertRadianceProbeToIrradianceCS / FixupBordersAndGenerateMips): one workgroup per probe traced this frame. Its
// radiance becomes the cosine-convolved irradiance probe the readers sample, and the mean and mean square of its
// rays' hit distances in a cos^50 lobe (DDGI's visibility moments, not in UE); all three atlases get their
// octahedral borders. Then the probe is published: its indirection entry loses the untraced bit, so readers use it.
Shader "RadianceCacheFilter"
{
    Compute
    {
        // One thread per (output texel, part of the traced texels): each output's sum over the probe's res^2 texels
        // is cut in RC_FILTER_SPLIT runs, summed by that many threads at once and added up in shared memory
        // (GPU-SCREENPROBE-6: 64 threads walking all 1024 texels in series left 100 groups latency-bound).
        // FPS-RC-DIRTY s2: the depth outputs (8x8) and the irradiance outputs (6x6) are separate threads in
        // separate warps - threads [0, 192) depth, [192, 300) irradiance - so no warp walks the texels for an
        // irradiance output past the 36th (the old 64 x 4 layout did both sums in every thread, 28 of 64 outputs
        // of the irradiance one for nothing). The last warp's lanes past RC_FILTER_JOBS only load.
        LocalSize(320, 1, 1);

        #include <Common/RadianceCacheState.glslh>

        layout(binding = 6, rgba16f) restrict uniform image2D RadianceCacheRadianceTexture;
        layout(binding = 7, rgba16f) restrict writeonly uniform image2D RadianceCacheIrradianceOutTexture;
        layout(binding = 8, rg16f) restrict writeonly uniform image2D RadianceCacheDepthOutTexture;

        // Traced resolution up to 32 (Scalability GlobalIllumination.RadianceCacheProbeResolution's range is 8..64:
        // a larger probe loads in slices below).
        #define RC_FILTER_MAX_TEXELS 1024u
        // DDGI's depth sharpness: the distance moments are averaged over a cos^50 lobe (the multiply chain in main),
        // over the texels inside it only: below this cosine l^50 < 1e-4, and the whole tail outside it weighs
        // 2 pi 0.8318^51 / 51 = 1e-5 sr of the lobe's 2 pi / 51 = 0.123 sr (8e-5 relative, under RG16F's 4.9e-4).
        #define RC_DISTANCE_LOBE_MIN_COSINE 0.8318

        // The outputs: kDepthProbeResolution^2 and kIrradianceProbeResolution^2 at most (static_assert in
        // WorldRadianceCache.cpp), each summed by RC_FILTER_SPLIT threads.
        #define RC_FILTER_DEPTH_OUTPUTS 64u
        #define RC_FILTER_IRRADIANCE_OUTPUTS 36u
        #define RC_FILTER_SPLIT 3u
        #define RC_FILTER_DEPTH_THREADS ( RC_FILTER_DEPTH_OUTPUTS * RC_FILTER_SPLIT )
        #define RC_FILTER_JOBS ( RC_FILTER_DEPTH_THREADS + RC_FILTER_IRRADIANCE_OUTPUTS * RC_FILTER_SPLIT )
        // LocalSize: RC_FILTER_JOBS (300) rounded up to whole 32-lane warps.
        #define RC_FILTER_THREADS 320u

        // FPS-RC-DIRTY: after the last slice the parts' sums take the slice arrays' first RC_FILTER_JOBS entries
        // (s_Radiance: rgb = irradiance sum, a = cosine sum; s_Direction: xy = moment sum, z = lobe sum) instead of
        // two arrays of their own: 29 KB of shared memory a group, not 36, so three groups share an SM, not two.
        shared vec4 s_Radiance[RC_FILTER_MAX_TEXELS];
        // Each loaded texel's direction, decoded once by its loader (not once per output thread).
        shared vec3 s_Direction[RC_FILTER_MAX_TEXELS];
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
            // The moments are stored in units of this range (RadianceCacheDistanceMomentRange): 0..1 in RG16F.
            const float maxDistance = RadianceCacheDistanceMomentRange( radianceCache, clipmap );
            const uint  irrRes  = radianceCache.atlas.y;
            const uint  depRes  = radianceCache.atlas.w;

            vec3  irradianceSum = vec3( 0.0 );
            float cosineSum     = 0.0;
            vec2  momentSum  = vec2( 0.0 );
            float lobeSum    = 0.0;
            // The flat thread index (all RC_FILTER_THREADS); its kind (depth below RC_FILTER_DEPTH_THREADS, a whole
            // number of warps, then irradiance), the output texel it sums for, and its part. A lane past
            // RC_FILTER_JOBS, or an output past the probe's res^2, sums nothing.
            const uint  thread  = gl_LocalInvocationIndex;
            const bool  isDepth = thread < RC_FILTER_DEPTH_THREADS;
            const uint  job     = isDepth ? thread : thread - RC_FILTER_DEPTH_THREADS;
            const uint  outputs = isDepth ? RC_FILTER_DEPTH_OUTPUTS : RC_FILTER_IRRADIANCE_OUTPUTS;
            const uint  local   = job % outputs;
            const uint  part    = job / outputs;
            const uint  outRes  = isDepth ? depRes : irrRes;
            const bool  sums    = thread < RC_FILTER_JOBS && local < outRes * outRes;
            const vec3  outDirection = RadianceCacheOctDecode( ( vec2( local % outRes, local / outRes ) + 0.5 ) / float( outRes ) * 2.0 - 1.0 );

            for ( uint first = 0u; first < texels; first += RC_FILTER_MAX_TEXELS )
            {
                const uint slice = min( RC_FILTER_MAX_TEXELS, texels - first );
                for ( uint i = thread; i < slice; i += RC_FILTER_THREADS )
                {
                    const uint k   = first + i;
                    s_Radiance[i]  = imageLoad( RadianceCacheRadianceTexture, origin + ivec2( k % res, k / res ) );
                    s_Direction[i] =
                         RadianceCacheOctDecode( ( vec2( k % res, k / res ) + 0.5 ) / float( res ) * 2.0 - 1.0 );
                }
                barrier();
                // This part's contiguous run of the slice (none for a thread that sums nothing).
                const uint runBegin = sums ? ( slice * part ) / RC_FILTER_SPLIT : 0u;
                const uint runEnd   = sums ? ( slice * ( part + 1u ) ) / RC_FILTER_SPLIT : 0u;
                // Warp-uniform: a warp is all depth or all irradiance threads.
                if ( isDepth )
                {
                    for ( uint i = runBegin; i < runEnd; ++i )
                    {
                        const vec3  direction = s_Direction[i];
                        const float l         = dot( outDirection, direction );
                        if ( l <= RC_DISTANCE_LOBE_MIN_COSINE )
                            continue;
                        // l^50 (the lobe's exponent) as l^32 * l^16 * l^2: seven multiplies, not pow's
                        // log2 + exp2 on the special-function units, per texel and output (FPS-RC-DIRTY).
                        const float l2  = l * l;
                        const float l4  = l2 * l2;
                        const float l8  = l4 * l4;
                        const float l16 = l8 * l8;
                        const float w   = l16 * l16 * l16 * l2;
                        const float d   = min( s_Radiance[i].a / maxDistance, 1.0 );
                        momentSum += w * vec2( d, d * d );
                        lobeSum   += w;
                    }
                }
                else
                {
                    for ( uint i = runBegin; i < runEnd; ++i )
                    {
                        const float c = dot( outDirection, s_Direction[i] );
                        if ( c > 0.0 )
                        {
                            irradianceSum += s_Radiance[i].rgb * c;
                            cosineSum     += c;
                        }
                    }
                }
                barrier();
            }
            // The slice loop's last barrier ended every read of the slice arrays: they now hold the parts' sums, each
            // at its thread's index (depth parts in s_Direction, irradiance parts in s_Radiance).
            s_Radiance[thread]  = vec4( irradianceSum, cosineSum );
            s_Direction[thread] = vec3( momentSum, lobeSum );
            barrier();
            // Part 0 of each output adds its parts (the same output's parts are `outputs` threads apart).
            if ( sums && part == 0u )
            {
                if ( isDepth )
                {
                    vec3 mom = s_Direction[thread];
                    for ( uint p = 1u; p < RC_FILTER_SPLIT; ++p )
                        mom += s_Direction[thread + outputs * p];
                    s_Distance[local] = mom.z > 0.0 ? mom.xy / mom.z : vec2( 1.0 );
                }
                else
                {
                    vec4 irr = s_Radiance[thread];
                    for ( uint p = 1u; p < RC_FILTER_SPLIT; ++p )
                        irr += s_Radiance[thread + outputs * p];
                    // (1/pi) * integral of L cos over the hemisphere = sum(L cos) / sum(cos) for evenly spread
                    // directions.
                    s_Irradiance[local] = irr.rgb / max( irr.a, 1e-4 );
                }
            }

            // Radiance borders (its interior is as traced).
            const int finalRes = int( res ) + 2;
            for ( uint i = thread; i < uint( finalRes * finalRes ); i += RC_FILTER_THREADS )
            {
                const ivec2 b = ivec2( i % uint( finalRes ), i / uint( finalRes ) ) - 1;
                const ivec2 s = RadianceCacheOctBorderSource( b, int( res ) );
                if ( s != b )
                    imageStore( RadianceCacheRadianceTexture, origin + b, imageLoad( RadianceCacheRadianceTexture, origin + s ) );
            }
            barrier();

            const ivec2 irrOrigin = RadianceCacheProbeAtlasOrigin( radianceCache, probe, irrRes + 2u ) + 1;
            const int   irrFinal  = int( irrRes ) + 2;
            for ( uint i = thread; i < uint( irrFinal * irrFinal ); i += RC_FILTER_THREADS )
            {
                const ivec2 b = ivec2( i % uint( irrFinal ), i / uint( irrFinal ) ) - 1;
                const ivec2 s = RadianceCacheOctBorderSource( b, int( irrRes ) );
                imageStore( RadianceCacheIrradianceOutTexture, irrOrigin + b, vec4( s_Irradiance[s.x + s.y * int( irrRes )], 1.0 ) );
            }
            const ivec2 depOrigin = RadianceCacheProbeAtlasOrigin( radianceCache, probe, depRes + 2u ) + 1;
            const int   depFinal  = int( depRes ) + 2;
            for ( uint i = thread; i < uint( depFinal * depFinal ); i += RC_FILTER_THREADS )
            {
                const ivec2 b = ivec2( i % uint( depFinal ), i / uint( depFinal ) ) - 1;
                const ivec2 s = RadianceCacheOctBorderSource( b, int( depRes ) );
                imageStore( RadianceCacheDepthOutTexture, depOrigin + b, vec4( s_Distance[s.x + s.y * int( depRes )], 0.0, 0.0 ) );
            }

            if ( thread == 0u )
            {
                const RadianceCacheProbe state = radianceCacheProbes[probe];
                radianceCacheProbes[probe].frames = uvec4( state.frames.x, FrameIndex, state.frames.w, state.frames.w );
                // The traced probe's entry drops its untraced bit - only while that entry still holds this probe:
                // writing an entry that holds another slot (or none) would leave that slot or this one doubled.
                const uint entry = RadianceCacheIndirectionIndex( radianceCache, state.cell.xyz, clipmap );
                if ( ( radianceCacheIndirection[entry] & ~RADIANCE_CACHE_PROBE_UNTRACED ) == probe )
                    radianceCacheIndirection[entry] = probe;
            }
        }
    }
}
