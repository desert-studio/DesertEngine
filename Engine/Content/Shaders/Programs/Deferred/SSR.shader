// DesertAsset {"Kind":"Shader","Guid":"3a2336ad2876438d915c69470c31e245","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSR"
{
    // Screen-space reflections: tile classification and the half-resolution trace, in ONE compute dispatch.
    // One workgroup per screen tile (Common/SSRTiles.glslh: grid = ceil(size / 8), tiles stretched over the
    // target). The workgroup first decides whether any pixel of its tile passes the trace's G-buffer gate
    // (Common/SSRGate.glslh) and writes that verdict to the tile mask the tiled resolve and composite read;
    // then it traces the half-resolution texels whose source pixel the tile owns - or writes 0 for them when
    // nothing in the tile can reflect. Every trace texel is written every frame, so the target needs no clear,
    // and the classify costs no pass of its own (it used to be a dispatch, and before that a render pass).
    //
    // The trace: for smooth opaque pixels, reflects the view ray off the surface, marches it through the
    // G-buffer in world space (projecting each step to screen) and samples the composited scene colour where it
    // hits. Output rgb = reflected colour, a = reflectance (Fresnel * smoothness * hit fade). Screen-space =>
    // reflections of off-screen / occluded geometry are missed (standard SSR limitation).
    // March: coarse pass with a slightly growing step over u_SSRParams.y world units, then a short binary
    // refinement between the last two points to pin the hit texel (kills the banding a coarse-only march has).
    // Compute has no derivatives, so every filtered read is textureLod(..., 0).

    Compute
    {
        #include <Common/SSRGate.glslh>

        Uniform(0) sampler2D u_GBufferAlbedo;   // rgb = albedo, a = metallic
        Uniform(1) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(2) sampler2D u_GBufferWorldPos; // rgb = world position
        Uniform(3) sampler2D u_SceneColor;      // composited opaque scene (reflection source)
        layout(binding = 4, rgba16f) writeonly uniform image2D u_Trace;    // half resolution
        layout(binding = 5, rgba8) writeonly uniform image2D u_TileMask;   // one texel per tile

        PushConstant PushConstants
        {
        	mat4 u_ViewProj;   // world -> clip (to project the marched ray to screen)
        	vec4 u_CameraPos;  // xyz = camera world position, w = per-frame seed (jitter + 2x2 rotation)
        	vec4 u_SSRParams;  // x = max steps, y = max ray distance (world), z = intensity, w = thickness (world)
        };

        LocalSize(8, 8, 1);

        shared uint s_TileReflects;

        // Per-pixel hash (same one SSAO uses) — jitters the ray start so the coarse march's banding turns into
        // fine noise, which the composite pass's blur then resolves into a smooth reflection.
        float hash12(vec2 p)
        {
        	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        	p3 += dot(p3, p3.yzx + 33.33);
        	return fract((p3.x + p3.y) * p3.z);
        }

        // World point -> screen UV. Returns uv in [0,1]; ok=false when behind the camera.
        bool projectToScreen(vec3 p, out vec2 uv)
        {
        	vec4 clip = u_ViewProj * vec4(p, 1.0);
        	if (clip.w <= 0.0)
        		return false;
        	uv = clip.xy / clip.w * 0.5 + 0.5;
        	uv.y = 1.0 - uv.y; // engine renders through a Y-flipped viewport (same convention as SSAO)
        	return true;
        }

        // Signed "ray point is this far behind the G-buffer surface at its texel" (camera-radial). Positive =
        // the ray crossed into geometry; sky texels report a huge negative (never a crossing).
        float depthDelta(vec3 p, vec2 uv)
        {
        	vec3 sN = textureLod(u_GBufferNormal, uv, 0.0).rgb;
        	if (dot(sN, sN) <= 0.001)
        		return -1e9; // sky — no occluder here
        	vec3 sPos = textureLod(u_GBufferWorldPos, uv, 0.0).rgb;
        	return distance(u_CameraPos.xyz, p) - distance(u_CameraPos.xyz, sPos);
        }

        // One ray from G-buffer pixel gPix. HALF RESOLUTION: each trace texel stands for a 2x2 block of G-buffer
        // pixels and traces from ONE of them, exactly (texelFetch - a filtered read would average normals and
        // positions across edges); which one rotates every frame, so the full-resolution temporal resolve sees
        // all four.
        vec4 TraceFrom(ivec2 gPix, ivec2 gSize)
        {
        	vec2 gUV = (vec2(gPix) + 0.5) / vec2(gSize);

        	vec4 gb = texelFetch(u_GBufferNormal, gPix, 0);
        	vec3 N  = gb.rgb;
        	if (dot(N, N) <= 0.001) return vec4(0.0); // sky / no geometry

        	float metallic   = texelFetch(u_GBufferAlbedo, gPix, 0).a;
        	float smoothFade = SSRSmoothFade(gb.a);
        	if (smoothFade < 0.01) return vec4(0.0);

        	N = normalize(N);
        	vec3 worldPos = texelFetch(u_GBufferWorldPos, gPix, 0).rgb;
        	vec3 V = normalize(u_CameraPos.xyz - worldPos);
        	vec3 R = reflect(-V, N);

        	// Schlick with a metalness-lerped F0: dielectrics reflect at grazing angles, metals everywhere.
        	float f0       = mix(0.04, 0.9, metallic);
        	float fresnel  = f0 + (1.0 - f0) * pow(1.0 - max(dot(N, V), 0.0), 5.0);
        	float strength = fresnel * smoothFade * u_SSRParams.z;
        	if (strength < 0.01) return vec4(0.0);

        	int   maxSteps  = int(u_SSRParams.x);
        	float maxDist   = u_SSRParams.y;
        	float thickness = u_SSRParams.w;

        	// Slightly growing step: fine contact reflections near the surface, long reach further out.
        	// Geometric series sized so all maxSteps steps sum EXACTLY to maxDist, last step = 8x the first.
        	float grow  = pow(8.0, 1.0 / float(maxSteps));
        	float step0 = maxDist * (grow - 1.0) / (pow(grow, float(maxSteps)) - 1.0);

        	vec3  hitColor = vec3(0.0);
        	float hit      = 0.0;
        	float stepLen  = step0;
        	float t        = 0.0;
        	// Start slightly off the surface to avoid self-hit. 2.0 is WORLD units, i.e. two CENTIMETRES:
        	// the metre-era 0.02 (= 2 cm then) survived the unit switch as 0.2 mm, far below the depth
        	// deltas a G-buffer texel carries at centimetre scale, so grazing rays began "inside" their own
        	// surface and were rejected by the very first sign-change test.
        	vec3  pPrev    = worldPos + N * 2.0;
        	// Jitter the start by a random fraction of the first step — DIFFERENT each frame (the seed in
        	// CameraPos.w) so the temporal accumulation averages a fresh estimate every frame and converges.
        	pPrev += R * ( step0 * hash12(gUV * 4096.0 + vec2(u_CameraPos.w)) );

        	// Crossing = the ray's depth delta changes SIGN between two samples (in front of the surface ->
        	// behind it). Detecting by sign change instead of a "within a thickness band" test is what removes
        	// the black march-quantization rings on curved reflectors: at grazing incidence the radial delta can
        	// jump PAST any band in one step, which used to discard the crossing and leave a dark ring.
        	float deltaPrev = -1.0;
        	for (int i = 0; i < maxSteps && t < maxDist; i++)
        	{
        		vec3 p = pPrev + R * stepLen;
        		t += stepLen;

        		vec2 suv;
        		if (!projectToScreen(p, suv)) break;
        		if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;

        		float delta = depthDelta(p, suv);
        		if (deltaPrev <= 0.0 && delta > 0.0)
        		{
        			// Binary refinement between pPrev (outside) and p (inside) — pins the exact crossing point.
        			vec3 lo = pPrev, hi = p;
        			for (int j = 0; j < 6; j++)
        			{
        				vec3 mid = 0.5 * (lo + hi);
        				vec2 muv;
        				if (!projectToScreen(mid, muv)) break;
        				if (depthDelta(mid, muv) > 0.0) hi = mid; else lo = mid;
        			}
        			vec3 hitP = 0.5 * (lo + hi);
        			vec2 huv;
        			if (!projectToScreen(hitP, huv)) break;

        			// A TRUE surface crossing refines to a point ~on the surface (tiny |delta|). A large refined
        			// delta means the ray slipped BEHIND an object's silhouette into the depth gap — that is not
        			// a hit: keep marching (it may legitimately strike something further along).
        			float refined = depthDelta(hitP, huv);
        			vec3  hN      = textureLod(u_GBufferNormal, huv, 0.0).rgb;
        			const bool backface = dot(hN, hN) > 0.001 && dot(normalize(hN), R) > 0.2;
        			if (abs(refined) < thickness && !backface)
        			{
        				hitColor = textureLod(u_SceneColor, huv, 0.0).rgb;
        				// Firefly clamp: reflections of blown-out HDR texels (sun-lit wall ~10x) otherwise
        				// produce single bright dots with contrast no spatial blur can hide.
        				float peak = max(hitColor.r, max(hitColor.g, hitColor.b));
        				if (peak > 3.0)
        					hitColor *= 3.0 / peak;
        				// Fade near the screen edge (no popping) and by ray travel (soft range limit).
        				vec2 edge = smoothstep(vec2(0.0), vec2(0.1), huv) * (1.0 - smoothstep(vec2(0.9), vec2(1.0), huv));
        				hit = edge.x * edge.y * (1.0 - smoothstep(0.7 * maxDist, maxDist, t));
        				break;
        			}
        			// else: silhouette gap / backface — fall through and continue the march.
        		}

        		deltaPrev = delta;
        		pPrev     = p;
        		stepLen  *= grow;
        	}

        	return vec4(hitColor, clamp(hit * strength, 0.0, 1.0));
        }

        void main()
        {
        	if (gl_LocalInvocationIndex == 0u)
        		s_TileReflects = 0u;
        	barrier();

        	// --- Classify: this tile spans [floor(t * scale), ceil((t + 1) * scale)) texels, up to 9, hence up
        	// to 2x2 texels per thread; the verdict is reduced in shared memory. ---
        	ivec2 full  = textureSize(u_GBufferNormal, 0);
        	ivec2 grid  = imageSize(u_TileMask);
        	ivec2 tile  = ivec2(gl_WorkGroupID.xy);
        	vec2  scale = vec2(full) / vec2(grid);
        	ivec2 p0    = ivec2(floor(vec2(tile) * scale));
        	ivec2 p1    = min(ivec2(ceil(vec2(tile + 1) * scale)), full);

        	bool reflects = false;
        	for (int oy = 0; oy < 2; oy++)
        		for (int ox = 0; ox < 2; ox++)
        		{
        			ivec2 p = p0 + ivec2(gl_LocalInvocationID.xy) + ivec2(ox, oy) * 8;
        			if (p.x < p1.x && p.y < p1.y && SSRPixelCanReflect(texelFetch(u_GBufferNormal, p, 0)))
        				reflects = true;
        		}
        	if (reflects)
        		atomicOr(s_TileReflects, 1u);
        	barrier();

        	const bool tileReflects = s_TileReflects != 0u;
        	if (gl_LocalInvocationIndex == 0u)
        		imageStore(u_TileMask, tile, vec4(tileReflects ? 1.0 : 0.0));

        	// --- Trace: the half-resolution texels whose source pixel THIS tile owns. A pixel's owner is the tile
        	// whose quad rasterizes it (floor((x + 0.5) / scale)); the stretched ranges above overlap by a pixel,
        	// ownership does not, so every trace texel is written by exactly one thread. 8 texels per axis from
        	// (p0 - 1) / 2 reach source pixels p0 - 2 .. p0 + 15, beyond the 9 a tile can own. ---
        	ivec2 traceSize = imageSize(u_Trace);
        	int   frame     = int(u_CameraPos.w);
        	ivec2 q         = max((p0 - 1) / 2, ivec2(0)) + ivec2(gl_LocalInvocationID.xy);
        	if (q.x >= traceSize.x || q.y >= traceSize.y)
        		return;
        	ivec2 src = min(q * 2 + ivec2(frame & 1, (frame >> 1) & 1), full - 1);
        	if (ivec2(floor((vec2(src) + 0.5) / scale)) != tile)
        		return;
        	imageStore(u_Trace, q, tileReflects ? TraceFrom(src, full) : vec4(0.0));
        }
    }
}
