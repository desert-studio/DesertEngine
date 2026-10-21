// DesertAsset {"Kind":"Shader","Guid":"dc22522acd434872887b7c7027351686","Versions":{"SHDR":1},"Dependencies":[]}
// THE IRRADIANCE FIELD GATHER, 1/2 (GIMethod::ProbeVolume; UE IrradianceFieldGatherCS, LumenIrradianceFieldGather.usf):
// one thread per pixel of the HALF-resolution grid — the G-buffer pixel at its even corner (2x, 2y) — the world
// radiance cache read in place, no screen probes in front of it. IrradianceFieldUpsample.shader brings it to the
// G-buffer's extent by depth and normal.
//   diffuse   the cache's irradiance (/ pi) at the pixel's normal: the eight probes around it, trilinear, weighted
//             by their facing and DDGI's Chebyshev visibility (RadianceCacheIrradianceAt), so a probe behind a wall
//             from the pixel does not light it;
//   specular  the same eight probes under the same weights, read about the reflection direction (one lookup per
//             probe for both): the cosine lobe around it, visibility-weighted — the
//             rough reflection this method can give without a reflection tracer (UE writes the irradiance field's
//             diffuse into RoughSpecularIndirect; a lobe about the reflection keeps a glossy floor's highlight on
//             the side of what it reflects).
//   backface  the same read at -N (its own weights: the probes behind the surface): the irradiance a two-sided
//             leaf lets through (the screen probes' backface, ScreenProbeFilter).
// Where no traced probe answers yet (cells allocated this frame) or past the coarsest clipmap the pixel reads black,
// as UE's: the composite then has no bounce there, never the sky through a wall.
// Output (half resolution): diffuse.rgb + .a = the sample's distance to the camera (0: no surface), specular.rgb,
// backface.rgb.
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
        Uniform sampler u_LinearClampSampler;
        Uniform sampler u_PointClampSampler;
        Uniform(4) texture2D u_GBufferDepth;
        #define u_GBufferDepth sampler2D(u_GBufferDepth, u_PointClampSampler)
        Uniform(5) texture2D u_GBufferB;
        #define u_GBufferB sampler2D(u_GBufferB, u_PointClampSampler)
        Uniform(6) texture2D u_RadianceCacheIrradiance;
        #define u_RadianceCacheIrradiance sampler2D(u_RadianceCacheIrradiance, u_LinearClampSampler)
        Uniform(7) texture2D u_RadianceCacheDepth;
        #define u_RadianceCacheDepth sampler2D(u_RadianceCacheDepth, u_LinearClampSampler)
        layout(binding = 8, rgba16f) restrict writeonly uniform image2D u_GIHalfDiffuseOut;
        layout(binding = 9, rgba16f) restrict writeonly uniform image2D u_GIHalfSpecularOut;
        layout(binding = 10, rgba16f) restrict writeonly uniform image2D u_GIHalfBackfaceOut;

        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #include <Common/RadianceCache.glslh>

        void main()
        {
            const ivec2 half_  = ivec2( gl_GlobalInvocationID.xy );
            const ivec2 extent = ivec2( giFrame.screen.xy );
            if ( any( greaterThanEqual( half_, ( extent + 1 ) / 2 ) ) )
                return;
            const ivec2 pixel = min( half_ * 2, extent - 1 );
            const vec4  gb    = texelFetch( u_GBufferB, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( u_GIHalfDiffuseOut, half_, vec4( 0.0 ) );
                imageStore( u_GIHalfSpecularOut, half_, vec4( 0.0 ) );
                imageStore( u_GIHalfBackfaceOut, half_, vec4( 0.0 ) );
                return;
            }
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p        = ReconstructWorldPosition( uv, texelFetch( u_GBufferDepth, pixel, 0 ).r,
                                                             giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const float distance = max( length( giFrame.camera.xyz - p ), 1e-3 );
            const vec3  v        = ( giFrame.camera.xyz - p ) / distance;
            const vec3  r        = reflect( -v, n );
            const float dither   = RadianceCachePixelDither( vec2( pixel ) ); // stable per pixel: no temporal filter follows

            vec3       specular;
            const vec4 diffuse = RadianceCacheIrradianceAtImpl( p, n, v, r, dither, true, specular );
            imageStore( u_GIHalfDiffuseOut, half_, vec4( diffuse.a > 0.0 ? diffuse.rgb : vec3( 0.0 ), distance ) );
            imageStore( u_GIHalfSpecularOut, half_, vec4( diffuse.a > 0.0 ? specular : vec3( 0.0 ), 1.0 ) );
            const vec4 backface = RadianceCacheIrradianceAt( p, -n, v, dither );
            imageStore( u_GIHalfBackfaceOut, half_, vec4( backface.a > 0.0 ? backface.rgb : vec3( 0.0 ), 1.0 ) );
        }
    }
}
