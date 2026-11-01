// DesertAsset {"Kind":"Shader","Guid":"caf401dbfb1d48ae9af1b59df379273d","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER — SHORT-RANGE AO (UE LumenScreenSpaceBentNormal.usf ScreenSpaceShortRangeAOCS, the pass
// r.Lumen.ScreenProbeGather.ShortRangeAO adds in front of the integrate): one thread per G-buffer pixel.
//   rays      SR_AO_RAYS cosine-distributed directions around the pixel's normal, rotated per pixel and frame;
//   trace     each one marched through the scene depth in SR_AO_STEPS linear steps, out to SR_AO_MAX_SCREEN_FRACTION
//             of the screen (UE r.Lumen.ScreenProbeGather.ShortRangeAO.MaxScreenTraceFraction): the occluders
//             between the pixel and the probes, which a probe one 16-pixel tile away cannot see. A step is occluded
//             when it passes behind the depth buffer AND the surface it passes behind lies within the ray's reach of
//             the shaded point (UE's foreground sample rejection: a surface nearer the camera than the AO radius is
//             IN FRONT of the ray, not around it — a cypress a metre before a wall is not a contact occluder of the
//             wall, and counting it drew a speckled dark ring along every crown, BISTRO-FOLIAGE-HALO);
//   output    xyz = the bent normal (the mean of the unoccluded directions, normalised; the normal when none is),
//             w = the unoccluded share (cosine-weighted by the sampling).
// The integrate reads it as UE's ScreenProbeIntegrateCS does with SHORT_RANGE_AO: the diffuse irradiance is the
// probes' SH at the bent normal times the AO, and every rough-specular sample is weighted by its visibility in the
// bent cone (UE CalculateSpecularOcclusion). This is what occludes thin leaf cards: the scalar SSAO samples a
// sphere of points around the pixel and finds the card's own plane open on both sides (FOLIAGE-LOOK-3 b4).
Shader "ScreenProbeShortRangeAO"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ReconstructPosition.glslh>
        #include <Common/ScreenProbes.glslh>

        ReadBuffer(0) ScreenProbeParamsUB
        {
            ScreenProbeParams screenProbeParams;
        };
        ReadBuffer(1) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform sampler PointClampSampler;
        Uniform(2) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(3) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        layout(binding = 4, rgba16f) restrict writeonly uniform image2D ShortRangeAoOutTexture;

        // UE r.Lumen.ScreenProbeGather.ShortRangeAO: 4 rays a pixel, the temporal filter (here the integrate's
        // history) gathers the rest.
        #define SR_AO_RAYS 4
        #define SR_AO_STEPS 8
        // UE r.Lumen.ScreenProbeGather.ShortRangeAO.MaxScreenTraceFraction.
        #define SR_AO_MAX_SCREEN_FRACTION 0.02
        // The ray leaves the surface by this share of its distance to the camera (the depth buffer's own precision
        // and the normal map's tilt off the geometric plane), and passes behind a surface only past it.
        #define SR_AO_RELATIVE_BIAS 0.002

        // Per pixel, ray and frame (PCG, as the integrate's SPPixelNoise).
        vec2 AONoise( uvec2 pixel, uint index, uint frame )
        {
            uvec3 h = uvec3( pixel.x, pixel.y * 8u + index, frame ) * 1664525u + 1013904223u;
            h.x += h.y * h.z; h.y += h.z * h.x; h.z += h.x * h.y;
            h ^= h >> 16u;
            h.x += h.y * h.z; h.y += h.z * h.x; h.z += h.x * h.y;
            return vec2( h.xy & 0xFFFFu ) / 65536.0;
        }

        // The screen texture coordinate of a world point (Common/ScreenUV.glslh's convention: (0,0) top-left).
        vec3 ProjectToScreen( vec3 world )
        {
            const vec4 clip = giFrame.viewProjection * vec4( world, 1.0 );
            const vec2 ndc  = clip.xy / max( clip.w, 1e-6 );
            return vec3( ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, clip.w );
        }

        void main()
        {
            const ivec2 pixel  = ivec2( gl_GlobalInvocationID.xy );
            const vec2  extent = giFrame.screen.xy;
            if ( any( greaterThanEqual( pixel, ivec2( extent ) ) ) )
                return;
            const vec4 gb = texelFetch( GBufferBTexture, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( ShortRangeAoOutTexture, pixel, vec4( 0.0, 0.0, 1.0, 1.0 ) );
                return;
            }
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p        = ReconstructWorldPosition( uv, texelFetch( GBufferDepthTexture, pixel, 0 ).r,
                                                             giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const vec3  camera   = giFrame.camera.xyz;
            const float distance = max( length( p - camera ), 1e-3 );
            const float bias     = SR_AO_RELATIVE_BIAS * distance;
            const vec3  origin   = p + n * bias;
            const mat3  frame    = SPProbeFrame( n );
            const uint  frameIdx = screenProbeParams.atlas.y;
            const vec2  rotation = AONoise( uvec2( pixel ), 0u, frameIdx );

            // The world length of SR_AO_MAX_SCREEN_FRACTION of the screen's width at the pixel's distance: one probe
            // of a known world length projected, then scaled.
            const vec3  uvP      = ProjectToScreen( origin );
            const float probeLen = 0.05 * distance;

            vec3  unoccludedSum = vec3( 0.0 );
            float unoccluded    = 0.0;
            for ( int k = 0; k < SR_AO_RAYS; ++k )
            {
                // Cosine-distributed (Malley), stratified over the rays and rotated per pixel and frame.
                const vec2  e   = vec2( fract( rotation.x + ( float( k ) + AONoise( uvec2( pixel ), uint( k ) + 1u,
                                                                                       frameIdx ).y ) /
                                                                float( SR_AO_RAYS ) ),
                                        AONoise( uvec2( pixel ), uint( k ) + 1u, frameIdx ).x );
                const float phi = 6.28318530718 * fract( e.x + rotation.y );
                const float r   = sqrt( e.y );
                const vec3  lt  = vec3( r * cos( phi ), r * sin( phi ), sqrt( max( 0.0, 1.0 - e.y ) ) );
                const vec3  l   = frame * lt;

                const vec3  uvQ        = ProjectToScreen( origin + l * probeLen );
                const float screenLen  = length( ( uvQ.xy - uvP.xy ) * vec2( 1.0, extent.y * giFrame.screen.z ) );
                const float rayLength  = min( probeLen * SR_AO_MAX_SCREEN_FRACTION / max( screenLen, 1e-5 ),
                                              0.5 * distance );
                const float stepJitter = AONoise( uvec2( pixel ), uint( k ) + 5u, frameIdx ).y;

                bool occluded = false;
                for ( int s = 0; s < SR_AO_STEPS && !occluded; ++s )
                {
                    const vec3 q   = origin + l * ( rayLength * ( float( s ) + stepJitter ) / float( SR_AO_STEPS ) );
                    const vec3 uvS = ProjectToScreen( q );
                    if ( uvS.z <= 0.0 || any( lessThan( uvS.xy, vec2( 0.0 ) ) ) ||
                         any( greaterThanEqual( uvS.xy, vec2( 1.0 ) ) ) )
                        break;
                    const ivec2 texel = ivec2( uvS.xy * extent );
                    if ( texel == pixel )
                        continue;
                    // Both points lie on the same pixel's view ray: their distances to the camera compare.
                    const vec3  scene     = ReconstructWorldPosition( ( vec2( texel ) + 0.5 ) * giFrame.screen.zw,
                                                                      texelFetch( GBufferDepthTexture, texel, 0 ).r,
                                                                      giFrame.invViewProjection );
                    // An occluder is the depth buffer's surface where the ray passes behind it, and only while that
                    // surface is within the ray's reach of the shaded point: farther away it is a foreground object
                    // the short ray passes behind (the gap between a crown and the wall), which the probes' own
                    // traces occlude, not this contact term. The bound is 3D, so it holds at any depth and angle.
                    const float rayDist   = length( q - camera );
                    const float behind    = rayDist - length( scene - camera );
                    occluded              = behind > bias && length( scene - origin ) < rayLength;
                }
                if ( !occluded )
                {
                    unoccludedSum += l;
                    unoccluded += 1.0;
                }
            }
            const float ao   = unoccluded / float( SR_AO_RAYS );
            const vec3  bent = dot( unoccludedSum, unoccludedSum ) > 1e-6 ? normalize( unoccludedSum ) : n;
            imageStore( ShortRangeAoOutTexture, pixel, vec4( bent, ao ) );
        }
    }
}
