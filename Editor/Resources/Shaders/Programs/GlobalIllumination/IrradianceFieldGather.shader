// DesertAsset {"Kind":"Shader","Guid":"dc22522acd434872887b7c7027351686","Versions":{"SHDR":1},"Dependencies":[]}
// THE IRRADIANCE FIELD GATHER (GIMethod::ProbeVolume; UE IrradianceFieldGatherCS, LumenIrradianceFieldGather.usf):
// one thread per G-buffer pixel, the world radiance cache read in place — no screen probes in front of it.
//   diffuse   the cache's irradiance (/ pi) at the pixel's normal: the eight probes around it, trilinear, weighted
//             by their facing and DDGI's Chebyshev visibility (RadianceCacheIrradianceAt), so a probe behind a wall
//             from the pixel does not light it;
//   specular  the same read about the reflection direction: the cosine lobe around it, visibility-weighted — the
//             rough reflection this method can give without a reflection tracer (UE writes the irradiance field's
//             diffuse into RoughSpecularIndirect; a lobe about the reflection keeps a glossy floor's highlight on
//             the side of what it reflects).
// Where no traced probe answers yet (cells allocated this frame) or past the coarsest clipmap the pixel reads black,
// as UE's: the composite then has no bounce there, never the sky through a wall.
// Output: diffuse.rgb + .a = 1, specular.rgb + .a = 1 (ViewTargetFormats::kGIFinalGather).
Shader "IrradianceFieldGather"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/RadianceCache.glslh>
        #include <Common/ReconstructPosition.glslh>

        ReadBuffer(0) GIFrameUB
        {
            GIFrame giFrame;
        };
        ReadBuffer(1) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(2) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(3) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };
        Uniform(4) sampler2D u_GBufferDepth;
        Uniform(5) sampler2D u_GBufferB;
        Uniform(6) sampler2D u_RadianceCacheIrradiance;
        Uniform(7) sampler2D u_RadianceCacheDepth;
        layout(binding = 8, rgba16f) restrict writeonly uniform image2D u_GIDiffuseOut;
        layout(binding = 9, rgba16f) restrict writeonly uniform image2D u_GISpecularOut;

        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

        // The clipmap pick's dither, stable per pixel (no temporal filter follows this pass): interleaved gradient
        // noise (Jimenez 2014), the pattern UE's InterleavedGradientNoise uses for the same pick.
        float PixelDither( vec2 pixel )
        {
            return fract( 52.9829189 * fract( dot( pixel, vec2( 0.06711056, 0.00583715 ) ) ) );
        }

        void main()
        {
            const ivec2 pixel = ivec2( gl_GlobalInvocationID.xy );
            if ( any( greaterThanEqual( pixel, ivec2( giFrame.screen.xy ) ) ) )
                return;
            const vec4 gb = texelFetch( u_GBufferB, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( u_GIDiffuseOut, pixel, vec4( 0.0 ) );
                imageStore( u_GISpecularOut, pixel, vec4( 0.0 ) );
                return;
            }
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p        = ReconstructWorldPosition( uv, texelFetch( u_GBufferDepth, pixel, 0 ).r,
                                                             giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const float distance = max( length( giFrame.camera.xyz - p ), 1e-3 );
            const vec3  v        = ( giFrame.camera.xyz - p ) / distance;
            const vec3  r        = reflect( -v, n );
            const float dither   = PixelDither( vec2( pixel ) );

            const vec4 diffuse  = RadianceCacheIrradianceAt( p, n, v, dither );
            const vec4 specular = RadianceCacheIrradianceAt( p, r, v, dither );
            imageStore( u_GIDiffuseOut, pixel, vec4( diffuse.a > 0.0 ? diffuse.rgb : vec3( 0.0 ), 1.0 ) );
            imageStore( u_GISpecularOut, pixel, vec4( specular.a > 0.0 ? specular.rgb : vec3( 0.0 ), 1.0 ) );
        }
    }
}
