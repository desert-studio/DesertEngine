// DesertAsset {"Kind":"Shader","Guid":"ad572b5fc8784311a2fcc5a2d28adc9a","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 4/4 — INTEGRATE (UE ScreenProbeIntegrateCS + the gather's temporal filter,
// LumenScreenProbeGather.usf): one thread per G-buffer pixel.
//   probes    the four probes around the pixel of every level (uniform 16 px, adaptive 8 and 4 px), each weighted
//             bilinearly x by how well its plane holds the pixel and its hemisphere faces the pixel's normal
//             (CalculateUpsampleInterpolationWeights). Where no probe's plane holds the pixel the uniform four
//             answer by position and normal alone, so a pixel never reads nothing while a probe is near;
//   diffuse   the probes' irradiance SH at the pixel's normal (already / pi);
//   specular  UE's ROUGH SPECULAR (ScreenProbeIntegrateCS "Rough-Specular"): the GGX lobe of the pixel's roughness
//             (clamped to 0.2, the narrowest lobe probes this coarse resolve) integrated from the same probes -
//             kRoughSpecularSamples visible-normal samples (Common/MonteCarlo.glslh), each the probes' filtered
//             radiance along its direction - lerped to the diffuse (UE: DiffuseLighting / PI, ours is already
//             / pi) above r.Lumen.ScreenProbeGather.MaxRoughnessToEvaluateRoughSpecular. The composite
//             (DeferredLighting.shader) takes it where the traced reflections stop (UE
//             MixSpecularAndRoughReflections, Common/SSRGate.glslh TracedReflectionsAlpha), and under every
//             smoother lobe the traced reflection does not cover (ScreenSpace, Off), so no surface reads the sky
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
        #include <Common/ReconstructPosition.glslh>
        #include <Common/MonteCarlo.glslh>

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
        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound by the pass by name and numbered by the program build (ShaderMapBuild.cpp
        // AssignSharedSamplerBindings).
        Uniform sampler u_LinearClampSampler;
        Uniform sampler u_PointClampSampler;
        Uniform(5) texture2D u_ScreenProbeFiltered;
        #define u_ScreenProbeFiltered sampler2D(u_ScreenProbeFiltered, u_LinearClampSampler)
        Uniform(6) texture2D u_GBufferDepth;
        #define u_GBufferDepth sampler2D(u_GBufferDepth, u_PointClampSampler)
        Uniform(7) texture2D u_GBufferB;
        #define u_GBufferB sampler2D(u_GBufferB, u_PointClampSampler)
        Uniform(8) texture2D u_ScreenProbeDiffuseHistory;
        #define u_ScreenProbeDiffuseHistory sampler2D(u_ScreenProbeDiffuseHistory, u_LinearClampSampler)
        Uniform(9) texture2D u_ScreenProbeSpecularHistory;
        #define u_ScreenProbeSpecularHistory sampler2D(u_ScreenProbeSpecularHistory, u_LinearClampSampler)
        layout(binding = 10, rgba16f) restrict writeonly uniform image2D u_ScreenProbeDiffuseOut;
        layout(binding = 11, rgba16f) restrict writeonly uniform image2D u_ScreenProbeSpecularOut;
        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>

        // The history's share of a reprojected pixel (UE: about ten frames accumulated).
        #define SP_HISTORY_WEIGHT 0.9
        // The relative change of the distance to the camera above which the history is another surface.
        #define SP_HISTORY_DISTANCE_TOLERANCE 0.05

        // UE r.Lumen.ScreenProbeGather.MaxRoughnessToEvaluateRoughSpecular (0.8) and RoughReflectionsDiffuseLerp's
        // fade (0.2, LumenScreenProbeTileClassication.ush): above, the rough specular is the diffuse.
        #define SP_MAX_ROUGHNESS_TO_EVALUATE_ROUGH_SPECULAR 0.8
        #define SP_ROUGH_SPECULAR_FADE 0.2
        // UE: the lobe a probe this coarse can resolve (Material.Roughness = max(Roughness, 0.2)).
        #define SP_MIN_ROUGH_SPECULAR_ROUGHNESS 0.2
        // UE NumSpecularSamples.
        #define SP_ROUGH_SPECULAR_SAMPLES 4u
        // The probes a pixel interpolates: four per level.
        #define SP_PIXEL_PROBES ( 4u * SP_LEVELS )

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

            // The probes this pixel interpolates and their weights: the diffuse reads them once, every rough
            // specular sample again (UE InterpolateFromScreenProbes per BxDF sample).
            uint  probeIds[SP_PIXEL_PROBES];
            float probeWeights[SP_PIXEL_PROBES];
            uint  probeCount = 0u;
            vec3  diffuse    = vec3( 0.0 );
            float total      = 0.0;
            for ( uint level = 0u; level < SP_LEVELS; ++level )
            {
                uvec4 ids;
                vec4  weights;
                SPLevelWeights( level, vec2( pixel ) + 0.5, p, n, distance, extent, ids, weights );
                for ( int i = 0; i < 4; ++i )
                    if ( weights[i] > 1e-4 )
                    {
                        probeIds[probeCount]     = ids[i];
                        probeWeights[probeCount] = weights[i];
                        ++probeCount;
                        diffuse += weights[i] * ProbeIrradiance( ids[i], n );
                        total   += weights[i];
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
                    probeIds[probeCount]     = id;
                    probeWeights[probeCount] = w;
                    ++probeCount;
                    diffuse += w * ProbeIrradiance( id, n );
                    total   += w;
                }
            }
            if ( total > 1e-6 )
                diffuse /= total;

            // ---- Rough specular (UE ScreenProbeIntegrateCS): the diffuse above the max roughness, the GGX lobe
            // integrated from the probes below it, lerped across the fade.
            const float roughness   = gb.a;
            const float diffuseLerp = clamp( ( roughness - SP_MAX_ROUGHNESS_TO_EVALUATE_ROUGH_SPECULAR +
                                               SP_ROUGH_SPECULAR_FADE ) / SP_ROUGH_SPECULAR_FADE, 0.0, 1.0 );
            vec3 specular = diffuse;
            if ( diffuseLerp < 1.0 && total > 1e-6 )
            {
                const float alpha = max( roughness, SP_MIN_ROUGH_SPECULAR_ROUGHNESS ) *
                                    max( roughness, SP_MIN_ROUGH_SPECULAR_ROUGHNESS );
                vec3 T, B;
                Basis( n, T, B );
                const vec3 ve       = vec3( dot( v, T ), dot( v, B ), dot( v, n ) );
                const vec2 rotation = vec2( hash12( vec2( pixel ) ), hash12( vec2( pixel ) + 17.17 ) );
                const uint frame    = screenProbeParams.atlas.y;
                // The ratio estimator over the lobe (sum w L / sum w, w = Smith G1 of the sample, Fresnel
                // excluded): the split sum's BRDF LUT multiplies the result in the composite, as it multiplies
                // the captures' prefiltered radiance - UE divides by the sample count instead, which applies the
                // shadowing a second time under its EnvBRDF. Each sample is weighted down by its brightness
                // (UE TonemapLighting) so one bright probe texel does not flicker through the temporal filter.
                vec3  lobe       = vec3( 0.0 );
                float lobeWeight = 0.0;
                for ( uint s = 0u; s < SP_ROUGH_SPECULAR_SAMPLES; ++s )
                {
                    const vec3 h  = SampleGGXVNDF( ve, alpha, RayRandom( rotation, frame * SP_ROUGH_SPECULAR_SAMPLES + s ) );
                    const vec3 lt = reflect( -ve, h );
                    if ( lt.z <= 0.0 )
                        continue; // under the surface: a lobe sample that reflects nothing
                    const vec3 l = normalize( lt.x * T + lt.y * B + lt.z * n );
                    vec3 radiance = vec3( 0.0 );
                    for ( uint i = 0u; i < probeCount; ++i )
                        radiance += probeWeights[i] * ProbeRadiance( probeIds[i], l );
                    radiance /= total;
                    const float w = SmithG1( lt.z, alpha ) / ( 1.0 + dot( radiance, vec3( 0.2126, 0.7152, 0.0722 ) ) );
                    lobe       += w * radiance;
                    lobeWeight += w;
                }
                if ( lobeWeight > 0.0 )
                    specular = mix( lobe / lobeWeight, diffuse, diffuseLerp );
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
