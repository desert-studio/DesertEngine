// DesertAsset {"Kind":"Shader","Guid":"1d3e01f044e7489da669573cc6e7a7b4","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 2/4 — TRACE (UE LumenScreenProbeTracing.usf: ScreenProbeTraceScreenTexturesCS, then
// ScreenProbeTraceMeshSDFsCS, then ScreenProbeTraceVoxelsCS with the radiance cache's far field, here one dispatch):
// one workgroup per probe, one ray per hemi-octahedral texel, jittered inside its texel every frame. A ray goes
// through two legs, the second starting where the first left off:
//   SCREEN    over this frame's closest-depth HZB (Common/HZBTrace.glslh, the walk SSR uses); a hit within the
//             depth buffer's thickness returns LAST frame's lit scene where the hit was (the view's scene colour
//             history, reprojected by the camera - UE GetHistoryScreenPosition), which carries every bounce the
//             view has lit so far. A ray that left the screen in front of everything is certain to the screen edge:
//             the next leg starts there; one that went behind a surface or ran out of steps is uncertain: the next
//             leg starts at the probe (UE bUncertain).
//   WORLD     the GI ray contract (Common/GITracing.glslh: the permutation's tracer, GIHitRadiance on a hit) from
//             where the screen left off - the software tracer marches the mesh SDFs of the view's near-field
//             froxel grid first (UE LumenMeshSDFCulling; a hit reads its object's cards first), then the global
//             field - only as far as the world radiance cache's coverage asks
//             (minTraceDistanceBeforeCache: past it the cache's probes see what the ray would); a ray that gets
//             there without a hit reads the cache's RADIANCE in its direction at the probe, from the probes that
//             see it (RadianceCacheRadianceAt, UE SampleRadianceCacheInterpolated). Outside the cache, or where its
//             probes have not traced yet, the ray goes on to the full trace distance and the sky.
// The program is compiled per tracer (GlobalIllumination::TracerVariant); it names none. .a keeps the distance the
// ray's radiance came from (the filter's angle test).
Shader "ScreenProbeTrace"
{
    Compute
    {
        LocalSize(8, 8, 1);

        #include <Common/GIFrame.glslh>
        #include <Common/ScreenProbes.glslh>
        #include <Common/RadianceCache.glslh>
        #include <Mesh/LightSources.glslh>
        #include <Mesh/LightGrid.glslh>

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
        ReadBuffer(4) PointLightsUB
        {
            PointLight pointLights[];
        };
        ReadBuffer(5) SpotLightsUB
        {
            SpotLight spotLights[];
        };
        ReadBuffer(6) LightGridParamsUB
        {
            LightGridParams lightGrid;
        };
        ReadBuffer(7) LightGridWorldCellsUB
        {
            uint lightGridWorldCells[];
        };
        #define LIGHT_GRID_WORLD_CELLS lightGridWorldCells
        #include <Mesh/LightGrid.glslh>
        ReadBuffer(8) RadianceCacheParamsUB
        {
            RadianceCacheParams radianceCache;
        };
        ReadBuffer(9) RadianceCacheIndirectionUB
        {
            uint radianceCacheIndirection[];
        };
        ReadBuffer(10) RadianceCacheProbesUB
        {
            RadianceCacheProbe radianceCacheProbes[];
        };

        // 32..42: the tracer's own resources (Common/GITracer.glslh, GI_TRACER_BINDING below).
        Uniform(16) samplerCube u_EnvSpecularTex;
        Uniform(17) sampler2D u_GBufferDepth;
        Uniform(18) sampler2D u_GBufferA;
        Uniform(19) sampler2D u_GBufferEmissive;
        Uniform(20) sampler2D u_RadianceCacheIrradiance;
        Uniform(21) sampler2D u_RadianceCacheDepth;
        Uniform(22) sampler2D u_RadianceCacheRadianceAtlas;
        layout(binding = 23, rgba16f) restrict writeonly uniform image2D u_ScreenProbeTraced;

        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #define RADIANCE_CACHE_RADIANCE_ATLAS u_RadianceCacheRadianceAtlas
        #include <Common/RadianceCache.glslh>
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 25
        #include <Common/GIHitLighting.glslh>
        Uniform(43) sampler2D u_HZB;               // this frame's closest-depth hierarchy (all mips, point)
        Uniform(44) sampler2D u_SceneColorHistory; // last frame's lit opaque scene (SceneColorHistoryRenderer)
        #include <Common/HZBTrace.glslh>

        // How far a ray looks before it is a miss, cm (UE r.Lumen.MaxTraceDistance; the radiance cache's own).
        #define SP_TRACE_DISTANCE 20000.0
        // The screen leg (UE r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.MaxIterations / RelativeDepthThickness):
        // the walk's step budget, and how far behind the depth buffer's surface a stop still counts as a hit, as a
        // share of the surface's view depth.
        #define SP_SCREEN_ITERATIONS 50
        #define SP_SCREEN_RELATIVE_THICKNESS 0.02
        // cm the screen ray leaves the probe's surface along its normal (SSR's).
        #define SP_SCREEN_BIAS 2.0

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
            float distance; // the hit's, or where the next leg starts (0: uncertain, from the probe)
        };

        // UE ScreenProbeTraceScreenTexturesCS over the HZB walk, from @p origin (world, biased) along @p dir.
        SPScreenTrace SPTraceScreen( vec3 origin, vec3 dir, float tMax, float noise )
        {
            SPScreenTrace result;
            result.hit      = false;
            result.radiance = vec3( 0.0 );
            result.distance = 0.0;
            vec3 originScreen;
            if ( !SPProjectToScreen( origin, originScreen ) || any( lessThan( originScreen.xy, vec2( 0.0 ) ) ) ||
                 any( greaterThan( originScreen.xy, vec2( 1.0 ) ) ) )
                return result; // UE: the bias pushed the start off the screen
            // The screen direction: a point a short way along, cut short of the camera plane (SSR.shader).
            const float originW = ( giFrame.viewProjection * vec4( origin, 1.0 ) ).w;
            const float dw      = ( giFrame.viewProjection * vec4( dir, 0.0 ) ).w;
            float       rayLength = 0.25 * originW;
            if ( dw < 0.0 )
                rayLength = min( rayLength, 0.5 * originW / -dw );
            vec3 endScreen;
            if ( !SPProjectToScreen( origin + dir * rayLength, endScreen ) )
                return result;
            const HZBTraceResult walk = HZBTraceScreen( originScreen, endScreen - originScreen, SP_SCREEN_ITERATIONS );
            const bool offScreen = any( lessThan( walk.Position.xy, vec2( 0.0 ) ) ) ||
                                   any( greaterThan( walk.Position.xy, vec2( 1.0 ) ) );
            if ( walk.Exhausted )
            {
                // Off the screen in front of every surface it passed: certain that far (UE writes the distance).
                if ( offScreen && walk.Position.z > 0.0 && walk.Position.z < 1.0 )
                {
                    const vec3 exitWorld =
                         ReconstructWorldPosition( clamp( walk.Position.xy, 0.0, 1.0 ), walk.Position.z,
                                                   giFrame.invViewProjection );
                    result.distance = min( max( distance( origin, exitWorld ) - 2.0 * SP_MESH_SDF_BIAS, 0.0 ), tMax );
                }
                return result;
            }
            const float surfaceZ = textureLod( u_GBufferDepth, walk.Position.xy, 0.0 ).r;
            const vec3  rayAt    = ReconstructWorldPosition( walk.Position.xy, walk.Position.z, giFrame.invViewProjection );
            const vec3  surface  = ReconstructWorldPosition( walk.Position.xy, surfaceZ, giFrame.invViewProjection );
            const float viewDepth = ( giFrame.viewProjection * vec4( surface, 1.0 ) ).w;
            if ( distance( rayAt, surface ) > SP_SCREEN_RELATIVE_THICKNESS * viewDepth )
                return result; // behind the depth buffer's first layer: uncertain, the world legs from the probe
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
                result.distance = max( hitDistance - 2.0 * SP_MESH_SDF_BIAS, 0.0 );
                return result;
            }
            result.hit      = true;
            result.radiance = textureLod( u_SceneColorHistory, prevUV, 0.0 ).rgb;
            result.distance = hitDistance;
            return result;
        }

        // Per texel and frame: a jitter inside the texel and a dither (PCG hash).
        vec3 TexelNoise( uint probe, uint texel, uint frame )
        {
            uvec3 v = uvec3( probe, texel, frame ) * 1664525u + 1013904223u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            v ^= v >> 16u;
            v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
            return vec3( v & 0xFFFFu ) / 65536.0;
        }

        void main()
        {
            const uint probe = gl_WorkGroupID.x;
            const uint uniformProbes = screenProbeParams.grid.z;
            if ( probe >= uniformProbes &&
                 probe - uniformProbes >= min( screenProbeIndex[0], screenProbeParams.grid.w ) )
                return;
            const ScreenProbe state = screenProbes[probe];
            if ( state.position.w <= 0.0 )
                return;

            const uint  texel  = gl_LocalInvocationIndex;
            const ivec2 t      = ivec2( texel % SP_RES, texel / SP_RES );
            const vec3  noise  = TexelNoise( probe, texel, screenProbeParams.atlas.y );
            const vec2  o      = ( vec2( t ) + noise.xy ) / float( SP_RES ) * 2.0 - 1.0;
            const vec3  N      = state.normal.xyz;
            const vec3  P      = state.position.xyz;
            const vec3  dir    = SPProbeFrame( N ) * SPHemiOctDecode( o );
            const GITraceBias bias   = GIRayBias( state.position.xyz, N, dir );
            const vec3        origin = state.position.xyz + N * bias.nearOffset;

            GITraceInput ray;
            ray.origin    = origin;
            ray.tMin      = 0.0;
            ray.farTMin   = GIFarFieldTMin( bias, N, dir );
            ray.direction = dir;
            ray.tMax      = SP_TRACE_DISTANCE;
            // The cone of one texel: the hemisphere's 2 pi over SP_RES^2 texels, as a cone's half angle.
            ray.coneAngle = sqrt( 2.0 / float( SP_RES * SP_RES ) );
            ray.flags     = 0u;

            const RadianceCacheCoverage coverage = RadianceCacheCoverageAt( radianceCache, origin, noise.z );
            if ( coverage.valid )
                ray.tMax = min( coverage.minTraceDistanceBeforeCache, SP_TRACE_DISTANCE );

            // 1. THE SCREEN; the world legs go on from where it left off along the same ray (UE
            //    VoxelTraceStartDistance).
            if ( ( screenProbeParams.atlas.z & SP_FLAG_SCREEN_TRACE ) != 0u )
            {
                const SPScreenTrace screen = SPTraceScreen( P + N * SP_SCREEN_BIAS, dir, ray.tMax, noise.z );
                if ( screen.hit )
                {
                    imageStore( u_ScreenProbeTraced, SPAtlasOrigin( probe, SP_RES ) + t,
                                vec4( screen.radiance, screen.distance ) );
                    return;
                }
                ray.tMin = min( screen.distance, ray.tMax );
            }

            // 2. THE WORLD (the near field's mesh SDFs, then the global field), then the radiance cache's far
            //    field, then the sky.
            vec4                result;
            const GITraceResult near = GITraceRay( ray );
            if ( near.hit )
                result = vec4( GIHitRadiance( near.surface, dir ), near.surface.distance );
            else
            {
                const vec4 far = coverage.valid ? RadianceCacheRadianceAt( origin, N, dir, noise.z ) : vec4( 0.0, 0.0, 0.0, -1.0 );
                if ( far.a > 0.0 )
                    result = vec4( far.rgb, SP_TRACE_DISTANCE );
                else
                {
                    // No cache answer: the rest of the ray traced here, so a new cell is neither black nor sky.
                    ray.tMin = ray.tMax;
                    ray.tMax = SP_TRACE_DISTANCE;
                    const GITraceResult rest = coverage.valid ? GITraceRay( ray ) : near;
                    result = rest.hit ? vec4( GIHitRadiance( rest.surface, dir ), rest.surface.distance )
                                      : vec4( GIMissRadiance( dir ), SP_TRACE_DISTANCE );
                }
            }
            imageStore( u_ScreenProbeTraced, SPAtlasOrigin( probe, SP_RES ) + t, result );
        }
    }
}
