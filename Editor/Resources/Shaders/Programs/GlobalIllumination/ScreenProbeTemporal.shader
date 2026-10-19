// DesertAsset {"Kind":"Shader","Guid":"3a02605d9d834e589e5e39869562f1e3","Versions":{"SHDR":1},"Dependencies":[]}
// Ported from UE 5.8 Shaders/Private/Lumen/LumenScreenProbeGatherTemporal.usf ScreenProbeTemporalReprojectionCS
// (the history weights, NumFramesAccumulated and the 1 / (1 + frames) blend of the diffuse, rough specular and
// backface diffuse), adapted: the history taps are tested here (Common/ScreenProbeTemporal.glslh, UE's
// classification pass); no fast-update mode (UE's LightIsMoving needs the traces' moving fraction, which the
// gather does not produce: REMAINDER-GI-DENOISE); the short-range AO is not accumulated separately (it is already
// in the integrated diffuse and specular).
// THE SCREEN PROBE GATHER, TEMPORAL FILTER: one thread per G-buffer pixel. The integrated lighting of this frame
// blended with the pixel's reprojected history, by how many frames the history holds: a pixel the history does
// not hold (disoccluded, off-screen last frame, a cut) restarts at this frame's value and converges in a few
// frames (1/2, 1/3, ...), a held one keeps up to SP_TEMPORAL_MAX_FRAMES_ACCUMULATED. Writes the view's new history
// set (SceneViewState ScreenProbeGatherHistory), which the deferred composite reads this frame.
Shader "ScreenProbeTemporal"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ScreenProbes.glslh>
        #include <Common/ReconstructPosition.glslh>

        ReadBuffer(0) ScreenProbeParamsUB
        {
            ScreenProbeParams screenProbeParams;
        };
        ReadBuffer(1) GIFrameUB
        {
            GIFrame giFrame;
        };
        Uniform(2) sampler2D u_GBufferDepth;
        Uniform(3) sampler2D u_GBufferB;
        Uniform(4) sampler2D Velocity;                 // NDC current - previous (View/Velocity.hpp)
        Uniform(5) sampler2D DiffuseIndirect;          // this frame's integrate
        Uniform(6) sampler2D RoughSpecularIndirect;
        Uniform(7) sampler2D BackfaceDiffuseIndirect;
        Uniform(8) sampler2D DiffuseIndirectHistory;   // the view's previous set
        Uniform(9) sampler2D RoughSpecularIndirectHistory;
        Uniform(10) sampler2D BackfaceDiffuseIndirectHistory;
        Uniform(11) sampler2D HistoryGuide;
        layout(binding = 12, rgba16f) restrict writeonly uniform image2D NewHistoryDiffuseIndirect;
        layout(binding = 13, rgba16f) restrict writeonly uniform image2D NewHistoryRoughSpecularIndirect;
        layout(binding = 14, rgba16f) restrict writeonly uniform image2D NewHistoryBackfaceDiffuseIndirect;
        layout(binding = 15, rgba16f) restrict writeonly uniform image2D NewHistoryGuide;

        #define SCREEN_PROBE_HISTORY_DECLARED
        #include <Common/ScreenProbeTemporal.glslh>

        // The same hash as ScreenProbeIntegrate's SPPixelNoise, another stream (UE BlueNoiseScalar).
        float TemporalNoise( uvec2 pixel, uint frame )
        {
            uvec3 h = uvec3( pixel.x, pixel.y * 4u + 3u, frame ) * 1664525u + 1013904223u;
            h.x += h.y * h.z; h.y += h.z * h.x; h.z += h.x * h.y;
            h ^= h >> 16u;
            h.x += h.y * h.z; h.y += h.z * h.x; h.z += h.x * h.y;
            return float( h.x & 0xFFFFu ) / 65536.0;
        }

        void main()
        {
            const ivec2 pixel  = ivec2( gl_GlobalInvocationID.xy );
            if ( any( greaterThanEqual( pixel, ivec2( giFrame.screen.xy ) ) ) )
                return;
            const vec4 gb = texelFetch( u_GBufferB, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( NewHistoryDiffuseIndirect, pixel, vec4( 0.0 ) );
                imageStore( NewHistoryRoughSpecularIndirect, pixel, vec4( 0.0 ) );
                imageStore( NewHistoryBackfaceDiffuseIndirect, pixel, vec4( 0.0 ) );
                imageStore( NewHistoryGuide, pixel, vec4( 0.0 ) );
                return;
            }
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const vec3  p        = ReconstructWorldPosition( uv, texelFetch( u_GBufferDepth, pixel, 0 ).r,
                                                             giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const float distance = max( length( p - giFrame.camera.xyz ), 1e-3 );

            vec3 diffuse  = texelFetch( DiffuseIndirect, pixel, 0 ).rgb;
            vec3 specular = texelFetch( RoughSpecularIndirect, pixel, 0 ).rgb;
            vec3 backface = texelFetch( BackfaceDiffuseIndirect, pixel, 0 ).rgb;

            const SPHistoryTaps taps =
                 SPHistoryReproject( pixel, p, n, TemporalNoise( uvec2( pixel ), screenProbeParams.atlas.y ) );
            float frames = 0.0;
            if ( SPHistoryAnyValid( taps ) )
            {
                frames            = min( SPHistoryFrames( taps ) + 1.0, SP_TEMPORAL_MAX_FRAMES_ACCUMULATED );
                const float alpha = 1.0 / ( 1.0 + frames );
                diffuse  = mix( SPHistoryAverage( DiffuseIndirectHistory, taps ).rgb, diffuse, alpha );
                specular = mix( SPHistoryAverage( RoughSpecularIndirectHistory, taps ).rgb, specular, alpha );
                backface = mix( SPHistoryAverage( BackfaceDiffuseIndirectHistory, taps ).rgb, backface, alpha );
            }
            imageStore( NewHistoryDiffuseIndirect, pixel, vec4( diffuse, distance ) );
            imageStore( NewHistoryRoughSpecularIndirect, pixel, vec4( specular, 1.0 ) );
            imageStore( NewHistoryBackfaceDiffuseIndirect, pixel, vec4( backface, 1.0 ) );
            imageStore( NewHistoryGuide, pixel, vec4( n, frames ) );
        }
    }
}
