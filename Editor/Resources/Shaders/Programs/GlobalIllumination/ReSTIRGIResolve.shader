// DesertAsset {"Kind":"Shader","Guid":"7758287f36be45e594d6ee1b7460a3ad","Versions":{"SHDR":1},"Dependencies":[]}
// THE RESTIR GI FINAL GATHER, 4/4 — THE RESOLVE (UE 5.4+ LumenReSTIRGather.usf UpsampleAndIntegrateCS): every
// G-buffer pixel from the 2x2 spatial reservoirs around it (bilinear, times a bilateral weight: the same plane
// within ReSTIRParams.tuning.z and the normal), each reservoir's sample evaluated at THIS pixel's point and normal,
// into the three textures the screen probes' integrate writes (ScreenProbeIntegrate.shader), which the same
// temporal filter then takes:
//   Diffuse   rgb = irradiance / pi (sum radiance x cos x W over the taps' weights), a = distance to the camera
//   Specular  rgb = the rough specular: a GGX ratio estimator over the same samples (sum D cos W L / sum D cos W),
//             blended toward the radiance cache's specular about the reflection by how much of the lobe the samples
//             cover, then toward the diffuse by UE RoughReflectionsDiffuseLerp; a = 0 (no moving-light history
//             boost: the reservoirs' temporal reuse is the history)
//   Backface  rgb = the radiance cache's irradiance / pi from -N where the shading model has UE's backface diffuse
// Rays start at each pixel's own surface, so the contact occlusion is in the signal (the gather reports
// ShortRangeAO: the composite applies no SSAO). A tap-less pixel (every neighbour rejected) reads the cache.
Shader "ReSTIRGIResolve"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/RadianceCache.glslh>
        #include <Common/ReconstructPosition.glslh>
        #include <Common/ReSTIRGI.glslh>

        ReadBuffer(0) ReSTIRParamsUB
        {
            ReSTIRParams restirParams;
        };
        ReadBuffer(1) GIFrameUB
        {
            GIFrame giFrame;
        };
        ReadBuffer(2) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(3) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(4) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform sampler PointClampSampler;
        Uniform sampler LinearClampSampler;
        Uniform(5) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(6) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        Uniform(7) utexture2D GBufferShadingWord;
        #define GBufferShadingWord usampler2D(GBufferShadingWord, PointClampSampler)
        Uniform(8) texture2D SpatialHit;
        #define SpatialHit sampler2D(SpatialHit, PointClampSampler)
        Uniform(9) texture2D SpatialRadiance;
        #define SpatialRadiance sampler2D(SpatialRadiance, PointClampSampler)
        Uniform(10) texture2D SpatialState;
        #define SpatialState sampler2D(SpatialState, PointClampSampler)
        Uniform(11) texture2D RadianceCacheIrradianceTexture;
        #define RadianceCacheIrradianceTexture sampler2D(RadianceCacheIrradianceTexture, LinearClampSampler)
        Uniform(12) texture2D RadianceCacheDepthTexture;
        #define RadianceCacheDepthTexture sampler2D(RadianceCacheDepthTexture, LinearClampSampler)
        layout(binding = 13, rgba16f) restrict writeonly uniform image2D ReSTIRDiffuseOut;
        layout(binding = 14, rgba16f) restrict writeonly uniform image2D ReSTIRSpecularOut;
        layout(binding = 15, rgba16f) restrict writeonly uniform image2D ReSTIRBackfaceOut;
        #include <Generated/ShadingModels.glslh>

        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

        // UE RoughReflectionsDiffuseLerp, as ScreenProbeIntegrate.shader SPRoughSpecularDiffuseLerp (the same UE
        // defaults: MaxRoughnessToEvaluateRoughSpecular 0.8, fade 0.2).
        #define RESTIR_MAX_ROUGHNESS_TO_EVALUATE_ROUGH_SPECULAR 0.8
        #define RESTIR_ROUGH_SPECULAR_FADE_LENGTH 0.2
        float RESTIRRoughSpecularDiffuseLerp( float roughness )
        {
            const float start = RESTIR_MAX_ROUGHNESS_TO_EVALUATE_ROUGH_SPECULAR - RESTIR_ROUGH_SPECULAR_FADE_LENGTH;
            return clamp( ( roughness - start ) / RESTIR_ROUGH_SPECULAR_FADE_LENGTH, 0.0, 1.0 );
        }

        void main()
        {
            const ivec2 pixel  = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 screen = ivec2( giFrame.screen.xy );
            if ( any( greaterThanEqual( pixel, screen ) ) )
                return;
            const vec4 gb = texelFetch( GBufferBTexture, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( ReSTIRDiffuseOut, pixel, vec4( 0.0 ) );
                imageStore( ReSTIRSpecularOut, pixel, vec4( 0.0 ) );
                imageStore( ReSTIRBackfaceOut, pixel, vec4( 0.0 ) );
                return;
            }
            const vec3  n         = normalize( gb.xyz );
            const vec2  uv        = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p         = ReconstructWorldPosition( uv, texelFetch( GBufferDepthTexture, pixel, 0 ).r,
                                                              giFrame.invViewProjection );
            const float distance  = max( length( p - giFrame.camera.xyz ), 1e-3 );
            const vec3  v         = ( giFrame.camera.xyz - p ) / distance;
            const vec3  reflected = reflect( -v, n );
            const float roughness = max( gb.a, 0.04 );
            const float diffuseLerp = RESTIRRoughSpecularDiffuseLerp( roughness );
            const float lobePeak    = RESTIRGGX( 1.0, roughness );

            // The reservoir texels around the pixel (the reservoirs stand for their texels' centres).
            const ivec2 extent = ivec2( restirParams.extent.xy );
            const vec2  at     = ( vec2( pixel ) + 0.5 ) / float( RESTIR_DOWNSAMPLE ) - 0.5;
            const ivec2 base   = ivec2( floor( at ) );
            const vec2  f      = at - vec2( base );
            vec3  irradiance   = vec3( 0.0 );
            vec3  specularSum  = vec3( 0.0 );
            float specularNorm = 0.0;
            float coverage     = 0.0;
            float weightSum    = 0.0;
            for ( int i = 0; i < 4; ++i )
            {
                const ivec2 offset = ivec2( i & 1, i >> 1 );
                const ivec2 t      = clamp( base + offset, ivec2( 0 ), extent - 1 );
                const vec4  state  = texelFetch( SpatialState, t, 0 );
                if ( state.y <= 0.0 || state.x <= 0.0 )
                    continue;
                const vec3 tapNormal = RESTIRUnpackNormal( floatBitsToUint( state.w ) );
                if ( !RESTIRSimilarSurface( distance, n, state.z, tapNormal, restirParams.tuning.z, 0.0 ) )
                    continue;
                const float bilinear = ( offset.x == 1 ? f.x : 1.0 - f.x ) * ( offset.y == 1 ? f.y : 1.0 - f.y );
                const float weight   = max( bilinear, 1e-3 ) * pow( max( dot( n, tapNormal ), 0.0 ), 8.0 );
                if ( weight <= 0.0 )
                    continue;
                const ReSTIRReservoir r =
                     RESTIRLoad( texelFetch( SpatialHit, t, 0 ), texelFetch( SpatialRadiance, t, 0 ), state );
                const vec3  dir       = RESTIRSampleDirection( r, p );
                const float cosine    = max( dot( n, dir ), 0.0 );
                const vec3  integrand = r.radiance * ( cosine * r.W );
                irradiance += weight * integrand;
                weightSum += weight;
                const float lobe = RESTIRGGX( clamp( dot( n, normalize( v + dir ) ), 0.0, 1.0 ), roughness );
                specularSum += weight * lobe * integrand;
                specularNorm += weight * lobe * cosine * r.W;
                coverage += weight * lobe / max( lobePeak, 1e-6 );
            }

            vec3       cacheSpecular;
            const vec4 cache = RadianceCacheIrradianceAtImpl( p, n, v, reflected, 0.5, diffuseLerp < 1.0, cacheSpecular );
            vec3 diffuse = weightSum > 0.0 ? irradiance / ( weightSum * RESTIR_PI ) : ( cache.a > 0.0 ? cache.rgb : vec3( 0.0 ) );
            if ( cache.a <= 0.0 )
                cacheSpecular = diffuse;
            const vec3  traced      = specularNorm > 0.0 ? specularSum / specularNorm : cacheSpecular;
            const float lobeCovered = weightSum > 0.0 ? clamp( coverage / weightSum, 0.0, 1.0 ) : 0.0;
            const vec3  specular    = mix( mix( cacheSpecular, traced, lobeCovered ), diffuse, diffuseLerp );

            vec3 backface = vec3( 0.0 );
            if ( DesertHasBackfaceDiffuse( DesertShadingModelIndex( texelFetch( GBufferShadingWord, pixel, 0 ).rg ) ) )
            {
                const vec4 back = RadianceCacheIrradianceAt( p, -n, v, 0.5 );
                if ( back.a > 0.0 )
                    backface = back.rgb;
            }
            imageStore( ReSTIRDiffuseOut, pixel, vec4( diffuse, distance ) );
            imageStore( ReSTIRSpecularOut, pixel, vec4( specular, 0.0 ) );
            imageStore( ReSTIRBackfaceOut, pixel, vec4( backface, 1.0 ) );
        }
    }
}
