// DesertAsset {"Kind":"Shader","Guid":"3a2336ad2876438d915c69470c31e245","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSR"
{
    // Screen-space reflections: tile classification and the trace (full or half resolution,
    // Reflections.DownsampleFactor), in ONE compute dispatch.
    // One workgroup per screen tile (grid = ceil(size / 8), tiles stretched over the
    // target). The workgroup first decides whether any pixel of its tile passes the trace's G-buffer gate
    // (Common/SSRGate.glslh) and writes that verdict to the tile mask the denoiser's reconstruction reads;
    // then it traces the trace texels whose source pixel the tile owns - or writes 0 for them when
    // nothing in the tile can reflect. Every trace texel is written every frame, so the target needs no clear,
    // and the classify costs no pass of its own (it used to be a dispatch, and before that a render pass).
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
    // MixSpecularAndRoughReflections), so a rough surface reflects the GI's radiance, not the captures; without
    // traced GI the captures answer it. The pass and the denoiser are the ScreenSpace method's.
    // HardwareTraced is the same permutation under the ray-query tracer (DESERT_GI_TRACER_HARDWARE; UE
    // LumenReflectionHardwareRayTracing.usf): the world leg is one ray query over the view's TLAS (masked segments
    // alpha-tested, Common/RayQueryVisibility.glslh), no mesh SDF near field; the hit is its instance's surface
    // cache cards (GIHitInstanceRows: the hit's own row), the miss the same captures-then-sky.
    // HIT LIGHTING (Reflections.HitLighting, Cinematic; UE Lumen reflections with hit lighting) is the hardware
    // permutation with DESERT_REFLECTIONS_HIT_LIGHTING: the compute below classifies only, and the "HitLighting" pass
    // traces every texel from a ray generation stage through the ray tracing pipeline - the world leg runs each hit
    // surface template's own closest-hit shader (its material, Mesh/Surface/Pass_RayHit.glslh) and lights it in place.
    // The texel trace itself is Common/SSRTrace.glslh, shared by both entry points.

    Compute
    {
        #include <Common/SSRTrace.glslh>

        void main()
        {
        	if (gl_LocalInvocationIndex == 0u)
        	{
        		s_TileReflects = 0u;
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
        			if (p.x < p1.x && p.y < p1.y && SSRPixelCanReflect(SSRSurfaceNormal(p)))
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
        #endif

        	// --- Trace: the trace texels whose source pixel THIS tile owns. A pixel's owner is the tile whose quad
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

        	if (!owns)
        		return;
        	vec4 ray   = vec4(0.0, 0.0, 0.0, -126.0);
        	vec4 trace = tileReflects ? TraceFrom(src, full, ray) : vec4(0.0);
        	imageStore(SsrTraceTexture, q, trace);
        	imageStore(TraceRayTexture, q, ray);
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
            #include <Common/SSRTrace.glslh>
            #include <Common/RayTracingView.glslh> // the hit groups' view block (bound, read by the hits)

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
