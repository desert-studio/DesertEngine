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
//             probes see what the ray would); a ray that gets there without a hit is left at the cache (reached, no
//             hit, nothing stored) for the cache-read leg. Outside the cache the ray goes on to the full trace
//             distance and the sky here (tMax is that distance).
//   SP_TRACE_CACHE_READ  over the rays the far field left at the cache (GPU-FARFIELD-6; their own compacted list,
//             UE's trace / shading split): each reads the cache's RADIANCE in its direction at the probe, from the
//             probes that see it (RadianceCacheRadianceAt, UE SampleRadianceCacheInterpolated). The far field's
//             waves no longer hold the cache read's eight-cell latency on their march lanes, and this leg's waves
//             are all cache reads (no march registers). Where no traced probe answers (cells the view just
//             reached), nothing is stored: the ray stays at the cache, reached and unanswered.
//   SP_TRACE_CACHE_MISS  over those rays only (their own compacted list, so a few full-length rays no longer hold
//             the far field's every wave): the rest of the ray, from the cache's distance to the full trace distance
//             and the sky, so a new cell is neither black nor sky.
//   SP_TRACE_HIT_LIGHTING over the far field's global-field hits (GPU-FARFIELD-3/5; UE defers hit lighting to a
//             compacted pass of its own): the far field marches with GI_TRACE_DISTANCE_ONLY and keeps only a hit's
//             distance (hit and reached bits both set, nothing stored); this leg rebuilds the surface on the ray and
//             lights it - the cards first (GIHitRadianceNeedsGlobalFieldInPlace), else in place
//             (GIHitRadianceGlobalFieldInPlace: the directional lights' global-field shadow marches, the cache's
//             bounce). The far field's waves no longer wait on the gradient, card search and shadow marches of their
//             few hit lanes; here every lane is such a hit. The software tracer only - no other tracer makes a
//             global-field hit.
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
        Uniform(16) textureCube EnvironmentSpecularTexture;
        #define EnvironmentSpecularTexture samplerCube(EnvironmentSpecularTexture, LinearClampSampler)
        // GPU-FARFIELD-7 (UE shared samplers: a permutation declares the samplers its code samples through): the
        // legs that LIGHT a hit read the cards (Common/SurfaceCacheSampling.glslh), the G-buffer (GIHitMaterial)
        // and the software field's object grid (GIHitObjectGridRows), all through PointClampSampler. One switch
        // per leg, SP_TRACE_LIGHTS_HITS, guards the sampler, the G-buffer textures and (GI_HIT_LIGHTING_NONE)
        // those readers in the headers; the pass binds them under the same rule (ScreenProbeGather.cpp
        // TraceLegLightsHits). The software far field defers its hits, the cache read reads the cache alone.
        #if !defined( SP_TRACE_NEAR_FIELD ) && !defined( SP_TRACE_FAR_FIELD ) && !defined( SP_TRACE_CACHE_MISS )
        #if !defined( SP_TRACE_HIT_LIGHTING ) && !defined( SP_TRACE_CACHE_READ )
        #define SP_TRACE_FAR_FIELD 1 // the registered default compile (no pass dispatches it)
        #endif
        #endif
        #if defined( SP_TRACE_NEAR_FIELD ) || defined( SP_TRACE_CACHE_MISS )
        #define SP_TRACE_LIGHTS_HITS 1
        #elif defined( SP_TRACE_HIT_LIGHTING ) && defined( DESERT_GI_TRACER_SOFTWARE )
        #define SP_TRACE_LIGHTS_HITS 1
        #elif defined( SP_TRACE_FAR_FIELD ) && !defined( DESERT_GI_TRACER_SOFTWARE )
        #define SP_TRACE_LIGHTS_HITS 1
        #endif
        #if defined( SP_TRACE_LIGHTS_HITS )
        Uniform sampler PointClampSampler;
        Uniform(17) texture2D GBufferDepthTexture;
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(18) texture2D GBufferATexture;
        #define GBufferATexture sampler2D(GBufferATexture, PointClampSampler)
        Uniform(19) texture2D GBufferEmissiveTexture;
        #define GBufferEmissiveTexture sampler2D(GBufferEmissiveTexture, PointClampSampler)
        #else
        #define GI_HIT_LIGHTING_NONE 1
        #endif
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

        // UE IsTraceMoving on a world hit: only a hit whose tracer knows its object's motion (GIHit.motionKnown:
        // the mesh SDF near field) can be moving; a global-field hit is not (UE TraceVoxels writes bMoving false).
        bool SPTraceHitMoving( uint probe, SPTexelRay texel, GIHit hit )
        {
            const float probeDepth = ( giFrame.viewProjection * vec4( texel.position, 1.0 ) ).w;
            return hit.motionKnown && SPIsTraceMoving( screenProbeSpeed[probe], probeDepth, hit.velocity );
        }

#if defined( SP_TRACE_MISS_REASONS )
        // One atomic per subgroup and reason: the cache-read leg reads the cache on every one of its rays.
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
        // (software: each lit by the hit-lighting leg; hardware: by GIHitRadiance here), the
        // rays outside the cache's coverage (marched to SP_TRACE_DISTANCE). One atomic per subgroup: the active lanes are the class.
        #define SP_FAR_FIELD_RAYS_LISTED  ( RADIANCE_CACHE_READ_REASONS + 0u )
        #define SP_FAR_FIELD_RAYS_HIT     ( RADIANCE_CACHE_READ_REASONS + 1u )
        #define SP_FAR_FIELD_RAYS_OUTSIDE ( RADIANCE_CACHE_READ_REASONS + 2u )
        // GPU-FARFIELD-3: the global field's sphere-trace steps of the far field's own marches (all its listed
        // rays: steps a ray = this / LISTED), the hits it handed to the hit-lighting leg, and that leg's steps
        // (its directional lights' shadow marches: steps a hit = this / DEFERRED).
        #define SP_FAR_FIELD_STEPS        ( RADIANCE_CACHE_READ_REASONS + 3u )
        #define SP_FAR_FIELD_RAYS_DEFERRED ( RADIANCE_CACHE_READ_REASONS + 4u )
        #define SP_HIT_LIGHTING_STEPS     ( RADIANCE_CACHE_READ_REASONS + 5u )
        // GPU-FARFIELD-5: every software far-field hit is handed over (DEFERRED = HIT); the hit-lighting leg's hits
        // a card holds (lit by the cards, no shadow march), the rest lit in place.
        #define SP_HIT_LIGHTING_CARD_HELD ( RADIANCE_CACHE_READ_REASONS + 6u )
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
            // A far-field hit (the far field left it with the hit and reached bits): its surface rebuilt on the same
            // ray at the same distance (the finest clipmap's gradient), lit by the cards, else in place.
            const float distance = SPTraceHitDistance( state );
            const GIHit hitAt = GIFarFieldSurface( origin + dir * distance, dir, distance, -1 );
            vec3        radiance;
            if ( GIHitRadianceNeedsGlobalFieldInPlace( hitAt, dir, radiance ) )
                radiance = GIHitRadianceGlobalFieldInPlace( hitAt, dir );
#if defined( SP_TRACE_MISS_REASONS )
            else
                SPCountFarFieldRays( SP_HIT_LIGHTING_CARD_HELD );
            SPCountSteps( SP_HIT_LIGHTING_STEPS );
#endif
            imageStore( ScreenProbeTracedTexture, at, vec4( radiance, distance ) );
            // A global-field hit is never moving (UE TraceVoxels writes bMoving false).
            screenProbeTraceHit[slot] = SPEncodeTraceHit( distance, true, false, false );
#endif
#elif defined( SP_TRACE_CACHE_READ )
            // A ray the far field left at the cache (reached, no hit; only covered rays get there). The coverage's
            // point and clipmap (UE SampleRadianceCacheInterpolated with FRadianceCacheCoverage): the probe's own
            // pixel, seen from the camera, so the eight cells read are the ones RadianceCacheMark stamped for that
            // pixel (RadianceCacheReadPoint), in the clipmap tMax was cut for.
            const vec3 V     = normalize( giFrame.camera.xyz - texel.position );
            uint       why   = RADIANCE_CACHE_READ_EMPTY;
            const vec4 cache = RadianceCacheRadianceAt( texel.position, N, V, dir, texel.coverage.clipmap, why );
#if defined( SP_TRACE_MISS_REASONS )
            SPCountCacheRead( why );
#endif
            // No traced probe answers, in the coverage's clipmap or a coarser one (RadianceCacheRadianceAt's
            // fallback): the state stays reached and unanswered, and the cache-miss leg traces the rest of the ray.
            if ( cache.a > 0.0 )
            {
                imageStore( ScreenProbeTracedTexture, at, vec4( cache.rgb, SP_TRACE_DISTANCE ) );
                screenProbeTraceHit[slot] = SPEncodeTraceHit( SP_TRACE_DISTANCE, false, false, false );
            }
#else
            // The far field, then the radiance cache's far field, then the sky.
            vec4                result;
            bool                moving = false;
#if defined( SP_TRACE_MISS_REASONS )
            SPCountFarFieldRays( SP_FAR_FIELD_RAYS_LISTED );
            if ( !texel.coverage.valid )
                SPCountFarFieldRays( SP_FAR_FIELD_RAYS_OUTSIDE );
#endif
#if defined( DESERT_GI_TRACER_SOFTWARE )
            // GPU-FARFIELD-5: the march keeps the hit's distance only (no gradient normal here: the hit-lighting
            // leg rebuilds the surface where it lights it).
            ray.flags |= GI_TRACE_DISTANCE_ONLY;
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
                // Every software far-field hit is a global-field hit (UE TraceVoxels): the hit-lighting leg lights
                // it - the cards first, else in place - in its own compacted list (UE's hit lighting pass), so this
                // leg's waves wait neither on a card search (99.5 % of these hits find no card) nor on a shadow
                // march of their few hit lanes. Nothing stored yet; hit + reached marks it.
#if defined( SP_TRACE_MISS_REASONS )
                SPCountFarFieldRays( SP_FAR_FIELD_RAYS_DEFERRED );
#endif
                screenProbeTraceHit[slot] = SPEncodeTraceHit( far.surface.distance, true, false, true );
                return;
#else
                // The hardware far field's hits are triangles (never global-field): lit here.
                result = vec4( GIHitRadiance( far.surface, dir ), far.surface.distance );
                moving = SPTraceHitMoving( probe, texel, far.surface );
#endif
            }
            else if ( texel.coverage.valid )
            {
                // At the cache: the cache-read leg reads it in its own compacted list (UE's trace / shading split),
                // so this leg's waves wait on no eight-cell cache read and carry none of its registers.
                screenProbeTraceHit[slot] = SPEncodeTraceHit( ray.tMax, false, false, true );
                return;
            }
            else
                result = vec4( GIMissRadiance( dir ), SP_TRACE_DISTANCE ); // tMax was the full distance
            imageStore( ScreenProbeTracedTexture, at, result );
            // The ray's final state (UE TraceVoxels and the far-field ray tracing pass write theirs): the filter
            // reads its moving bit.
            screenProbeTraceHit[slot] = SPEncodeTraceHit( result.a, hit, moving, false );
#endif
        }
    }
}
