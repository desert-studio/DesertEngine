// DesertAsset {"Kind":"Shader","Guid":"3a2336ad2876438d915c69470c31e245","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSR"
{
    // Screen-space reflections: tile classification and the trace (full or half resolution,
    // Reflections.DownsampleFactor), two dispatches of ONE compute program (SsrParams3.w names the stage), as UE
    // Lumen reflections classify tiles into a compacted list of trace texels and trace that list indirectly.
    // CLASSIFY ("SSR: Classify"): one workgroup per screen tile (grid = ceil(size / 8), tiles stretched over the
    // target). The workgroup decides whether any pixel of its tile passes the trace's G-buffer gate
    // (Common/SSRGate.glslh) and writes that verdict to the tile mask the denoiser's reconstruction reads; then it
    // LISTS the trace texels whose source pixel the tile owns and passes the gate (Common/SSRTrace.glslh
    // ssrTraceList, sizing the trace's indirect arguments) and writes 0 for the others it owns.
    // TRACE ("SSR: Trace"): one thread per listed texel, so a wave never idles on a texel nobody traces. Every trace
    // texel is written every frame by one of the two, so the target needs no clear.
    //
    // The trace (SSR2): per traced texel, Reflections.RaysPerPixel rays whose directions are drawn from the GGX
    // distribution of VISIBLE normals (Heitz 2018; UE MonteCarlo.ush ImportanceSampleVisibleGGX), each walked over the
    // closest-depth HZB (Common/HZBTrace.glslh) and validated into a confidence (SSRHitConfidence). The rays are
    // combined with the RATIO ESTIMATOR (Stachowiak 2015, "Stochastic Screen-Space Reflections"; UE SSRT's
    // reflection reconstruction): sum(w * L) / sum(w), w = the BRDF*cos/pdf of the ray without its Fresnel - the
    // split-sum Fresnel-and-geometry term (the BRDF LUT) multiplies the result in the lighting, exactly as it
    // multiplies the reflection environment. Output rgb = the reflected radiance where rays hit, a = the share of
    // the lobe the hits answered (the COVERAGE); 1 - a is answered by the next source - the reflection captures,
    // then the sky (Mesh/AmbientIBL.glslh DesertComposeTracedReflection) - so a miss, the screen edge or a rough
    // lobe hand over smoothly instead of ending.
    //
    // The radiance a hit returns is LAST FRAME's lit scene (UE: the reprojected previous scene colour), read where
    // the hit point was on last frame's screen: the trace runs before this frame's lighting, which then composes it.
    // Compute has no derivatives, so every filtered read is textureLod(..., 0).
    //
    // TRACED (ReflectionMethod::SoftwareTraced, the permutation DESERT_REFLECTIONS_TRACED + the GI tracer's
    // defines, GlobalIllumination::TracerVariant): UE Lumen's reflections (LumenReflectionTracing.cpp
    // TraceReflections: screen, then mesh SDFs, then the global field; LumenReflections.cpp's MaxRoughnessToTrace)
    // over the SAME tracer the GI's screen probes walk - no second tracer. A ray the screen does not answer (it left
    // the screen, went behind the depth buffer's first layer, met the sky or a texel last frame did not see - UE's
    // vignette dither decides at the edge) continues from where the screen was certain through the GI ray contract
    // (Common/GITracer.glslh: for the software tracer the mesh SDFs of the view's near-field froxel grid, UE
    // LumenMeshSDFCulling, then the global field) to the trace distance - ONE such world ray per texel, drawn among
    // the rays the screen left in proportion to
    // their weight, answering for all of them (UE: one traced ray per texel); a hit returns its surface cache
    // cards' lighting, or is lit in place where no card
    // holds it (GIHitRadiance). A ray that meets nothing is NOT answered: the captures, then the sky, answer it in
    // the composite, as they answer a ScreenSpace miss (UE: the sky light of a Lumen miss; here the captures sit in
    // between). Past kTracedMaxRoughness (UE r.Lumen.Reflections.MaxRoughnessToTrace, faded over
    // kTracedRoughnessFade, Common/SSRGate.glslh TracedReflectionsAlpha) a ray only walks the screen, as
    // ScreenSpace does; past kSSRMaxRoughness nothing is traced. Under the traced GI the composite hands that
    // rough part of the lobe to the screen probe gather's rough specular by the same fade (UE
    // MixSpecularAndRoughReflections), so a rough surface reflects the GI's radiance, not the captures - and,
    // as UE's tile classification, such a texel is not traced at all (SSRGate.glslh TracedReflectionsPixelTraces,
    // SsrParams3.z): a tile of grass, earth and bark marks itself empty and costs no ray. Without traced GI the
    // captures answer the rough part and the screen walk past the max stays. The pass and the denoiser are the
    // ScreenSpace method's.
    // HardwareTraced is the same permutation under the ray-query tracer (DESERT_GI_TRACER_HARDWARE; UE
    // LumenReflectionHardwareRayTracing.usf): the world leg is one ray query over the view's TLAS (masked segments
    // alpha-tested, Common/RayQueryVisibility.glslh), no mesh SDF near field; the hit is its instance's surface
    // cache cards (GIHitInstanceRows: the hit's own row), the miss the same captures-then-sky.
    // HIT LIGHTING (Reflections.HitLighting, Cinematic; UE Lumen reflections with hit lighting) is the hardware
    // permutation with DESERT_REFLECTIONS_HIT_LIGHTING: the compute below classifies only, and the "HitLighting" pass
    // traces every texel from a ray generation stage through the ray tracing pipeline - the world leg runs each hit
    // surface template's own closest-hit shader (its material, Mesh/Surface/Pass_RayHit.glslh) and lights it in place.
    // TWO KERNELS for the traced compute (UE LumenReflectionTracing: the screen trace, then the world traces over
    // the compacted rays the screen left): the classify and the screen walk run the screen leg's permutation
    // (DESERT_REFLECTIONS_SCREEN_LEG: the traced gate and world-ray draw, no tracer, so the HZB walk keeps the
    // screen's registers and occupancy), which lists the texels that drew a world ray; the traced permutation's
    // compute is the world stage over that list (DESERT_REFLECTIONS_WORLD_STAGE). Same sums, same order: the same
    // output as one kernel doing both.
    // The texel trace itself is Common/SSRTrace.glslh, shared by both entry points.

    Compute
    {
        #include <Common/SSRTrace.glslh>

        #if !defined( DESERT_REFLECTIONS_TRACED )
        // --- THE TRACE (SsrParams3.w = 1, "SSR: Trace"): one thread per texel the classify listed, over the indirect
        // dispatch the classify sized (UE LumenReflectionTracing.usf over ReflectionTracingTileIndirectArgs). Every
        // lane of a wave walks a ray: none idles on a texel its tile does not own or a pixel too rough to trace.
        // The traced methods' screen leg (DESERT_REFLECTIONS_SCREEN_LEG) traces no world ray here: it writes the
        // texel as if its world ray missed and lists the texels that drew one for the world stage (UE: the screen
        // trace, then the world traces over the compacted rays the screen left) - this kernel carries no tracer,
        // so the HZB walk runs at the occupancy of the screen walk alone, and the world rays run on full waves.
        // ---
        void TraceListed()
        {
        	const uint index = gl_WorkGroupID.x * kSSRTraceListGroupSize + gl_LocalInvocationIndex;
        #if defined( DESERT_REFLECTIONS_SCREEN_LEG )
        	if (gl_LocalInvocationIndex == 0u)
        		s_ListCount = 0u;
        	barrier();
        	bool        toWorld = false;
        	SSRWorldRay world;
        #endif
        	if (index < ssrTraceListArgs.w)
        	{
        		const uint   listedTexel = ssrTraceList[index];
        		const ivec2  q     = ivec2(int(listedTexel & 0xFFFFu), int(listedTexel >> 16u));
        		const ivec2  full  = textureSize(GBufferNormalTexture, 0);
        		const ivec2  src   = SSRTraceSourcePixel(q, int(SsrParams3.x + 0.5), int(CameraPosition.w), full);
        		vec4         ray   = vec4(0.0, 0.0, 0.0, -126.0);
        		vec4         trace = vec4(0.0);
        		SSRTexel     t;
        		SSRTraceSums s;
        		if (SSRTraceTexel(src, full, t, s))
        		{
        			trace = SSRResolveTexel(t, s, ray);
        #if defined( DESERT_REFLECTIONS_SCREEN_LEG )
        			toWorld                 = s.worldWeight > 0.0;
        			world.RadianceHitWeight = vec4(s.radiance, s.hitWeight);
        			world.HitPointCoverage  = vec4(s.hitPoint, s.coverage);
        			world.WorldWeight       = s.worldWeight;
        			world.WeightSum         = s.weightSum;
        			world.WorldStart        = s.worldStart;
        			world.TexelRay          = uint(q.x) | (uint(q.y) << 14u) | (uint(s.worldRay) << 28u);
        #endif
        		}
        		imageStore(SsrTraceTexture, q, trace);
        		imageStore(TraceRayTexture, q, ray);
        	}
        #if defined( DESERT_REFLECTIONS_SCREEN_LEG )
        	// The classify's compaction: a groupshared count, one global atomic per workgroup, and the world
        	// stage's workgroup count raised to the end of this run (atomicMax), so its indirect arguments need no
        	// pass.
        	uint slot = 0u;
        	if (toWorld)
        		slot = atomicAdd(s_ListCount, 1u);
        	barrier();
        	if (gl_LocalInvocationIndex == 0u && s_ListCount > 0u)
        	{
        		s_ListStart = atomicAdd(ssrWorldListArgs.w, s_ListCount);
        		atomicMax(ssrWorldListArgs.x,
        		          (s_ListStart + s_ListCount + kSSRTraceListGroupSize - 1u) / kSSRTraceListGroupSize);
        	}
        	barrier();
        	if (toWorld)
        		ssrWorldList[s_ListStart + slot] = world;
        #endif
        }
        #endif

        #if defined( DESERT_REFLECTIONS_WORLD_STAGE )
        // --- THE WORLD STAGE (SsrParams3.w = 2, "SSR: World trace"): one thread per texel the screen leg listed,
        // over the indirect dispatch it sized - the texel's frame and its drawn ray recomputed from the texel
        // (SSRTexelSetup, SSRRayDirection: the screen leg's statements), the world ray traced, and on a hit the
        // texel rewritten from the screen's sums plus the world's answer. A miss leaves the texel as the screen
        // leg wrote it. The software tracer runs it as three legs over compacted lists (near field, far field, hit
        // lighting: Common/SSRTrace.glslh THE SOFTWARE TRACER'S WORLD LEGS), the hardware tracer as one ray query. ---

        // World list entry @p index rebuilt: its trace texel @p q, the texel's frame @p t and the screen's sums @p s
        // with the drawn world ray (s.worldL). False: the texel's frame cannot be rebuilt (nothing to trace).
        bool SSRWorldListedTexel(uint index, out ivec2 q, out SSRTexel t, out SSRTraceSums s)
        {
        	const SSRWorldRay listed = ssrWorldList[index];
        	const uint        packed = listed.TexelRay;
        	q                        = ivec2(int(packed & 0x3FFFu), int((packed >> 14u) & 0x3FFFu));
        	const ivec2       full   = textureSize(GBufferNormalTexture, 0);
        	const int         factor = int(SsrParams3.x + 0.5);
        	const ivec2       src    = SSRTraceSourcePixel(q, factor, int(CameraPosition.w), full);
        	if (!SSRTexelSetup(src, full, t))
        		return false;
        	s.radiance    = listed.RadianceHitWeight.rgb;
        	s.hitWeight   = listed.RadianceHitWeight.a;
        	s.coverage    = listed.HitPointCoverage.w;
        	s.weightSum   = listed.WeightSum;
        	s.hitPoint    = listed.HitPointCoverage.xyz;
        	s.worldWeight = listed.WorldWeight;
        	s.worldStart  = listed.WorldStart;
        	s.worldRay    = int(listed.TexelRay >> 28u);
        	vec3 Lt;
        	vec3 L;
        	SSRRayDirection(t, s.worldRay, int(SsrParams.x), uint(CameraPosition.w), Lt, L);
        	s.worldL = L;
        	// Only read when nothing hit, which a world hit rules out (its weight is > 0).
        	s.rayDir = Lt;
        	return true;
        }

        // The world ray's hit (radiance @p L_i at @p surface) added to the screen's sums and texel @p q rewritten.
        void SSRWorldWriteHit(ivec2 q, SSRTexel t, SSRTraceSums s, vec3 L_i, vec3 surface)
        {
        	SSRAddWorldHit(s, L_i, surface);
        	vec4       ray;
        	const vec4 trace = SSRResolveTexel(t, s, ray);
        	imageStore(SsrTraceTexture, q, trace);
        	imageStore(TraceRayTexture, q, ray);
        }

        #if defined( DESERT_GI_TRACER_SOFTWARE )
        // --- THE SOFTWARE TRACER'S WORLD LEGS (Common/SSRTrace.glslh SsrWorldFarListUB / SsrWorldHitListUB). Each
        // leg appends what it leaves to the next leg's list as the screen leg appends to the world list: a
        // groupshared count, one global atomic per workgroup, the next leg's workgroup count raised by atomicMax -
        // so no early return before the barriers. ---
        shared uint s_LegCount;
        shared uint s_LegStart;

        // Workgroup compaction into a leg list, first half: this lane's place among the workgroup's listed lanes
        // (s_LegCount their number). The caller then reserves the run in its list (its lane 0 adds s_LegCount to
        // its list's count into s_LegStart and raises the next leg's workgroups), barrier(), and writes at
        // s_LegStart + the place. Called from uniform control flow (barriers).
        uint SSRLegPlace(bool listed)
        {
        	if (gl_LocalInvocationIndex == 0u)
        		s_LegCount = 0u;
        	barrier();
        	uint place = 0u;
        	if (listed)
        		place = atomicAdd(s_LegCount, 1u);
        	barrier();
        	return place;
        }

        // --- NEAR FIELD (SsrParams3.w = 2, "SSR: World trace"): one thread per world list entry - the mesh SDFs of
        // the origin's froxel; a hit lit (GIHitRadiance) and written, a miss with a far-field span listed. ---
        void TraceWorldNearField()
        {
        	const uint index = gl_WorkGroupID.x * kSSRTraceListGroupSize + gl_LocalInvocationIndex;
        	bool           toFar = false;
        	SSRWorldFarRay far;
        	far.Listed = uvec4(0u);
        	far.Start  = vec4(0.0);
        	far.Dir    = vec4(0.0);
        	ivec2        q;
        	SSRTexel     t;
        	SSRTraceSums s;
        	if (index < ssrWorldListArgs.w && SSRWorldListedTexel(index, q, t, s) && s.worldWeight > 0.0)
        	{
        		const GITraceBias  bias    = GIRayBias(t.worldPos, t.N);
        		const float        farTMin = GIFarFieldTMin(bias, t.N, s.worldL);
        		GITraceInput       ray     = SSRWorldRayInput(t.worldPos, t.N, s.worldL, s.worldStart, bias.nearOffset,
        		                                              farTMin);
        		float              farStart;
        		const GITraceResult near = GITraceNearField(ray, farStart);
        		if (near.hit)
        			SSRWorldWriteHit(q, t, s, GIHitRadiance(near.surface, s.worldL), near.surface.position);
        		else
        		{
        			ray.tMin = farStart;
        			float globalStart;
        			float marchLength;
        			toFar      = GIFarFieldSpan(ray, globalStart, marchLength);
        			far.Listed = uvec4(index, floatBitsToUint(bias.nearOffset), floatBitsToUint(globalStart),
        			                   GITraceSeed(ray));
        			far.Start  = vec4(ray.origin + ray.direction * globalStart, marchLength);
        			far.Dir    = vec4(ray.direction, 0.0);
        		}
        	}
        	const uint place = SSRLegPlace(toFar);
        	if (gl_LocalInvocationIndex == 0u && s_LegCount > 0u)
        	{
        		s_LegStart = atomicAdd(ssrWorldFarArgs.w, s_LegCount);
        		atomicMax(ssrWorldFarArgs.x, (s_LegStart + s_LegCount + kSSRTraceListGroupSize - 1u) / kSSRTraceListGroupSize);
        	}
        	barrier();
        	if (toFar)
        		ssrWorldFar[s_LegStart + place] = far;
        }

        // --- FAR FIELD (SsrParams3.w = 3, "SSR: World far field"): one thread per far list entry - the global
        // field's march the near field listed (SSRWorldFarRay: its start, direction, length and dither seed), nothing
        // rebuilt; a hit listed for the hit lighting. ---
        void TraceWorldFarField()
        {
        	const uint index = gl_WorkGroupID.x * kSSRTraceListGroupSize + gl_LocalInvocationIndex;
        	bool  toHit = false;
        	uvec4 hit   = uvec4(0u);
        	if (index < ssrWorldFarArgs.w)
        	{
        		const SSRWorldFarRay         far   = ssrWorldFar[index];
        		const GlobalDistanceFieldHit march = GITraceFarFieldMarchFrom(far.Start.xyz, far.Dir.xyz, far.Start.w,
        		                                                              far.Listed.w, 0u);
        		toHit = march.Hit;
        		hit   = uvec4(far.Listed.x | (uint(clamp(march.Level + 1, 0, 15)) << 28u), far.Listed.y, far.Listed.z,
        		              floatBitsToUint(march.T));
        	}
        	const uint place = SSRLegPlace(toHit);
        	if (gl_LocalInvocationIndex == 0u && s_LegCount > 0u)
        	{
        		s_LegStart = atomicAdd(ssrWorldHitArgs.w, s_LegCount);
        		atomicMax(ssrWorldHitArgs.x, (s_LegStart + s_LegCount + kSSRTraceListGroupSize - 1u) / kSSRTraceListGroupSize);
        	}
        	barrier();
        	if (toHit)
        		ssrWorldHit[s_LegStart + place] = hit;
        }

        // --- HIT LIGHTING (SsrParams3.w = 4, "SSR: World hit lighting"): one thread per hit list entry - the far
        // field hit's surface rebuilt (GITraceFarField's GIFarFieldSurface of the same position and clipmap), lit by
        // GIHitRadiance and written. ---
        void LightWorldHits()
        {
        	const uint index = gl_WorkGroupID.x * kSSRTraceListGroupSize + gl_LocalInvocationIndex;
        	if (index >= ssrWorldHitArgs.w)
        		return;
        	const uvec4 hit = ssrWorldHit[index];
        	ivec2        q;
        	SSRTexel     t;
        	SSRTraceSums s;
        	if (!SSRWorldListedTexel(hit.x & 0x0FFFFFFFu, q, t, s))
        		return;
        	const float nearOffset  = uintBitsToFloat(hit.y);
        	const float globalStart = uintBitsToFloat(hit.z);
        	const float marchT      = uintBitsToFloat(hit.w);
        	const int   level       = int(hit.x >> 28u) - 1;
        	const vec3  origin      = t.worldPos + t.N * nearOffset;
        	const vec3  start       = origin + s.worldL * globalStart;
        	const GIHit surface =
        	     GIFarFieldSurface(start + s.worldL * marchT, s.worldL, globalStart + marchT, level);
        	SSRWorldWriteHit(q, t, s, GIHitRadiance(surface, s.worldL), surface.position);
        }
        #else
        // --- THE WHOLE WORLD RAY (SsrParams3.w = 2, "SSR: World trace"; the hardware tracer: one ray query, no legs):
        // one thread per world list entry, written on a hit. ---
        void TraceWorldListed()
        {
        	const uint index = gl_WorkGroupID.x * kSSRTraceListGroupSize + gl_LocalInvocationIndex;
        	if (index >= ssrWorldListArgs.w)
        		return;
        	ivec2        q;
        	SSRTexel     t;
        	SSRTraceSums s;
        	if (!SSRWorldListedTexel(index, q, t, s) || !SSRAddWorldRay(t, s))
        		return;
        	vec4       ray;
        	const vec4 trace = SSRResolveTexel(t, s, ray);
        	imageStore(SsrTraceTexture, q, trace);
        	imageStore(TraceRayTexture, q, ray);
        }
        #endif
        #endif

        #if !defined( DESERT_REFLECTIONS_WORLD_STAGE )
        // --- THE CLASSIFY (SsrParams3.w = 0, "SSR: Classify"): one workgroup per screen tile. ---
        void ClassifyTile()
        {
        	if (gl_LocalInvocationIndex == 0u)
        	{
        		s_TileReflects = 0u;
        #if !defined( DESERT_REFLECTIONS_HIT_LIGHTING )
        		s_ListCount = 0u;
        #endif
        	}
        	barrier();

        	// --- Classify: this tile spans [floor(t * scale), ceil((t + 1) * scale)) texels, up to 9, hence up
        	// to 2x2 texels per thread; the verdict is reduced in shared memory. ---
        	ivec2 full  = textureSize(GBufferNormalTexture, 0);
        	ivec2 grid  = imageSize(TileMaskTexture);
        	ivec2 tile  = ivec2(gl_WorkGroupID.xy);
        	vec2  scale = vec2(full) / vec2(grid);
        	ivec2 p0    = ivec2(floor(vec2(tile) * scale));
        	ivec2 p1    = min(ivec2(ceil(vec2(tile + 1) * scale)), full);

        	bool reflects = false;
        	for (int oy = 0; oy < 2; oy++)
        		for (int ox = 0; ox < 2; ox++)
        		{
        			ivec2 p = p0 + ivec2(gl_LocalInvocationID.xy) + ivec2(ox, oy) * 8;
        			if (p.x < p1.x && p.y < p1.y && SSRTexelTraces(SSRSurfaceNormal(p)))
        				reflects = true;
        		}
        	if (reflects)
        		atomicOr(s_TileReflects, 1u);
        	barrier();

        	const bool tileReflects = s_TileReflects != 0u;
        	if (gl_LocalInvocationIndex == 0u)
        		imageStore(TileMaskTexture, tile, vec4(tileReflects ? 1.0 : 0.0));
        #if defined( DESERT_REFLECTIONS_HIT_LIGHTING )
        	// Hit lighting: the "HitLighting" ray generation traces every texel after this classify.
        	return;
        #else

        	// --- List: the trace texels whose source pixel THIS tile owns. A pixel's owner is the tile whose quad
        	// rasterizes it (floor((x + 0.5) / scale)); the stretched ranges above overlap by a pixel, ownership does
        	// not, so every trace texel is written by exactly one thread. The tile owns at most 8 pixels per axis
        	// (scale <= 8), starting at `first` (p0, or p0 + 1 when p0 is the previous tile's); 8 texels per axis
        	// from (first - (factor - 1)) / factor reach all of them at either factor (factor 1: first .. first + 7;
        	// factor 2: source pixels from below first to at least first + 13, the rotation included). ---
        	ivec2 traceSize = imageSize(SsrTraceTexture);
        	int   frame     = int(CameraPosition.w);
        	int   factor    = int(SsrParams3.x + 0.5);
        	ivec2 first     = p0 + ivec2(notEqual(ivec2(floor((vec2(p0) + 0.5) / scale)), tile));
        	ivec2 q         = max((first - (factor - 1)) / factor, ivec2(0)) + ivec2(gl_LocalInvocationID.xy);
        	ivec2 src       = SSRTraceSourcePixel(q, factor, frame, full);
        	const bool owns = q.x < traceSize.x && q.y < traceSize.y && ivec2(floor((vec2(src) + 0.5) / scale)) == tile;

        	// Listed when the tile reflects and the texel's own pixel passes the trace's gate (the one TraceFrom asks
        	// first, SSRTexelTraces); any other texel the tile owns is written here as the trace writes a texel that
        	// reflects nothing, so every trace texel is still written every frame and the target needs no clear.
        	const bool listed = owns && tileReflects && SSRTexelTraces(SSRSurfaceNormal(src));
        	if (owns && !listed)
        	{
        		imageStore(SsrTraceTexture, q, vec4(0.0));
        		imageStore(TraceRayTexture, q, vec4(0.0, 0.0, 0.0, -126.0));
        	}
        	// UE's per-tile compaction: a groupshared count, one global atomic per tile, and the trace's workgroup
        	// count raised to the end of this tile's run (atomicMax), so the indirect arguments need no pass.
        	uint slot = 0u;
        	if (listed)
        		slot = atomicAdd(s_ListCount, 1u);
        	barrier();
        	if (gl_LocalInvocationIndex == 0u && s_ListCount > 0u)
        	{
        		s_ListStart = atomicAdd(ssrTraceListArgs.w, s_ListCount);
        		atomicMax(ssrTraceListArgs.x,
        		          (s_ListStart + s_ListCount + kSSRTraceListGroupSize - 1u) / kSSRTraceListGroupSize);
        	}
        	barrier();
        	if (listed)
        		ssrTraceList[s_ListStart + slot] = uint(q.x) | (uint(q.y) << 16u);
        #endif
        }

        #endif

        void main()
        {
        #if defined( DESERT_REFLECTIONS_WORLD_STAGE ) && defined( DESERT_GI_TRACER_SOFTWARE )
        	// The legs by SsrParams3.w (SSRRenderer::TraceStage World / WorldFarField / WorldHitLighting): uniform
        	// across the dispatch, so the legs' barriers are in uniform control flow.
        	if (SsrParams3.w >= 3.5)
        		LightWorldHits();
        	else if (SsrParams3.w >= 2.5)
        		TraceWorldFarField();
        	else
        		TraceWorldNearField();
        #elif defined( DESERT_REFLECTIONS_WORLD_STAGE )
        	TraceWorldListed();
        #else
        #if !defined( DESERT_REFLECTIONS_TRACED )
        	if (SsrParams3.w >= 0.5)
        	{
        		TraceListed();
        		return;
        	}
        #endif
        	ClassifyTile();
        #endif
        }
    }

    // THE HIT-LIT TRACE (Reflections.HitLighting on the hardware tracer; System::SSRRenderer::EnsureHitLightingProgram):
    // the same texel trace as the compute above, launched over the trace (its resolution) by the ray tracing pipeline
    // (Graphic/RayTracingPipeline.hpp) after the compute's classify. One thread per trace texel; a texel whose source
    // pixel's tile does not reflect writes 0, exactly as the compute writes it. The miss stage only says "missed": the
    // sky and the captures answer a miss in the composite, as for every traced ray.
    Pass "HitLighting"
    {
        RayGen
        {
            #include <Common/SSRTrace.glslh> // its CameraUB is also the view block the hit groups read

            void main()
            {
            	const ivec2 q         = ivec2(gl_LaunchIDEXT.xy);
            	const ivec2 traceSize = imageSize(SsrTraceTexture);
            	if (q.x >= traceSize.x || q.y >= traceSize.y)
            		return;
            	const ivec2 full  = textureSize(GBufferNormalTexture, 0);
            	const ivec2 grid  = imageSize(TileMaskTexture);
            	const vec2  scale = vec2(full) / vec2(grid);
            	const int   frame = int(CameraPosition.w);
            	const ivec2 src   = SSRTraceSourcePixel(q, int(SsrParams3.x + 0.5), frame, full);
            	const ivec2 tile  = min(ivec2(floor((vec2(src) + 0.5) / scale)), grid - 1);
            	vec4 ray   = vec4(0.0, 0.0, 0.0, -126.0);
            	vec4 trace = imageLoad(TileMaskTexture, tile).r > 0.5 ? TraceFrom(src, full, ray) : vec4(0.0);
            	imageStore(SsrTraceTexture, q, trace);
            	imageStore(TraceRayTexture, q, ray);
            }
        }

        Miss
        {
            #include <Common/RayTracingPayload.glslh>

            layout(location = 0) rayPayloadInEXT DesertRayPayload payload;

            void main()
            {
            	payload.HitT = -1.0;
            }
        }
    }
}
