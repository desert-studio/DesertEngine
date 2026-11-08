// DesertAsset {"Kind":"Shader","Guid":"1d3e01f044e7489da669573cc6e7a7b4","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE TRACE, 3/3 — THE WORLD (UE LumenScreenProbeTracing.usf ScreenProbeTraceMeshSDFsCS, then
// ScreenProbeTraceVoxelsCS with the radiance cache's far field): one thread per ray the screen
// (ScreenProbeTraceScreen.shader) left open, read from the compacted list (ScreenProbeCompactTraces.shader) and
// rebuilt from its probe and texel (Common/ScreenProbeTracing.glslh). Three permutations, three passes, each going on from
// where the previous leg left off along the same ray (the trace hit buffer's distance; UE VoxelTraceStartDistance):
//   SP_TRACE_NEAR_FIELD  the GI ray contract's near field (Common/GITracing.glslh GITraceNearField: the software
//             tracer's mesh SDFs of the view's near-field froxel grid, UE LumenMeshSDFCulling; a hit reads its
//             object's cards first) over the rays not past its end; a hit's radiance goes to ScreenProbes.Traced, a
//             miss hands the far field where it goes on.
//   SP_TRACE_FAR_FIELD   over every ray with no hit: the contract's far field (GITraceFarField: the global field) only
//             as far as the world radiance cache's coverage asks (minTraceDistanceBeforeCache: past it the cache's
//             probes see what the ray would); a ray that gets there without a hit reads the cache's RADIANCE in its
//             direction at the probe, from the probes that see it (RadianceCacheRadianceAt, UE
//             SampleRadianceCacheInterpolated). Outside the cache the ray goes on to the full trace distance and the
//             sky here (tMax is that distance). Where the cache covers the ray but no traced probe answers (cells the
//             view just reached), nothing is traced here: the ray is left at the cache, reached and unanswered.
//   SP_TRACE_CACHE_MISS  over those rays only (their own compacted list, so a few full-length rays no longer hold
//             the far field's every wave): the rest of the ray, from the cache's distance to the full trace distance
//             and the sky, so a new cell is neither black nor sky.
//   SP_TRACE_HIT_LIGHTING over the far field's global-field hits (GPU-FARFIELD-3, GPU-SCREENPROBE-9; UE defers hit
//             lighting to a compacted pass of its own): the far field keeps only a hit's distance (hit and
//             reached bits both set, nothing stored) and this leg rebuilds the surface on the ray, reads the cards
//             (GIHitRadianceNeedsGlobalFieldInPlace) and lights in place the hits no card holds
//             (GIHitRadianceGlobalFieldInPlace: the directional lights' global-field shadow marches, the cache's
//             bounce). The far field's waves no longer wait on the card lookup or the shadow marches of their few
//             hit lanes; here every lane is a hit. The software tracer only - no other tracer makes a global-field
//             hit.
// The program is compiled per tracer (GlobalIllumination::TracerVariant) and per leg; it names no tracer. .a keeps the
// distance the ray's radiance came from (the filter's angle test).
Shader "ScreenProbeTrace"
{
    Compute
    {
#if defined( SP_TRACE_MISS_REASONS )
        // The dev counter's atomics: one per subgroup and reason (subgroupBallot / subgroupElect), the step totals
        // summed per subgroup (subgroupAdd).
        #extension GL_KHR_shader_subgroup_basic : require
        #extension GL_KHR_shader_subgroup_ballot : require
        #extension GL_KHR_shader_subgroup_arithmetic : require
#endif
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
#if defined( SP_TRACE_MISS_REASONS )
        // PROBE-TURN-1 (dev, the far-field leg only): every read of the radiance cache this frame by why it answered
        // or not (RADIANCE_CACHE_READ_*, Common/RadianceCache.glslh), zeroed per frame and read back into the
        // profile dump (ScreenProbeGather.cpp ScreenProbeMissCounts).
        Buffer(15) ScreenProbeMissReasonsUB
        {
            uint screenProbeMissReasons[];
        };
#endif

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
#if defined( SP_TRACE_MISS_REASONS )
        // The global field's sphere-trace steps this thread took (Common/GlobalDistanceField.glslh GDF_COUNT_STEPS).
        #define GDF_COUNT_STEPS 1
        uint gdfCountedSteps = 0u;
#endif
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 25
        #include <Common/GIHitLighting.glslh>
        #if !defined( SP_TRACE_NEAR_FIELD ) && !defined( SP_TRACE_FAR_FIELD ) && !defined( SP_TRACE_CACHE_MISS )
        #if !defined( SP_TRACE_HIT_LIGHTING )
        #define SP_TRACE_FAR_FIELD 1 // the registered default compile (no pass dispatches it)
        #endif
        #endif

        // UE IsTraceMoving on a world hit: only a hit whose tracer knows its object's motion (GIHit.motionKnown:
        // the mesh SDF near field) can be moving; a global-field hit is not (UE TraceVoxels writes bMoving false).
        bool SPTraceHitMoving( uint probe, SPTexelRay texel, GIHit hit )
        {
            const float probeDepth = ( giFrame.viewProjection * vec4( texel.position, 1.0 ) ).w;
            return hit.motionKnown && SPIsTraceMoving( screenProbeSpeed[probe], probeDepth, hit.velocity );
        }

#if defined( SP_TRACE_MISS_REASONS )
        // One atomic per subgroup and reason: the far field reads the cache on most of its rays.
        void SPCountCacheRead( uint why )
        {
            for ( uint reason = 0u; reason < RADIANCE_CACHE_READ_REASONS; ++reason )
            {
                const uvec4 ballot = subgroupBallot( why == reason );
                const uint  count  = subgroupBallotBitCount( ballot );
                if ( subgroupElect() && count > 0u )
                    atomicAdd( screenProbeMissReasons[reason], count );
            }
        }

        // GPU-FARFIELD-1: the far-field leg's rays by what its own trace did, after the cache-read words
        // (ScreenProbeGather.hpp ScreenProbeMissReasons, same order): every ray listed, the rays the global field hit
        // (each lit by GIHitRadiance: the cards, or by the directional lights, one global-field test each), the
        // rays outside the cache's coverage (marched to SP_TRACE_DISTANCE). One atomic per subgroup: the active lanes are the class.
        #define SP_FAR_FIELD_RAYS_LISTED  ( RADIANCE_CACHE_READ_REASONS + 0u )
        #define SP_FAR_FIELD_RAYS_HIT     ( RADIANCE_CACHE_READ_REASONS + 1u )
        #define SP_FAR_FIELD_RAYS_OUTSIDE ( RADIANCE_CACHE_READ_REASONS + 2u )
        // GPU-FARFIELD-3: the global field's sphere-trace steps of the far field's own marches (all its listed
        // rays: steps a ray = this / LISTED), the hits no card holds (counted by the hit-lighting leg, which gets
        // every hit and lights these in place), and that leg's steps (its directional lights' shadow marches:
        // steps a hit = this / DEFERRED).
        #define SP_FAR_FIELD_STEPS        ( RADIANCE_CACHE_READ_REASONS + 3u )
        #define SP_FAR_FIELD_RAYS_DEFERRED ( RADIANCE_CACHE_READ_REASONS + 4u )
        #define SP_HIT_LIGHTING_STEPS     ( RADIANCE_CACHE_READ_REASONS + 5u )
        void SPCountFarFieldRays( uint word )
        {
            const uint count = subgroupBallotBitCount( subgroupBallot( true ) );
            if ( subgroupElect() )
                atomicAdd( screenProbeMissReasons[word], count );
        }
        // The active lanes' gdfCountedSteps into @p word, one atomic per subgroup.
        void SPCountSteps( uint word )
        {
            const uint total = subgroupAdd( gdfCountedSteps );
            if ( subgroupElect() && total > 0u )
                atomicAdd( screenProbeMissReasons[word], total );
        }
#endif

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
#elif defined( SP_TRACE_CACHE_MISS )
            // The rest of a ray the cache did not answer: from the cache's distance to the full trace distance.
            ray.tMin                 = ray.tMax;
            ray.tMax                 = SP_TRACE_DISTANCE;
            const GITraceResult rest = GITraceRay( ray );
            const vec4 result = rest.hit ? vec4( GIHitRadiance( rest.surface, dir ), rest.surface.distance )
                                         : vec4( GIMissRadiance( dir ), SP_TRACE_DISTANCE );
            imageStore( ScreenProbeTracedTexture, at, result );
            screenProbeTraceHit[slot] =
                 SPEncodeTraceHit( result.a, rest.hit, rest.hit && SPTraceHitMoving( probe, texel, rest.surface ), false );
#elif defined( SP_TRACE_HIT_LIGHTING )
#if defined( DESERT_GI_TRACER_SOFTWARE )
            // A far-field hit (the far field left it with the hit and reached bits): its surface rebuilt on the
            // same ray at the same distance (the normal from the finest clipmap holding it), read from the cards
            // first and lit in place where no card holds it (GIHitRadiance in its two halves, for the counter).
            const float distance = SPTraceHitDistance( state );
            const GIHit hitAt    = GIFarFieldSurface( origin + dir * distance, dir, distance, -1 );
            vec3        radiance;
            if ( GIHitRadianceNeedsGlobalFieldInPlace( hitAt, dir, radiance ) )
            {
#if defined( SP_TRACE_MISS_REASONS )
                SPCountFarFieldRays( SP_FAR_FIELD_RAYS_DEFERRED );
#endif
                radiance = GIHitRadianceGlobalFieldInPlace( hitAt, dir );
            }
#if defined( SP_TRACE_MISS_REASONS )
            SPCountSteps( SP_HIT_LIGHTING_STEPS );
#endif
            imageStore( ScreenProbeTracedTexture, at, vec4( radiance, distance ) );
            // A global-field hit is never moving (UE TraceVoxels writes bMoving false).
            screenProbeTraceHit[slot] = SPEncodeTraceHit( distance, true, false, false );
#endif
#else
            // The far field, then the radiance cache's far field, then the sky.
            vec4                result;
            bool                moving = false;
#if defined( SP_TRACE_MISS_REASONS )
            SPCountFarFieldRays( SP_FAR_FIELD_RAYS_LISTED );
            if ( !texel.coverage.valid )
                SPCountFarFieldRays( SP_FAR_FIELD_RAYS_OUTSIDE );
#endif
            const GITraceResult far    = GITraceFarField( ray );
#if defined( SP_TRACE_MISS_REASONS )
            SPCountSteps( SP_FAR_FIELD_STEPS );
#endif
            bool                hit    = far.hit;
            if ( far.hit )
            {
#if defined( SP_TRACE_MISS_REASONS )
                SPCountFarFieldRays( SP_FAR_FIELD_RAYS_HIT );
#endif
#if defined( DESERT_GI_TRACER_SOFTWARE )
                // EVERY global-field hit goes to the hit-lighting leg (UE TraceVoxels keeps no hit lighting
                // in the trace; LumenScreenProbeHitLighting is its own pass): this leg keeps only the distance,
                // hit + reached marks it. The cards' lookup (SurfaceCacheHitLighting: the object grid, the
                // instance rows, the card samples) and the gradient normal ran here on a quarter of the lanes
                // while the rest of their wave waited, and found no card on ~99 % of them; the leg runs it on
                // waves that are all hits, and this kernel keeps only the march and the cache read (fewer
                // registers for the latency-bound read). A global-field hit is never moving (UE TraceVoxels
                // writes bMoving false).
                screenProbeTraceHit[slot] = SPEncodeTraceHit( far.surface.distance, true, false, true );
                return;
#else
                // The hardware tracer's far field makes no global-field hit: its hits are lit here.
                result = vec4( GIHitRadiance( far.surface, dir ), far.surface.distance );
                moving = SPTraceHitMoving( probe, texel, far.surface );
#endif
            }
            else
            {
                // The coverage's point and clipmap (UE SampleRadianceCacheInterpolated with FRadianceCacheCoverage):
                // the probe's own pixel, seen from the camera, so the eight cells read are the ones RadianceCacheMark
                // stamped for that pixel (RadianceCacheReadPoint), in the clipmap tMax was cut for.
                vec4 cache = vec4( 0.0, 0.0, 0.0, -1.0 );
                uint why   = RADIANCE_CACHE_READ_EMPTY;
                if ( texel.coverage.valid )
                {
                    const vec3 V = normalize( giFrame.camera.xyz - texel.position );
                    cache        = RadianceCacheRadianceAt( texel.position, N, V, dir, texel.coverage.clipmap, why );
#if defined( SP_TRACE_MISS_REASONS )
                    SPCountCacheRead( why );
#endif
                }
                if ( cache.a > 0.0 )
                    result = vec4( cache.rgb, SP_TRACE_DISTANCE );
                else if ( texel.coverage.valid )
                {
                    // No traced probe answers, in the coverage's clipmap or a coarser one (RadianceCacheRadianceAt's
                    // fallback): the cache-miss leg traces the rest of this ray.
                    screenProbeTraceHit[slot] = SPEncodeTraceHit( ray.tMax, false, false, true );
                    return;
                }
                else
                    result = vec4( GIMissRadiance( dir ), SP_TRACE_DISTANCE ); // tMax was the full distance
            }
            imageStore( ScreenProbeTracedTexture, at, result );
            // The ray's final state (UE TraceVoxels and the far-field ray tracing pass write theirs): the filter
            // reads its moving bit.
            screenProbeTraceHit[slot] = SPEncodeTraceHit( result.a, hit, moving, false );
#endif
        }
    }
}
