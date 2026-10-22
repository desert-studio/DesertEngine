// DesertAsset {"Kind":"Shader","Guid":"1d3e01f044e7489da669573cc6e7a7b4","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE TRACE, 3/3 — THE WORLD (UE LumenScreenProbeTracing.usf ScreenProbeTraceMeshSDFsCS, then
// ScreenProbeTraceVoxelsCS with the radiance cache's far field): one thread per ray the screen
// (ScreenProbeTraceScreen.shader) left open, read from the compacted list (ScreenProbeCompactTraces.shader) and
// rebuilt from its probe and texel (Common/ScreenProbeTracing.glslh). Two permutations, two passes, each going on from
// where the previous leg left off along the same ray (the trace hit buffer's distance; UE VoxelTraceStartDistance):
//   SP_TRACE_NEAR_FIELD  the GI ray contract's near field (Common/GITracing.glslh GITraceNearField: the software
//             tracer's mesh SDFs of the view's near-field froxel grid, UE LumenMeshSDFCulling; a hit reads its
//             object's cards first) over the rays not past its end; a hit's radiance goes to ScreenProbes.Traced, a
//             miss hands the far field where it goes on.
//   SP_TRACE_FAR_FIELD   over every ray with no hit: the contract's far field (GITraceFarField: the global field) only
//             as far as the world radiance cache's coverage asks (minTraceDistanceBeforeCache: past it the cache's
//             probes see what the ray would); a ray that gets there without a hit reads the cache's RADIANCE in its
//             direction at the probe, from the probes that see it (RadianceCacheRadianceAt, UE
//             SampleRadianceCacheInterpolated). Outside the cache, or where its probes have not traced yet, the ray
//             goes on to the full trace distance and the sky.
// The program is compiled per tracer (GlobalIllumination::TracerVariant) and per leg; it names no tracer. .a keeps the
// distance the ray's radiance came from (the filter's angle test).
Shader "ScreenProbeTrace"
{
    Compute
    {
        LocalSize(64, 1, 1);

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
        Buffer(11) ScreenProbeTraceHitUB
        {
            uint screenProbeTraceHit[];
        };
        ReadBuffer(12) ScreenProbeCompactAllocatorUB
        {
            uint screenProbeCompactAllocator; // the rays listed
        };
        ReadBuffer(13) ScreenProbeCompactTexelsUB
        {
            uint screenProbeCompactTexels[]; // probe * SP_RES² + texel
        };
        ReadBuffer(14) ScreenProbeSpeedUB
        {
            float screenProbeSpeed[]; // per probe, world cm per frame (ScreenProbePlace): the moving test
        };

        // 32..42: the tracer's own resources (Common/GITracer.glslh, GI_TRACER_BINDING below).
        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound by the pass by name and numbered by the program build (ShaderMapBuild.cpp
        // AssignSharedSamplerBindings).
        Uniform sampler LinearClampSampler;
        Uniform sampler PointClampSampler;
        Uniform(16) textureCube EnvironmentSpecularTexture;
        #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)
        Uniform(17) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(18) texture2D GBufferATexture;
        #define GBufferATexture sampler2D(GBufferATexture, PointClampSampler)
        Uniform(19) texture2D GBufferEmissiveTexture;
        #define GBufferEmissiveTexture sampler2D(GBufferEmissiveTexture, PointClampSampler)
        Uniform(20) texture2D RadianceCacheIrradianceTexture;
        #define RadianceCacheIrradianceTexture sampler2D(RadianceCacheIrradianceTexture, LinearClampSampler)
        Uniform(21) texture2D RadianceCacheDepthTexture;
        #define RadianceCacheDepthTexture sampler2D(RadianceCacheDepthTexture, LinearClampSampler)
        Uniform(22) texture2D RadianceCacheRadianceAtlasTexture;
        #define RadianceCacheRadianceAtlasTexture sampler2D(RadianceCacheRadianceAtlasTexture, LinearClampSampler)
        layout(binding = 23, rgba16f) restrict writeonly uniform image2D ScreenProbeTracedTexture;

        #define SCREEN_PROBES_DECLARED
        #include <Common/ScreenProbes.glslh>
        #include <Common/ScreenProbeTracing.glslh>
        #define RADIANCE_CACHE_INDIRECTION radianceCacheIndirection
        #define RADIANCE_CACHE_PROBES radianceCacheProbes
        #define RADIANCE_CACHE_RADIANCE_ATLAS RadianceCacheRadianceAtlasTexture
        #include <Common/RadianceCache.glslh>
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 25
        #include <Common/GIHitLighting.glslh>
        #if !defined( SP_TRACE_NEAR_FIELD ) && !defined( SP_TRACE_FAR_FIELD )
        #define SP_TRACE_FAR_FIELD 1 // the registered default compile (no pass dispatches it)
        #endif

        // UE IsTraceMoving on a world hit: only a hit whose tracer knows its object's motion (GIHit.motionKnown:
        // the mesh SDF near field) can be moving; a global-field hit is not (UE TraceVoxels writes bMoving false).
        bool SPTraceHitMoving( uint probe, SPTexelRay texel, GIHit hit )
        {
            const float probeDepth = ( giFrame.viewProjection * vec4( texel.position, 1.0 ) ).w;
            return hit.motionKnown && SPIsTraceMoving( screenProbeSpeed[probe], probeDepth, hit.velocity );
        }

        void main()
        {
            if ( gl_GlobalInvocationID.x >= screenProbeCompactAllocator )
                return;
            const uint       slot  = screenProbeCompactTexels[gl_GlobalInvocationID.x];
            const uint       probe = slot / ( SP_RES * SP_RES );
            const SPTexelRay texel = SPMakeTexelRay( probe, slot % ( SP_RES * SP_RES ) );
            const uint       state = screenProbeTraceHit[slot];
            const vec3       N     = texel.normal;
            const vec3       dir   = texel.direction;
            const GITraceBias bias   = GIRayBias( texel.position, N, dir );
            const vec3        origin = texel.position + N * bias.nearOffset;
            const ivec2       at     = SPAtlasOrigin( probe, SP_RES ) + texel.texel;

            GITraceInput ray;
            ray.origin    = origin;
            ray.tMin      = min( SPTraceHitDistance( state ), texel.tMax );
            ray.farTMin   = GIFarFieldTMin( bias, N, dir );
            ray.direction = dir;
            ray.tMax      = texel.tMax;
            // The cone of one texel: the hemisphere's 2 pi over SP_RES^2 texels, as a cone's half angle.
            ray.coneAngle = sqrt( 2.0 / float( SP_RES * SP_RES ) );
            ray.flags     = 0u;

#if defined( SP_TRACE_NEAR_FIELD )
            float               farStart;
            const GITraceResult near = GITraceNearField( ray, farStart );
            if ( near.hit )
            {
                imageStore( ScreenProbeTracedTexture, at, vec4( GIHitRadiance( near.surface, dir ), near.surface.distance ) );
                screenProbeTraceHit[slot] =
                     SPEncodeTraceHit( near.surface.distance, true, SPTraceHitMoving( probe, texel, near.surface ), false );
            }
            else
                screenProbeTraceHit[slot] =
                     SPEncodeTraceHit( farStart, false, false, texel.coverage.valid && farStart >= texel.tMax );
#else
            // The far field, then the radiance cache's far field, then the sky.
            vec4                result;
            bool                moving = false;
            const GITraceResult far    = GITraceFarField( ray );
            bool                hit    = far.hit;
            if ( far.hit )
            {
                result = vec4( GIHitRadiance( far.surface, dir ), far.surface.distance );
                moving = SPTraceHitMoving( probe, texel, far.surface );
            }
            else
            {
                const vec4 cache = texel.coverage.valid ? RadianceCacheRadianceAt( origin, N, dir, texel.noise.z )
                                                        : vec4( 0.0, 0.0, 0.0, -1.0 );
                if ( cache.a > 0.0 )
                    result = vec4( cache.rgb, SP_TRACE_DISTANCE );
                else
                {
                    // No cache answer: the rest of the ray traced here, so a new cell is neither black nor sky.
                    ray.tMin                 = ray.tMax;
                    ray.tMax                 = SP_TRACE_DISTANCE;
                    const GITraceResult rest = texel.coverage.valid ? GITraceRay( ray ) : far;
                    result = rest.hit ? vec4( GIHitRadiance( rest.surface, dir ), rest.surface.distance )
                                      : vec4( GIMissRadiance( dir ), SP_TRACE_DISTANCE );
                    hit    = rest.hit;
                    moving = rest.hit && SPTraceHitMoving( probe, texel, rest.surface );
                }
            }
            imageStore( ScreenProbeTracedTexture, at, result );
            // The ray's final state (UE TraceVoxels and the far-field ray tracing pass write theirs): the filter
            // reads its moving bit.
            screenProbeTraceHit[slot] = SPEncodeTraceHit( result.a, hit, moving, false );
#endif
        }
    }
}
