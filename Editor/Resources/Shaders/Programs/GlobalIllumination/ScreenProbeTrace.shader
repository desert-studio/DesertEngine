// DesertAsset {"Kind":"Shader","Guid":"1d3e01f044e7489da669573cc6e7a7b4","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE GATHER, 2/4 — TRACE (UE LumenScreenProbeTracing.usf: ScreenProbeTraceScreenTexturesCS, then
// ScreenProbeTraceMeshSDFsCS, then ScreenProbeTraceVoxelsCS with the radiance cache's far field, here one dispatch):
// one workgroup per probe, one ray per hemi-octahedral texel, jittered inside its texel every frame. A ray goes
// through three legs, each starting where the one before left off:
//   SCREEN    over this frame's closest-depth HZB (Common/HZBTrace.glslh, the walk SSR uses); a hit within the
//             depth buffer's thickness returns LAST frame's lit scene where the hit was (the view's scene colour
//             history, reprojected by the camera - UE GetHistoryScreenPosition), which carries every bounce the
//             view has lit so far. A ray that left the screen in front of everything is certain to the screen edge:
//             the next leg starts there; one that went behind a surface or ran out of steps is uncertain: the next
//             leg starts at the probe (UE bUncertain).
//   MESH SDFS (software tracer) the objects near the probe - the view-culled list, culled again by this workgroup
//             to the sphere its rays reach (UE LumenMeshSDFCulling) - marched out to the near field's distance
//             (Common/GITraceMeshSDF.glslh); a hit reads its object's cards first (GIHitRadiance).
//   WORLD     the GI ray contract (Common/GITracing.glslh: the permutation's tracer, GIHitRadiance on a hit) from
//             the near field's end only as far as the world radiance cache's coverage asks
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

        // 11..16: the tracer's own resources (Common/GITracer.glslh, GI_TRACER_BINDING below).
        Uniform(17) samplerCube u_EnvSpecularTex;
        Uniform(18) sampler2D u_GBufferDepth;
        Uniform(19) sampler2D u_GBufferA;
        Uniform(20) sampler2D u_GBufferEmissive;
        Uniform(21) sampler2D u_RadianceCacheIrradiance;
        Uniform(22) sampler2D u_RadianceCacheDepth;
        Uniform(23) sampler2D u_RadianceCacheRadianceAtlas;
        layout(binding = 24, rgba16f) restrict writeonly uniform image2D u_ScreenProbeTraced;

        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #define RADIANCE_CACHE_RADIANCE_ATLAS u_RadianceCacheRadianceAtlas
        #include <Common/RadianceCache.glslh>
        #define GI_TRACER_BINDING 11
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 25
        #include <Common/GIHitLighting.glslh>
        // 32..37: the mesh SDF near field's (software tracer only).
        #if defined( DESERT_GI_TRACER_SOFTWARE )
        #define GI_MESH_SDF_BINDING 32
        #include <Common/GITraceMeshSDF.glslh>
        #endif
        Uniform(38) sampler2D u_HZB;               // this frame's closest-depth hierarchy (all mips, point)
        Uniform(39) sampler2D u_SceneColorHistory; // last frame's lit opaque scene (SceneColorHistoryRenderer)
        #include <Common/HZBTrace.glslh>

        // How far a ray looks before it is a miss, cm (UE r.Lumen.MaxTraceDistance; the radiance cache's own).
        #define SP_TRACE_DISTANCE 20000.0
        // The screen leg (UE r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.MaxIterations / RelativeDepthThickness):
        // the walk's step budget, and how far behind the depth buffer's surface a stop still counts as a hit, as a
        // share of the surface's view depth.
        #define SP_SCREEN_ITERATIONS 50
        #define SP_SCREEN_RELATIVE_THICKNESS 0.02
        // cm the screen ray leaves the probe's surface along its normal (SSR's), and the near field's origin
        // leaves it along the normal and the ray (UE's mesh SDF SurfaceBias).
        #define SP_SCREEN_BIAS 2.0
        #define SP_MESH_SDF_BIAS 5.0
        // The near field's objects a probe keeps (the first that reach it, in the view-culled list's order).
        #define SP_MESH_SDF_MAX_OBJECTS 64u

        shared uint  s_MeshSDFCount;
        shared uint  s_MeshSDFObjects[SP_MESH_SDF_MAX_OBJECTS];
        shared vec4  s_MeshSDFBoxMin[SP_MESH_SDF_MAX_OBJECTS]; // world box, w = its distance from the probe (cm)
        shared vec3  s_MeshSDFBoxMax[SP_MESH_SDF_MAX_OBJECTS];
        shared uint  s_MeshSDFOrder[SP_MESH_SDF_MAX_OBJECTS];  // the slots nearest the probe first

        // The segment [tMin, tMax] of the ray against a world box: false when it misses.
        bool SPRayHitsBox( vec3 origin, vec3 invDir, float tMin, float tMax, vec3 boxMin, vec3 boxMax )
        {
            const vec3 a     = ( boxMin - origin ) * invDir;
            const vec3 b     = ( boxMax - origin ) * invDir;
            const vec3 lo    = min( a, b );
            const vec3 hi    = max( a, b );
            const float near = max( max( lo.x, lo.y ), max( lo.z, tMin ) );
            const float far  = min( min( hi.x, hi.y ), min( hi.z, tMax ) );
            return near <= far;
        }

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
            const uint  legs   = uint( screenProbeParams.trace.z );
            const float meshSDFDistance = screenProbeParams.trace.x;

            // THE PROBE'S MESH SDF OBJECTS (UE CullMeshSDFObjectsToGrid, per probe here): the view-culled objects
            // whose box lies within the near field's reach of the probe, kept in shared memory for all its rays.
            const bool nearField = ( legs & SP_TRACE_MESH_SDFS ) != 0u && state.position.w <= screenProbeParams.trace.y;
            if ( texel == 0u )
                s_MeshSDFCount = 0u;
            barrier();
        #if defined( DESERT_GI_TRACER_SOFTWARE )
            if ( nearField )
            {
                const float reach = meshSDFDistance + 2.0 * SP_MESH_SDF_BIAS;
                const uint  count = GIMeshSDFCulledCount();
                for ( uint i = texel; i < count; i += SP_RES * SP_RES )
                {
                    vec3       boxMin, boxMax;
                    const uint object = GIMeshSDFCulledObject( i, boxMin, boxMax );
                    const vec3 gap    = max( max( boxMin - P, P - boxMax ), vec3( 0.0 ) );
                    if ( dot( gap, gap ) > reach * reach )
                        continue;
                    const uint slot = atomicAdd( s_MeshSDFCount, 1u );
                    if ( slot < SP_MESH_SDF_MAX_OBJECTS )
                    {
                        s_MeshSDFObjects[slot] = object;
                        s_MeshSDFBoxMin[slot]  = vec4( boxMin, length( gap ) );
                        s_MeshSDFBoxMax[slot]  = boxMax;
                    }
                }
            }
            barrier();
            // Nearest first (a rank per slot): a ray's first hit then clips the objects behind it to nothing.
            {
                const uint kept = min( s_MeshSDFCount, SP_MESH_SDF_MAX_OBJECTS );
                if ( texel < kept )
                {
                    const float own  = s_MeshSDFBoxMin[texel].w;
                    uint        rank = 0u;
                    for ( uint j = 0u; j < kept; ++j )
                    {
                        const float other = s_MeshSDFBoxMin[j].w;
                        rank += ( other < own || ( other == own && j < texel ) ) ? 1u : 0u;
                    }
                    s_MeshSDFOrder[rank] = texel;
                }
            }
        #endif
            barrier();

            // The world leg's origin when no near field ran, and the radiance cache's lookup point.
            const vec3 cacheOrigin = P + N * GIRayBias( P, N, dir ).normalOffset;
            GITraceInput ray;
            ray.origin    = cacheOrigin;
            ray.tMin      = 0.0;
            ray.direction = dir;
            ray.tMax      = SP_TRACE_DISTANCE;
            // The cone of one texel: the hemisphere's 2 pi over SP_RES^2 texels, as a cone's half angle.
            ray.coneAngle = sqrt( 2.0 / float( SP_RES * SP_RES ) );
            ray.flags     = 0u;

            const RadianceCacheCoverage coverage = RadianceCacheCoverageAt( radianceCache, cacheOrigin, noise.z );
            if ( coverage.valid )
                ray.tMax = min( coverage.minTraceDistanceBeforeCache, SP_TRACE_DISTANCE );

            // 1. THE SCREEN.
            float start = 0.0; // where the next leg starts along the ray
            if ( ( legs & SP_TRACE_SCREEN ) != 0u )
            {
                const SPScreenTrace screen = SPTraceScreen( P + N * SP_SCREEN_BIAS, dir, ray.tMax, noise.z );
                if ( screen.hit )
                {
                    imageStore( u_ScreenProbeTraced, SPAtlasOrigin( probe, SP_RES ) + t,
                                vec4( screen.radiance, screen.distance ) );
                    return;
                }
                start = screen.distance;
            }

            // 2. THE MESH SDFS, out to the near field's distance; the world leg goes on from there along the same
            //    ray (UE VoxelTraceStartDistance).
            vec4 result  = vec4( 0.0 );
            bool answered = false;
        #if defined( DESERT_GI_TRACER_SOFTWARE )
            if ( nearField )
            {
                const vec3  nearOrigin = P + ( N + dir ) * SP_MESH_SDF_BIAS;
                const float nearEnd    = min( meshSDFDistance, ray.tMax );
                if ( start < nearEnd )
                {
                    GIMeshSDFTrace near;
                    near.hitDistance = nearEnd;
                    near.hitObject   = 0u;
                    const uint objects = min( s_MeshSDFCount, SP_MESH_SDF_MAX_OBJECTS );
                    const vec3 invDir  = vec3( dir.x != 0.0 ? 1.0 / dir.x : 3.4e38, dir.y != 0.0 ? 1.0 / dir.y : 3.4e38,
                                               dir.z != 0.0 ? 1.0 / dir.z : 3.4e38 );
                    for ( uint k = 0u; k < objects; ++k )
                    {
                        const uint slot = s_MeshSDFOrder[k];
                        // Past the nearest hit so far, or off this ray: nothing of the object can come nearer.
                        if ( s_MeshSDFBoxMin[slot].w > near.hitDistance + 2.0 * SP_MESH_SDF_BIAS ||
                             !SPRayHitsBox( nearOrigin, invDir, start, near.hitDistance, s_MeshSDFBoxMin[slot].xyz,
                                            s_MeshSDFBoxMax[slot] ) )
                            continue;
                        GITraceMeshSDFObject( nearOrigin, dir, start, nearEnd, s_MeshSDFObjects[slot], near );
                    }
                    if ( near.hitDistance < nearEnd )
                    {
                        const GIHit hit = GIMeshSDFHit( nearOrigin, dir, 0.0, near );
                        result          = vec4( GIHitRadiance( hit, dir ), hit.distance );
                        answered        = true;
                    }
                }
                ray.origin = nearOrigin;
                start      = max( start, nearEnd );
            }
        #endif

            // 3. THE WORLD, then the radiance cache's far field, then the sky.
            if ( !answered )
            {
                ray.tMin = min( start, ray.tMax );
                const GITraceResult near = GITraceRay( ray );
                if ( near.hit )
                    result = vec4( GIHitRadiance( near.surface, dir ), near.surface.distance );
                else
                {
                    const vec4 far = coverage.valid ? RadianceCacheRadianceAt( cacheOrigin, N, dir, noise.z ) : vec4( 0.0, 0.0, 0.0, -1.0 );
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
            }
            imageStore( u_ScreenProbeTraced, SPAtlasOrigin( probe, SP_RES ) + t, result );
        }
    }
}
