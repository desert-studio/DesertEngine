// DesertAsset {"Kind":"Shader","Guid":"1d3e01f044e7489da669573cc6e7a7b4","Versions":{"SHDR":1},"Dependencies":[]}
// THE SCREEN PROBE TRACE, 3/3 — THE WORLD (UE LumenScreenProbeTracing.usf ScreenProbeTraceMeshSDFsCS, then
// ScreenProbeTraceVoxelsCS with the radiance cache's far field): one thread per ray the screen
// (ScreenProbeTraceScreen.shader) left open, read from its leg's list and rebuilt from its probe and texel
// (Common/ScreenProbeTracing.glslh); a leg lists the rays it leaves for the next one itself (THE LEGS' LISTS there).
// Three permutations, three passes, each going on from
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
        // The legs' list appends (SPListRay: subgroupBallot / subgroupElect / subgroupBroadcastFirst).
        #extension GL_KHR_shader_subgroup_basic : require
        #extension GL_KHR_shader_subgroup_ballot : require
#if defined( SP_TRACE_MISS_REASONS )
        // The dev counter's atomics: one per subgroup and reason, the step totals summed per subgroup
        // (subgroupAdd).
        #extension GL_KHR_shader_subgroup_arithmetic : require
#endif
        LocalSize(256, 1, 1); // SP_TRACE_LEG_GROUP_SIZE (Common/ScreenProbeTracing.glslh)

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
        #define SP_TRACE_CACHE_READ 1 // the registered default compile (no pass dispatches it; no tracer, no hits)
        #endif
        #endif
        // THE LEGS' LISTS (Common/ScreenProbeTracing.glslh): the near field lists its misses for the far field,
        // the far field its rays at the cache for the cache read (and, with the software tracer, its hits for the
        // hit lighting: the second list), the cache read the rays it could not answer for the cache miss.
        #if defined( SP_TRACE_NEAR_FIELD ) || defined( SP_TRACE_FAR_FIELD ) || defined( SP_TRACE_CACHE_READ )
        #define SP_TRACE_LISTS_RAYS 1
        Buffer(44) ScreenProbeOutArgsUB
        {
            uvec4 screenProbeOutArgs;
        };
        Buffer(45) ScreenProbeOutAllocatorUB
        {
            uint screenProbeOutAllocator;
        };
        Buffer(46) ScreenProbeOutTexelsUB
        {
            uint screenProbeOutTexels[];
        };
        #endif
        // The second list: the software far field's hits for the hit lighting; the near field's misses at the cache
        // for the cache read (FPS-SP-TRACE-4: they have nothing left to march, so the far field never sees them).
        #if defined( SP_TRACE_FAR_FIELD ) && defined( DESERT_GI_TRACER_SOFTWARE )
        #define SP_TRACE_LISTS_HITS 1
        #endif
        #if defined( SP_TRACE_LISTS_HITS ) || defined( SP_TRACE_NEAR_FIELD )
        #define SP_TRACE_LISTS_RAYS2 1
        Buffer(47) ScreenProbeOut2ArgsUB
        {
            uvec4 screenProbeOut2Args;
        };
        Buffer(48) ScreenProbeOut2AllocatorUB
        {
            uint screenProbeOut2Allocator;
        };
        Buffer(49) ScreenProbeOut2TexelsUB
        {
            uint screenProbeOut2Texels[];
        };
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
#if defined( SP_TRACE_CACHE_READ )
        // The cache-read leg traces nothing and lights no hit: the tracer's types only, so it declares no
        // tracer resource and no shared sampler it would not sample (ScreenProbeGather binds none).
        #include <Common/GITracing.glslh>
#else
        #define GI_TRACER_BINDING 32
        #include <Common/GITracer.glslh>
        #define GI_SURFACE_CACHE_BINDING 25
        #include <Common/GIHitLighting.glslh>
#endif

        // UE IsTraceMoving on a world hit: only a hit whose tracer knows its object's motion (GIHit.motionKnown:
        // the mesh SDF near field) can be moving; a global-field hit is not (UE TraceVoxels writes bMoving false).
#if !defined( SP_TRACE_CACHE_READ )
        bool SPTraceHitMoving( uint probe, SPTexelRay texel, GIHit hit )
        {
            const float probeDepth = ( giFrame.viewProjection * vec4( texel.position, 1.0 ) ).w;
            return hit.motionKnown && SPIsTraceMoving( screenProbeSpeed[probe], probeDepth, hit.velocity );
        }
#endif

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

#if defined( SP_TRACE_LISTS_RAYS )
        // Append the lanes that list their ray to the next leg's list (UE ScreenProbeCompactTracesCS's wave-ops
        // path): in lane order, one atomic on the allocator and one on the dispatch per subgroup. Called once by
        // every lane still running.
        void SPListRay( const bool listed, const uint slot )
        {
            const uvec4 ballot = subgroupBallot( listed );
            const uint  count  = subgroupBallotBitCount( ballot );
            if ( count == 0u )
                return;
            uint start = 0u;
            if ( subgroupElect() )
            {
                start = atomicAdd( screenProbeOutAllocator, count );
                atomicMax( screenProbeOutArgs.x,
                           ( start + count + SP_TRACE_LEG_GROUP_SIZE - 1u ) / SP_TRACE_LEG_GROUP_SIZE );
            }
            start = subgroupBroadcastFirst( start );
            if ( listed )
                screenProbeOutTexels[start + subgroupBallotExclusiveBitCount( ballot )] = slot;
        }
#endif
#if defined( SP_TRACE_LISTS_RAYS2 )
        // The same into the second list (the far field's hits for the hit-lighting leg, the near field's rays at the
        // cache for the cache-read leg).
        void SPListRay2( const bool listed, const uint slot )
        {
            const uvec4 ballot = subgroupBallot( listed );
            const uint  count  = subgroupBallotBitCount( ballot );
            if ( count == 0u )
                return;
            uint start = 0u;
            if ( subgroupElect() )
            {
                start = atomicAdd( screenProbeOut2Allocator, count );
                atomicMax( screenProbeOut2Args.x,
                           ( start + count + SP_TRACE_LEG_GROUP_SIZE - 1u ) / SP_TRACE_LEG_GROUP_SIZE );
            }
            start = subgroupBroadcastFirst( start );
            if ( listed )
                screenProbeOut2Texels[start + subgroupBallotExclusiveBitCount( ballot )] = slot;
        }
#endif

        // FPS-SP-NEARFAR: only the hit-lighting leg orders its group's rays by direction. The near field walks
        // the object list of the ray's origin froxel (GITraceNearField) and the far field marches the global field
        // from the probe's point: in list order a subgroup holds one or two probes' rays, so its lanes walk one
        // object list and fetch one neighbourhood (UE ScreenProbeTraceMeshSDFs / TraceVoxels: coherent per-wave
        // probe access). Binned by direction a subgroup held several probes' froxels - a loop as long as the
        // longest of their lists and scattered fetches - behind fifteen group barriers; the cache read and the
        // cache miss read and trace from the probe's point the same way. Those legs take lane i = the list's ray,
        // with the probe's bias inline and no barrier. The hit lighting lights surfaces the rays reached, where
        // neighbouring probes' rays of one direction land together: it keeps the binning and the bias table.
        #if defined( SP_TRACE_HIT_LIGHTING )
        #define SP_TRACE_LEG_BINS_RAYS 1
        #endif
        #if defined( SP_TRACE_LEG_BINS_RAYS )
        // THE GROUP'S RAYS BY DIRECTION (ray binning: Frostbite / SEED "Hybrid real-time rendering", 2018; UE's
        // ScreenProbeTraceMeshSDFs / TraceVoxels take their compacted list in probe order). A leg's list holds a
        // probe's rays together: one subgroup marched the sky-bound rays and the ground-bound rays of one probe
        // side by side and waited on its longest march every time. The group's 256 listed rays are counted into
        // bins by their texel (a direction in the probe's hemisphere: neighbouring probes' frames nearly agree), the bins
        // laid out by a prefix sum, and lane i traces the i-th ray in bin order - so a subgroup holds a few
        // adjacent texels of several neighbouring probes. A permutation within the group: every ray is traced by
        // exactly one lane, with the same inputs; only which lane does it, and so the order of the next leg's
        // list (SPListRay: any order is a valid list), changes.
        // The key: the Morton order of the texel's (x, y) (SP_RES <= 16: four bits each, below 256), so adjacent
        // keys are adjacent texels in both axes.
        #define SP_LEG_BINS 256u // == SP_TRACE_LEG_GROUP_SIZE: one bin a lane to clear and to scan
        shared uint s_LegBinCount[SP_LEG_BINS];
        shared uint s_LegBinScan[2u * SP_LEG_BINS]; // ping-pong halves of the inclusive prefix sum
        shared uint s_LegSorted[SP_TRACE_LEG_GROUP_SIZE];

        uint SPLegMortonNibble( uint v )
        {
            v = ( v | ( v << 2u ) ) & 0x33u;
            return ( v | ( v << 1u ) ) & 0x55u;
        }

        uint SPLegDirectionKey( uint slot )
        {
            const uint texel = slot % ( SP_RES * SP_RES );
            return SPLegMortonNibble( texel % SP_RES ) | ( SPLegMortonNibble( texel / SP_RES ) << 1u );
        }

        // Called by every thread of the group in uniform control flow (it holds the barriers). False for a lane
        // past the list's end; otherwise @p slot is the ray this lane traces.
        bool SPLegSortedSlot( out uint slot )
        {
            const uint lane   = gl_LocalInvocationIndex;
            const uint first  = gl_WorkGroupID.x * SP_TRACE_LEG_GROUP_SIZE;
            const uint listed = min( screenProbeCompactAllocator - min( first, screenProbeCompactAllocator ),
                                     SP_TRACE_LEG_GROUP_SIZE ); // this group's rays: lanes 0 .. listed - 1
            const bool mine   = lane < listed;
            const uint read   = mine ? screenProbeCompactTexels[first + lane] : 0u;
            const uint key    = SPLegDirectionKey( read );
            s_LegBinCount[lane] = 0u;
            barrier();
            uint rank = 0u;
            if ( mine )
                rank = atomicAdd( s_LegBinCount[key], 1u );
            barrier();
            // The bins' inclusive prefix sum (Hillis-Steele, the screen leg's SPListOpenRays scan).
            uint source = 0u;
            s_LegBinScan[lane] = s_LegBinCount[lane];
            barrier();
            for ( uint offset = 1u; offset < SP_LEG_BINS; offset <<= 1u )
            {
                uint sum = s_LegBinScan[source * SP_LEG_BINS + lane];
                if ( lane >= offset )
                    sum += s_LegBinScan[source * SP_LEG_BINS + lane - offset];
                source ^= 1u;
                s_LegBinScan[source * SP_LEG_BINS + lane] = sum;
                barrier();
            }
            if ( mine )
                s_LegSorted[s_LegBinScan[source * SP_LEG_BINS + key] - s_LegBinCount[key] + rank] = read;
            barrier();
            slot = mine ? s_LegSorted[lane] : 0u;
            return mine;
        }

        // FPS-SP-TRACE-5 THE GROUP'S PROBES' BIAS (UE ScreenProbeGather: per-probe data computed once, read by
        // every trace of the probe). GIRayBias is a function of the probe's point and normal, never of the ray's
        // direction (Common/GITracing.glslh), and the software tracer's walks the froxel's object list: once for
        // each distinct probe among the group's rays, not once a ray. Each listed lane enters its probe into a
        // groupshared open-addressing table (SP_LEG_BINS entries >= the group's rays: it never fills); the lane
        // that enters a probe gives it the next dense index, lanes 0 .. probes - 1 compute one probe's bias each
        // side by side (one or two subgroups' work, not all eight), and every ray reads its probe's. The same
        // function of the same inputs as before: the same bias, bit for bit.
        #define SP_LEG_NO_PROBE 0xFFFFFFFFu
        shared uint        s_LegProbeKey[SP_LEG_BINS];          // the table: a probe, or SP_LEG_NO_PROBE
        shared uint        s_LegProbeIndex[SP_LEG_BINS];        // the table: that probe's dense index
        shared uint        s_LegProbe[SP_TRACE_LEG_GROUP_SIZE]; // dense index -> probe
        shared GITraceBias s_LegBias[SP_TRACE_LEG_GROUP_SIZE];  // dense index -> its probe's bias
        shared uint        s_LegProbes;                         // the group's distinct probes

        // Called by every thread of the group in uniform control flow (it holds the barriers), with what
        // SPLegSortedSlot returned and the probe of its slot. The bias of @p probe's rays when @p listed.
        GITraceBias SPLegProbeBias( const bool listed, const uint probe )
        {
            const uint lane     = gl_LocalInvocationIndex;
            s_LegProbeKey[lane] = SP_LEG_NO_PROBE;
            if ( lane == 0u )
                s_LegProbes = 0u;
            barrier();
            uint entry = probe % SP_LEG_BINS; // a group's probes are mostly consecutive: consecutive entries
            uint index = 0u;
            if ( listed )
            {
                for ( uint tries = 0u; tries < SP_LEG_BINS; ++tries )
                {
                    const uint held = atomicCompSwap( s_LegProbeKey[entry], SP_LEG_NO_PROBE, probe );
                    if ( held == SP_LEG_NO_PROBE )
                    {
                        const uint dense       = atomicAdd( s_LegProbes, 1u );
                        s_LegProbe[dense]      = probe;
                        s_LegProbeIndex[entry] = dense;
                        break;
                    }
                    if ( held == probe )
                        break;
                    entry = ( entry + 1u ) % SP_LEG_BINS;
                }
            }
            barrier();
            if ( lane < s_LegProbes )
            {
                const ScreenProbe state = screenProbes[s_LegProbe[lane]];
                s_LegBias[lane]         = GIRayBias( state.position.xyz, state.normal.xyz );
            }
            barrier();
            if ( listed )
                index = s_LegProbeIndex[entry];
            return s_LegBias[index]; // an unlisted lane's is never read
        }
        #endif // SP_TRACE_LEG_BINS_RAYS

        void main()
        {
#if defined( SP_TRACE_LEG_BINS_RAYS )
            uint              slot;
            const bool        listed = SPLegSortedSlot( slot );
            const GITraceBias bias   = SPLegProbeBias( listed, slot / ( SP_RES * SP_RES ) );
            if ( !listed )
                return; // past the list's end: after the sort's and the bias's barriers
#else
            if ( gl_GlobalInvocationID.x >= screenProbeCompactAllocator )
                return;
            const uint slot = screenProbeCompactTexels[gl_GlobalInvocationID.x]; // list order: by probe
#endif
            const uint       probe = slot / ( SP_RES * SP_RES );
            const SPTexelRay texel = SPMakeTexelRay( probe, slot % ( SP_RES * SP_RES ) );
            const uint       state = screenProbeTraceHit[slot];
            const vec3       N     = texel.normal;
            const vec3       dir   = texel.direction;
            const ivec2       at     = SPAtlasOrigin( probe, SP_RES ) + texel.texel;
#if !defined( SP_TRACE_CACHE_READ )
#if !defined( SP_TRACE_LEG_BINS_RAYS )
            // A function of the probe's point and normal (GIRayBias): its lanes share the probe, so they share it.
            const GITraceBias bias   = GIRayBias( texel.position, N );
#endif
            const vec3        origin = texel.position + N * bias.nearOffset;

            GITraceInput ray;
            ray.origin    = origin;
            ray.tMin      = min( SPTraceHitDistance( state ), texel.tMax );
            ray.farTMin   = GIFarFieldTMin( bias, N, dir );
            ray.direction = dir;
            ray.tMax      = texel.tMax;
            // The cone of one texel: the hemisphere's 2 pi over SP_RES^2 texels, as a cone's half angle.
            ray.coneAngle = sqrt( 2.0 / float( SP_RES * SP_RES ) );
            ray.flags     = 0u;
#endif

#if defined( SP_TRACE_NEAR_FIELD )
            float               farStart;
            const GITraceResult near = GITraceNearField( ray, farStart );
            const bool atCache = !near.hit && texel.coverage.valid && farStart >= texel.tMax;
            if ( near.hit )
            {
                imageStore( ScreenProbeTracedTexture, at, vec4( GIHitRadiance( near.surface, dir ), near.surface.distance ) );
                screenProbeTraceHit[slot] =
                     SPEncodeTraceHit( near.surface.distance, true, SPTraceHitMoving( probe, texel, near.surface ), false );
            }
            else
                screenProbeTraceHit[slot] = SPEncodeTraceHit( farStart, false, false, atCache );
            // FPS-SP-TRACE-4 (UE compacts the voxel trace's rays to those with distance left): a miss at the cache
            // has walked to its tMax already, so it goes straight to the cache read; every other miss goes on in
            // the far field.
            SPListRay( !near.hit && !atCache, slot );
            SPListRay2( atCache, slot );
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
            SPListRay( !( cache.a > 0.0 ), slot ); // the cache-miss leg's list
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
            bool atCache     = false; // left for the cache-read leg (its list)
            bool deferredHit = false; // left for the hit-lighting leg (its list)
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
                deferredHit               = true;
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
                atCache                   = true;
            }
            else
                result = vec4( GIMissRadiance( dir ), SP_TRACE_DISTANCE ); // tMax was the full distance
            if ( !atCache && !deferredHit )
            {
                imageStore( ScreenProbeTracedTexture, at, result );
                // The ray's final state (UE TraceVoxels and the far-field ray tracing pass write theirs): the
                // filter reads its moving bit.
                screenProbeTraceHit[slot] = SPEncodeTraceHit( result.a, hit, moving, false );
            }
            SPListRay( atCache, slot );
#if defined( SP_TRACE_LISTS_HITS )
            SPListRay2( deferredHit, slot );
#endif
#endif
        }
    }
}
