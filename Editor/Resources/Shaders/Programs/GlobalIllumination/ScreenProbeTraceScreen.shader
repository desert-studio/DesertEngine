// DesertAsset {"Kind":"Shader","Guid":"69a3f579c1744c1b9e91cc5d075f7b0e","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE TRACE, 1/3 — THE SCREEN (UE LumenScreenProbeTracing.usf ScreenProbeTraceScreenTexturesCS; with the
// screen trace off it is UE's ClearTracesCS): one workgroup per probe, one ray per hemi-octahedral texel
// (Common/ScreenProbeTracing.glslh), walked over this frame's closest-depth HZB (Common/HZBTrace.glslh, the walk SSR
// uses). A hit within the depth buffer's thickness returns LAST frame's lit scene where the hit was (the view's scene
// colour history, reprojected by the camera - UE GetHistoryScreenPosition), which carries every bounce the view has
// lit so far. The walk goes only as far as the world legs would (the radiance cache's coverage); every point it
// crossed in front of the depth buffer is certain, so a ray it does not answer (off the screen, out of steps, behind
// a surface, stopped by something thin) goes on from the last such point (UE bWriteDepthOnMiss), and one that walked
// the whole way has reached the cache. Every texel's state goes to the trace hit buffer; the world legs
// (ScreenProbeTrace.shader) run over the rays it left open (ScreenProbeCompactTraces.shader).
Shader "ScreenProbeTraceScreen"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ScreenProbes.glslh>
        #include <Common/RadianceCache.glslh>

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
        ReadBuffer(4) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        Buffer(5) ScreenProbeTraceHitUB
        {
            uint screenProbeTraceHit[];
        };
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D ScreenProbeTracedTexture;
        Uniform sampler PointClampSampler;
        Uniform(7) texture2D HzbTexture;               // this frame's closest-depth hierarchy (all mips, point)
        #define HzbTexture sampler2D(HzbTexture, PointClampSampler)
        Uniform sampler LinearClampSampler;
        Uniform(8) texture2D SceneColorHistoryTexture; // last frame's lit opaque scene (SceneColorHistoryRenderer)
        #define SceneColorHistoryTexture sampler2D(SceneColorHistoryTexture, LinearClampSampler)
        ReadBuffer(9) ScreenProbeSpeedUB
        {
            float screenProbeSpeed[]; // per probe, world cm per frame (ScreenProbePlace)
        };
        Uniform(10) texture2D Velocity; // NDC.xyz current - previous (View/Velocity.hpp): a hit's world motion
        #define Velocity sampler2D(Velocity, PointClampSampler)

        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #include <Common/ScreenProbeTracing.glslh>
        #include <Common/ReconstructPosition.glslh>
        #include <Common/HZBTrace.glslh>

        // The screen leg (UE r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.*, LumenScreenProbeTracing.cpp:66-93):
        // the walk's step budget (MaxIterations); how far behind the depth buffer's surface a stop still counts as a
        // hit, as a share of the surface's view depth (RelativeDepthThickness); the mip-0 texels past a hit that must
        // stay behind the depth for the hit to be certain (NumThicknessStepsToDetermineCertainty).
        #define SP_SCREEN_ITERATIONS 50
        #define SP_SCREEN_RELATIVE_THICKNESS 0.02
        #define SP_SCREEN_THICKNESS_STEPS 4
        // cm the screen ray leaves the probe's surface along its normal (SSR's).
        #define SP_SCREEN_BIAS 2.0
        // cm a world leg starts before the point up to which the screen was certain (twice UE's mesh SDF
        // SurfaceBias, 5 cm; SSR.shader's kWorldStartBackoff): the world's surface stands off the depth buffer's.
        #define SP_WORLD_START_BACKOFF 10.0

        // World -> (uv, deviceZ) of the G-buffer's (jittered) projection, uv top-left (the engine's flipped
        // viewport). False behind the camera.
        bool SPProjectToScreen( vec3 p, out vec3 screen )
        {
            const vec4 clip = giFrame.viewProjection * vec4( p, 1.0 );
            if ( clip.w <= 0.0 )
                return false;
            screen = vec3( clip.xy / clip.w * vec2( 0.5, -0.5 ) + 0.5, clip.z / clip.w );
            return true;
        }

        // The screen leg's verdict for one ray.
        struct SPScreenTrace
        {
            bool  hit;      // radiance holds last frame's lit scene at the hit
            vec3  radiance;
            float distance; // the hit's, or where the next leg starts (0: from the probe)
            vec3  velocity; // a hit's world motion this frame, cm (UE HitWorldVelocity)
        };

        // UE ScreenProbeTraceScreenTexturesCS (LumenScreenProbeTracing.usf:57-370, the HZB-traversal permutation):
        // the walk from @p origin (world, biased) along @p dir only as far as @p tMax, the distance past which the
        // radiance cache answers (UE TraceDistance = min(MaxTraceDistance, MinTraceDistanceBeforeInterpolation)).
        // Whatever the screen does not answer, the world legs go on with from the last point the walk found in front
        // of the depth buffer (UE writes the trace distance on every miss of the HZB walk, bWriteDepthOnMiss).
        SPScreenTrace SPTraceScreen( vec3 origin, vec3 dir, float tMax, float noise )
        {
            SPScreenTrace result;
            result.hit      = false;
            result.radiance = vec3( 0.0 );
            result.distance = 0.0;
            result.velocity = vec3( 0.0 );
            vec3 originScreen;
            if ( !SPProjectToScreen( origin, originScreen ) || any( lessThan( originScreen.xy, vec2( 0.0 ) ) ) ||
                 any( greaterThan( originScreen.xy, vec2( 1.0 ) ) ) )
                return result; // UE: the bias pushed the start off the screen
            // The segment's end: tMax along, cut at 0.99 of the way to the camera plane so it projects (UE TraceHZB).
            const float originW = ( giFrame.viewProjection * vec4( origin, 1.0 ) ).w;
            const float dw      = ( giFrame.viewProjection * vec4( dir, 0.0 ) ).w;
            const float endDistance = dw < 0.0 ? min( 0.99 * originW / -dw, tMax ) : tMax;
            vec3 endScreen;
            if ( !SPProjectToScreen( origin + dir * endDistance, endScreen ) )
                return result;
            const HZBTraceResult walk = HZBTraceScreen( originScreen, endScreen, SP_SCREEN_ITERATIONS );
            if ( !walk.Stopped )
            {
                // In front of every surface it passed, as far as it got: certain that far. The whole segment up to
                // tMax: nothing on screen before the trace's end, the world legs have nothing left to do.
                if ( walk.ReachedEnd && !walk.Clipped && endDistance >= tMax )
                {
                    result.distance = tMax;
                    return result;
                }
                const vec3 reached =
                     ReconstructWorldPosition( walk.Position.xy, walk.Position.z, giFrame.invViewProjection );
                result.distance = min( max( distance( origin, reached ) - SP_WORLD_START_BACKOFF, 0.0 ), tMax );
                return result;
            }
            // The stop's thickness (UE TraceHZB: the ray's view depth behind the surface's, relative to it).
            const float surfaceZ  = texelFetch( HzbTexture, clamp( ivec2( walk.Position.xy * vec2( textureSize( HzbTexture, 0 ) ) ),
                                                              ivec2( 0 ), textureSize( HzbTexture, 0 ) - 1 ), 0 ).r;
            const vec3  rayAt     = ReconstructWorldPosition( walk.Position.xy, walk.Position.z, giFrame.invViewProjection );
            const vec3  surface   = ReconstructWorldPosition( walk.Position.xy, surfaceZ, giFrame.invViewProjection );
            const float rayDepth  = ( giFrame.viewProjection * vec4( rayAt, 1.0 ) ).w;
            const float viewDepth = ( giFrame.viewProjection * vec4( surface, 1.0 ) ).w;
            if ( rayDepth - viewDepth >= SP_SCREEN_RELATIVE_THICKNESS * viewDepth ||
                 HZBThinSurfaceAhead( walk, originScreen, SP_SCREEN_THICKNESS_STEPS ) )
            {
                // Behind the depth buffer's first layer, or stopped by something thin: the world legs go on from
                // the last point the walk saw in front of the depth.
                const vec3 visible =
                     ReconstructWorldPosition( walk.LastVisible.xy, walk.LastVisible.z, giFrame.invViewProjection );
                result.distance = min( max( distance( origin, visible ) - SP_WORLD_START_BACKOFF, 0.0 ), tMax );
                return result;
            }
            const float hitDistance = distance( origin, surface );
            if ( hitDistance >= tMax )
            {
                result.distance = tMax; // nothing on screen before the trace's end
                return result;
            }
            // Where the hit was last frame (UE GetHistoryScreenPosition: the camera's motion).
            const vec4 prevClip = screenProbeParams.prevViewProjection * vec4( surface, 1.0 );
            const vec2 prevUV   = prevClip.w > 0.0 ? prevClip.xy / prevClip.w * vec2( 0.5, -0.5 ) + 0.5 : vec2( -1.0 );
            // UE: no hit near the screen's edge (dithered by the vignette), nor where last frame did not see it; the
            // next leg starts just before the surface the screen found (the depth there was certain).
            const float vignette = min( HZBScreenEdgeFade( walk.Position.xy ),
                                        any( lessThan( prevUV, vec2( 0.0 ) ) ) || any( greaterThan( prevUV, vec2( 1.0 ) ) )
                                             ? 0.0
                                             : HZBScreenEdgeFade( prevUV ) );
            if ( vignette < noise )
            {
                result.distance = max( hitDistance - SP_WORLD_START_BACKOFF, 0.0 );
                return result;
            }
            result.hit      = true;
            // Stored pre-exposed by last frame's P (ViewFrame::PrevPreExposure): undone, the probes stay absolute.
            result.radiance = textureLod( SceneColorHistoryTexture, prevUV, 0.0 ).rgb * screenProbeParams.prevCamera.w;
            result.distance = hitDistance;
            // UE: the hit's position minus where its velocity says it was last frame (GetHistoryScreenPosition at
            // the hit; TAA jitter included, as UE notes). Only the moving test reads it.
            const ivec2 velocityTexel = clamp( ivec2( walk.Position.xy * vec2( textureSize( Velocity, 0 ) ) ),
                                               ivec2( 0 ), textureSize( Velocity, 0 ) - 1 );
            const vec3 previousNdc = SPPreviousSurfaceNdc( screenProbeParams, walk.Position.xy, surfaceZ,
                                                           texelFetch( Velocity, velocityTexel, 0 ).xyz );
            result.velocity = surface - SPPreviousWorldPosition( screenProbeParams, previousNdc );
            return result;
        }

        void main()
        {
            const uint probe = gl_WorkGroupID.x;
            if ( !SPProbeTraced( probe ) )
                return;
            const uint       texel = gl_LocalInvocationIndex;
            const SPTexelRay ray   = SPMakeTexelRay( probe, texel );
            const uint       slot  = probe * SP_RES * SP_RES + texel;

            float distance = 0.0;
            if ( ( screenProbeParams.atlas.z & SP_FLAG_SCREEN_TRACE ) != 0u )
            {
                const SPScreenTrace screen =
                     SPTraceScreen( ray.position + ray.normal * SP_SCREEN_BIAS, ray.direction, ray.tMax, ray.noise.z );
                if ( screen.hit )
                {
                    imageStore( ScreenProbeTracedTexture, SPAtlasOrigin( probe, SP_RES ) + ray.texel,
                                vec4( screen.radiance, screen.distance ) );
                    const float probeDepth = ( giFrame.viewProjection * vec4( ray.position, 1.0 ) ).w;
                    const bool  moving = SPIsTraceMoving( screenProbeSpeed[probe], probeDepth, screen.velocity );
                    screenProbeTraceHit[slot] = SPEncodeTraceHit( screen.distance, true, moving, false );
                    return;
                }
                distance = min( screen.distance, ray.tMax );
            }
            // The world legs go on from here along the same ray (UE VoxelTraceStartDistance); a ray the screen walked
            // all the way has reached the radiance cache.
            screenProbeTraceHit[slot] =
                 SPEncodeTraceHit( distance, false, false, ray.coverage.valid && distance >= ray.tMax );
        }
    }
}
