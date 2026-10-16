// DesertAsset {"Kind":"Shader","Guid":"ad572b5fc8784311a2fcc5a2d28adc9a","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 4/4 — INTEGRATE (UE ScreenProbeIntegrateCS + the gather's temporal filter,
// LumenScreenProbeGather.usf): one thread per G-buffer pixel.
//   probes    the four probes around the pixel of every level (uniform 16 px, adaptive 8 and 4 px), each weighted
//             bilinearly x by how well its plane holds the pixel and its hemisphere faces the pixel's normal
//             (CalculateUpsampleInterpolationWeights). Where no probe's plane holds the pixel the uniform four
//             answer by position and normal alone, so a pixel never reads nothing while a probe is near; where
//             no probe is near at all (a lone leaf against the sky) the world radiance cache answers in place;
//   diffuse   the probes' irradiance SH at the pixel's normal (already / pi);
//   specular  UE's rough specular (LumenScreenProbeGather.usf ScreenProbeIntegrateCS, the default
//             ROUGH_SPECULAR_ALONG_REFLECTION_DIRECTION 0): the diffuse irradiance above (UE `DiffuseLighting / PI`)
//             lerped toward four visible-GGX samples of the probes' filtered radiance by RoughReflectionsDiffuseLerp
//             (fully the irradiance from roughness MaxRoughnessToEvaluateRoughSpecular = 0.8 up, fully the GGX
//             samples 0.2 below it). It is NOT the probes' radiance along the one mirror direction, which UE turns
//             off because it "samples values below the visible horizon": a direction under the probe's hemisphere
//             reads its horizon texel, the open sky at a crown's edge, and that was every pale fleck on Bistro's
//             bush and every pale-blue linde leaf (FOLIAGE-LOOK-3: the specular knockout removed both, the reflection
//             environment in its place made them brighter). The rough reflections of UE (ScreenProbeGather
//             integrates the specular of surfaces above r.Lumen.Reflections.MaxRoughnessToTrace from the same
//             probes); smoother surfaces read it too until a reflection tracer exists, so no surface reads the sky
//             cube in a closed room;
//   temporal  blended with the pixel's reprojected history while the history's distance to the camera matches
//             the pixel's (UE r.Lumen.ScreenProbeGather.Temporal, ~10 frames).
// Output: diffuse.rgb + .a = the pixel's distance to the camera (the next frame's history test), specular.rgb.
Shader "ScreenProbeIntegrate"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ScreenProbes.glslh>
        #include <Common/RadianceCache.glslh>
        #include <Common/ReconstructPosition.glslh>

        ReadBuffer(0) ScreenProbeParamsUB
        {
            ScreenProbeParams screenProbeParams;
        };
        ReadBuffer(1) ScreenProbeIndexUB
        {
            uint screenProbeIndex[];
        };
        ReadBuffer(2) ScreenProbesUB
        {
            ScreenProbe screenProbes[];
        };
        ReadBuffer(3) GIFrameUB
        {
            GIFrame giFrame;
        };
        ReadBuffer(4) ScreenProbeSHUB
        {
            vec4 screenProbeSH[];
        };
        Uniform(5) sampler2D u_ScreenProbeFiltered;
        Uniform(6) sampler2D u_GBufferDepth;
        Uniform(7) sampler2D u_GBufferB;
        Uniform(8) sampler2D u_ScreenProbeDiffuseHistory;
        Uniform(9) sampler2D u_ScreenProbeSpecularHistory;
        layout(binding = 10, rgba16f) restrict writeonly uniform image2D u_ScreenProbeDiffuseOut;
        layout(binding = 11, rgba16f) restrict writeonly uniform image2D u_ScreenProbeSpecularOut;
        ReadBuffer(12) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(13) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(14) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform(15) sampler2D u_RadianceCacheIrradiance;
        Uniform(16) sampler2D u_RadianceCacheDepth;
        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

        // The history's share of a reprojected pixel (UE: about ten frames accumulated).
        #define SP_HISTORY_WEIGHT 0.9
        // The relative change of the distance to the camera above which the history is another surface.
        #define SP_HISTORY_DISTANCE_TOLERANCE 0.05

        // UE r.Lumen.ScreenProbeGather.MaxRoughnessToEvaluateRoughSpecular (and ...ForFoliage), both 0.8, and the
        // 0.2 fade of RoughReflectionsDiffuseLerp (LumenScreenProbeTileClassication.ush): at and above 0.8 the rough
        // specular IS the irradiance, below 0.6 it is all GGX samples.
        #define SP_MAX_ROUGHNESS_TO_EVALUATE_ROUGH_SPECULAR 0.8
        #define SP_ROUGH_SPECULAR_FADE_LENGTH 0.2
        // UE NumSpecularSamples, and the floor UE puts under the roughness the probes can answer (a ~2x2 footprint).
        #define SP_ROUGH_SPECULAR_SAMPLES 4
        #define SP_ROUGH_SPECULAR_MIN_ROUGHNESS 0.2

        // UE RoughReflectionsDiffuseLerp: the share of the rough specular the irradiance answers.
        float SPRoughSpecularDiffuseLerp( float roughness )
        {
            return clamp( ( roughness - SP_MAX_ROUGHNESS_TO_EVALUATE_ROUGH_SPECULAR + SP_ROUGH_SPECULAR_FADE_LENGTH ) /
                              SP_ROUGH_SPECULAR_FADE_LENGTH,
                          0.0, 1.0 );
        }

        // Per pixel, sample and frame (PCG, as ScreenProbeTrace's TexelNoise).
        vec2 SPPixelNoise( uvec2 pixel, uint index, uint frame )
        {
            uvec3 h = uvec3( pixel.x, pixel.y * 4u + index, frame ) * 1664525u + 1013904223u;
            h.x += h.y * h.z; h.y += h.z * h.x; h.z += h.x * h.y;
            h ^= h >> 16u;
            h.x += h.y * h.z; h.y += h.z * h.x; h.z += h.x * h.y;
            return vec2( h.xy & 0xFFFFu ) / 65536.0;
        }

        // UE MonteCarlo.ush ImportanceSampleVisibleGGX (Dupuy & Benyoub 2023, spherical caps; isotropic, unbounded):
        // a micro-normal of the GGX lobe of `alpha` seen from tangent-space `v` (z = the shading normal).
        vec3 SPSampleVisibleGGX( vec2 e, float alpha, vec3 v )
        {
            const vec3  vh  = normalize( vec3( alpha * v.xy, v.z ) );
            const float phi = 6.28318530718 * e.x;
            const float z   = mix( 1.0, -vh.z, e.y );
            const float st  = sqrt( clamp( 1.0 - z * z, 0.0, 1.0 ) );
            const vec3  h   = vec3( st * cos( phi ), st * sin( phi ), z ) + vh;
            return normalize( vec3( alpha * h.xy, max( 0.0, h.z ) ) );
        }

        vec3 ProbeIrradiance( uint probe, vec3 n )
        {
            vec3 e = vec3( 0.0 );
            for ( uint k = 0u; k < 9u; ++k )
                e += screenProbeSH[probe * 9u + k].rgb * SPSHBasis( k, n );
            return max( e, vec3( 0.0 ) );
        }

        vec3 ProbeRadiance( uint probe, vec3 direction )
        {
            const ScreenProbe state = screenProbes[probe];
            const vec2 o      = SPProbeOct( SPProbeFrame( state.normal.xyz ), direction );
            const vec2 texel  = vec2( SPAtlasOrigin( probe, SP_TILE ) + 1 ) + ( o * 0.5 + 0.5 ) * float( SP_RES );
            return textureLod( u_ScreenProbeFiltered, texel / vec2( textureSize( u_ScreenProbeFiltered, 0 ) ), 0.0 ).rgb;
        }

        void main()
        {
            const ivec2 pixel  = ivec2( gl_GlobalInvocationID.xy );
            const vec2  extent = giFrame.screen.xy;
            if ( any( greaterThanEqual( pixel, ivec2( extent ) ) ) )
                return;
            const vec4 gb = texelFetch( u_GBufferB, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( u_ScreenProbeDiffuseOut, pixel, vec4( 0.0 ) );
                imageStore( u_ScreenProbeSpecularOut, pixel, vec4( 0.0 ) );
                return;
            }
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p        = ReconstructWorldPosition( uv, texelFetch( u_GBufferDepth, pixel, 0 ).r,
                                                             giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const float distance = max( length( p - giFrame.camera.xyz ), 1e-3 );
            const vec3  v        = ( giFrame.camera.xyz - p ) / distance;
            const vec3  r        = reflect( -v, n );

            // UE's rough specular samples (ScreenProbeIntegrateCS, Rough-Specular): four visible-GGX reflections of
            // the view about the shading normal. A shading normal turned from the eye (a normal-mapped or two-sided
            // leaf seen edge-on) samples with |N.V|, UE's BxDF convention (BxDFContext NoV = saturate(abs(NoV))); a
            // sample that leaves below the surface carries nothing, as the GGX integrand's N.L does.
            const float roughness   = max( gb.a, 0.04 );
            const float diffuseLerp = SPRoughSpecularDiffuseLerp( roughness );
            vec3        roughL[SP_ROUGH_SPECULAR_SAMPLES];
            float       roughW[SP_ROUGH_SPECULAR_SAMPLES];
            {
                const mat3  frame = SPProbeFrame( n );
                vec3        vt    = transpose( frame ) * v;
                vt.z              = max( abs( vt.z ), 1e-4 );
                const float alpha = max( roughness, SP_ROUGH_SPECULAR_MIN_ROUGHNESS ) *
                                    max( roughness, SP_ROUGH_SPECULAR_MIN_ROUGHNESS );
                // UE BiasBSDFImportantSample: the second coordinate squeezed 10% toward the middle of its range.
                const vec2 jitter = SPPixelNoise( uvec2( pixel ), 0u, screenProbeParams.atlas.y );
                for ( int k = 0; k < SP_ROUGH_SPECULAR_SAMPLES; ++k )
                {
                    vec2 e = vec2( fract( jitter.x + float( k ) / float( SP_ROUGH_SPECULAR_SAMPLES ) ),
                                   SPPixelNoise( uvec2( pixel ), uint( k ) + 1u, screenProbeParams.atlas.y ).y );
                    e.y           = ( e.y - 0.5 ) * 0.9 + 0.5;
                    const vec3 h  = SPSampleVisibleGGX( e, alpha, vt );
                    const vec3 lt = reflect( -vt, h );
                    roughL[k]     = frame * lt;
                    roughW[k]     = ( diffuseLerp < 1.0 && lt.z > 0.0 ) ? 1.0 / float( SP_ROUGH_SPECULAR_SAMPLES ) : 0.0;
                }
            }

            vec3  diffuse  = vec3( 0.0 );
            vec3  specular = vec3( 0.0 );
            float total    = 0.0;
            // The probes that hold the pixel and their weights: the diffuse reads every one, each GGX sample one of
            // them picked by weight (UE r.Lumen.ScreenProbeGather.StochasticInterpolation) - four radiance fetches a
            // pixel instead of four per probe (Bistro's backlit crown: 7.1 ms of integrate, then a GPU watchdog).
            uint  heldId[SP_LEVELS * 4u];
            float heldW[SP_LEVELS * 4u];
            uint  held = 0u;
            for ( uint level = 0u; level < SP_LEVELS; ++level )
            {
                uvec4 ids;
                vec4  weights;
                SPLevelWeights( level, vec2( pixel ) + 0.5, p, n, distance, extent, ids, weights );
                for ( int i = 0; i < 4; ++i )
                    if ( weights[i] > 1e-4 )
                    {
                        diffuse  += weights[i] * ProbeIrradiance( ids[i], n );
                        heldId[held] = ids[i];
                        heldW[held]  = weights[i];
                        ++held;
                        total    += weights[i];
                    }
            }
            if ( total <= 1e-4 )
            {
                // No probe's plane holds the pixel: the uniform four by position and normal alone.
                const vec2  c = ( vec2( pixel ) + 0.5 ) / SPSpacing( 0u ) - 0.5;
                const ivec2 b = ivec2( floor( c ) );
                const vec2  f = c - vec2( b );
                for ( int i = 0; i < 4; ++i )
                {
                    const ivec2 o  = ivec2( i & 1, i >> 1 );
                    const uint  id = SPProbeAt( 0u, b + o, extent );
                    if ( id == SP_INVALID )
                        continue;
                    const vec2  bilinear = mix( 1.0 - f, f, vec2( o ) );
                    const float w = bilinear.x * bilinear.y * ( max( dot( n, screenProbes[id].normal.xyz ), 0.0 ) + 0.05 );
                    diffuse  += w * ProbeIrradiance( id, n );
                    heldId[held] = id;
                    heldW[held]  = w;
                    ++held;
                    total    += w;
                }
            }
            if ( total > 1e-6 )
            {
                diffuse /= total;
                for ( int k = 0; k < SP_ROUGH_SPECULAR_SAMPLES; ++k )
                {
                    if ( roughW[k] <= 0.0 )
                        continue;
                    float pick = SPPixelNoise( uvec2( pixel ), uint( k ) + 8u, screenProbeParams.atlas.y ).x * total;
                    uint  j    = 0u;
                    while ( j + 1u < held && pick >= heldW[j] )
                    {
                        pick -= heldW[j];
                        ++j;
                    }
                    specular += roughW[k] * ProbeRadiance( heldId[j], roughL[k] );
                }
                specular = mix( specular, diffuse, diffuseLerp );
            }
            else
            {
                // NO SCREEN PROBE HOLDS THE PIXEL, NOT EVEN BY POSITION: every probe of its tiles sits on the sky (a
                // leaf card or a twig at a crown's edge, alone against the sky, under every level's placement). It
                // reads the world radiance cache in place, as the ProbeVolume method does for every pixel
                // (IrradianceFieldGather) - the same cache the probes' own rays end in - and is never black: before
                // this, Bistro's linde tree drew every such leaf as a black cut-out (FOLIAGE-LOOK-2 x1_uncovered).
                vec3       cacheSpecular;
                const vec4 cache = RadianceCacheIrradianceAtImpl( p, n, v, r, 0.5, true, cacheSpecular );
                if ( cache.a > 0.0 )
                {
                    // The cache answers the mirror direction only: UE's lerp toward the irradiance holds here too.
                    diffuse  = cache.rgb;
                    specular = mix( cacheSpecular, cache.rgb, diffuseLerp );
                }
            }

            // ---- Temporal: the pixel's surface in last frame's view.
            if ( screenProbeParams.atlas.z != 0u )
            {
                const vec4 clip = screenProbeParams.prevViewProjection * vec4( p, 1.0 );
                if ( clip.w > 0.0 )
                {
                    const vec2 ndc    = clip.xy / clip.w;
                    const vec2 prevUV = vec2( ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5 );
                    if ( all( greaterThanEqual( prevUV, vec2( 0.0 ) ) ) && all( lessThanEqual( prevUV, vec2( 1.0 ) ) ) )
                    {
                        const vec4  history  = textureLod( u_ScreenProbeDiffuseHistory, prevUV, 0.0 );
                        const float expected = length( p - screenProbeParams.prevCamera.xyz );
                        if ( history.a > 0.0 && abs( history.a - expected ) < SP_HISTORY_DISTANCE_TOLERANCE * expected )
                        {
                            diffuse  = mix( diffuse, history.rgb, SP_HISTORY_WEIGHT );
                            specular = mix( specular, textureLod( u_ScreenProbeSpecularHistory, prevUV, 0.0 ).rgb,
                                            SP_HISTORY_WEIGHT );
                        }
                    }
                }
            }
            imageStore( u_ScreenProbeDiffuseOut, pixel, vec4( diffuse, distance ) );
            imageStore( u_ScreenProbeSpecularOut, pixel, vec4( specular, 1.0 ) );
        }
    }
}
