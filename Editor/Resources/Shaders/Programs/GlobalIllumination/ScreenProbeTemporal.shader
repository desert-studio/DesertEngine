// DesertAsset {"Kind":"Shader","Guid":"3a02605d9d834e589e5e39869562f1e3","Versions":{"SHDR":1},"Dependencies":[]}
// Ported from UE 5.8 Shaders/Private/Lumen/LumenScreenProbeGatherTemporal.usf ScreenProbeTemporalReprojectionCS
// (the history weights, NumFramesAccumulated and the 1 / (1 + frames) blend of the diffuse, rough specular and
// backface diffuse), adapted: the history taps are tested here (Common/ScreenProbeTemporal.glslh, UE's
// classification pass: depth reprojected by the velocity's depth, the foliage threshold and material test); the
// fast-update mode as UE's (:486-500: LightIsMoving - the traces' moving share, ScreenProbeFilter, interpolated by
// the integrate into the rough specular's alpha - turns into a fast-update amount that caps the frames the history
// may hold; the amount kept in the new rough specular history's alpha, UE's FastUpdateMode_NumFramesAccumulated)
// without its neighbourhood clamp (UE r.Lumen.ScreenProbeGather.Temporal.FastUpdateModeUseNeighborhoodClamp,
// off by default); the short-range AO is not accumulated separately (it is already in the integrated diffuse and
// specular).
// THE SCREEN PROBE GATHER, TEMPORAL FILTER: one thread per G-buffer pixel. The integrated lighting of this frame
// blended with the pixel's reprojected history, by how many frames the history holds: a pixel the history does
// not hold (disoccluded, off-screen last frame, a cut) restarts at this frame's value and converges in a few
// frames (1/2, 1/3, ...), a held one keeps up to SP_TEMPORAL_MAX_FRAMES_ACCUMULATED - fewer where the probes'
// rays hit something moving relative to them (fast update: moving lighting leaves no trail). Writes the view's new
// history set (SceneViewState ScreenProbeGatherHistory), which the deferred composite reads this frame.
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
        Uniform sampler PointClampSampler;
        Uniform(2) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(3) texture2D GBufferBTexture;
        #define GBufferBTexture sampler2D(GBufferBTexture, PointClampSampler)
        Uniform(4) texture2D Velocity;                 // NDC.xyz current - previous (View/Velocity.hpp)
        #define Velocity sampler2D(Velocity, PointClampSampler)
        Uniform(5) texture2D DiffuseIndirect;          // this frame's integrate
        #define DiffuseIndirect sampler2D(DiffuseIndirect, PointClampSampler)
        Uniform(6) texture2D RoughSpecularIndirect;    // .a = LightIsMoving (ScreenProbeIntegrate)
        #define RoughSpecularIndirect sampler2D(RoughSpecularIndirect, PointClampSampler)
        Uniform(7) texture2D BackfaceDiffuseIndirect;
        #define BackfaceDiffuseIndirect sampler2D(BackfaceDiffuseIndirect, PointClampSampler)
        Uniform(8) texture2D DiffuseIndirectHistory;   // the view's previous set
        Uniform(9) texture2D RoughSpecularIndirectHistory; // .a = the fast-update amount
        Uniform(10) texture2D BackfaceDiffuseIndirectHistory;
        Uniform(11) texture2D HistoryGuide;
        layout(binding = 12, rgba16f) restrict writeonly uniform image2D NewHistoryDiffuseIndirect;
        layout(binding = 13, rgba16f) restrict writeonly uniform image2D NewHistoryRoughSpecularIndirect;
        layout(binding = 14, rgba16f) restrict writeonly uniform image2D NewHistoryBackfaceDiffuseIndirect;
        layout(binding = 15, rgba16f) restrict writeonly uniform image2D NewHistoryGuide;
        Uniform(16) utexture2D GBufferShadingWord;     // the foliage flag (DesertHasBackfaceDiffuse)
        #define GBufferShadingWord usampler2D(GBufferShadingWord, PointClampSampler)

        #include <Generated/ShadingModels.glslh>
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
            const vec4 gb = texelFetch( GBufferBTexture, pixel, 0 );
            if ( dot( gb.xyz, gb.xyz ) <= 0.001 )
            {
                imageStore( NewHistoryDiffuseIndirect, pixel, vec4( 0.0 ) );
                imageStore( NewHistoryRoughSpecularIndirect, pixel, vec4( 0.0 ) );
                imageStore( NewHistoryBackfaceDiffuseIndirect, pixel, vec4( 0.0 ) );
                imageStore( NewHistoryGuide, pixel, vec4( 0.0 ) );
                return;
            }
            const vec2  uv       = ( vec2( pixel ) + 0.5 ) * giFrame.screen.zw;
            const float depth    = texelFetch( GBufferDepthTexture, pixel, 0 ).r;
            const vec3  p        = ReconstructWorldPosition( uv, depth, giFrame.invViewProjection );
            const vec3  n        = normalize( gb.xyz );
            const float distance = max( length( p - giFrame.camera.xyz ), 1e-3 );
            const bool  foliage  = SPPixelIsFoliage( pixel );

            vec3        diffuse       = texelFetch( DiffuseIndirect, pixel, 0 ).rgb;
            const vec4  newSpecular   = texelFetch( RoughSpecularIndirect, pixel, 0 );
            vec3        specular      = newSpecular.rgb;
            const float lightIsMoving = newSpecular.a;
            vec3        backface      = texelFetch( BackfaceDiffuseIndirect, pixel, 0 ).rgb;

            const float         noise = TemporalNoise( uvec2( pixel ), screenProbeParams.atlas.y );
            const SPHistoryTaps taps  = SPHistoryReproject( pixel, depth, p, n, foliage, noise );
            float frames         = 0.0;
            float fastUpdateOut  = 0.0; // UE OutFastUpdateModeAmount: this frame's amount, into the history
            if ( ( screenProbeParams.atlas.z & SP_FLAG_HISTORY_VALID ) != 0u )
            {
                // UE (VALID_HISTORY): this frame's amount from the moving share, held at least at the history's
                // (it stabilises the amount and keeps propagating around where the moving object was last frame).
                const vec4 historySpecular = SPHistoryAverage( RoughSpecularIndirectHistory, taps );
                fastUpdateOut              = SPFastUpdateModeAmount( lightIsMoving );
                const float fastUpdate =
                     max( fastUpdateOut, min( historySpecular.a, SP_TEMPORAL_MAX_FAST_UPDATE_MODE_AMOUNT ) );
                if ( SPHistoryAnyValid( taps ) )
                {
                    frames = min( min( SPHistoryFrames( taps ) + 1.0, SP_TEMPORAL_MAX_FRAMES_ACCUMULATED ),
                                  ( 1.0 - fastUpdate ) * SP_TEMPORAL_MAX_FRAMES_ACCUMULATED );
                    const float alpha = 1.0 / ( 1.0 + frames );
                    diffuse  = mix( SPHistoryAverage( DiffuseIndirectHistory, taps ).rgb, diffuse, alpha );
                    specular = mix( historySpecular.rgb, specular, alpha );
                    backface = mix( SPHistoryAverage( BackfaceDiffuseIndirectHistory, taps ).rgb, backface, alpha );
                }
            }
            imageStore( NewHistoryDiffuseIndirect, pixel, vec4( diffuse, distance ) );
            imageStore( NewHistoryRoughSpecularIndirect, pixel, vec4( specular, fastUpdateOut ) );
            imageStore( NewHistoryBackfaceDiffuseIndirect, pixel, vec4( backface, 1.0 ) );
            imageStore( NewHistoryGuide, pixel, vec4( n, SPPackGuideW( frames, foliage ) ) );
        }
    }
}
