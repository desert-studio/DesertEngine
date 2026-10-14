// DesertAsset {"Kind":"Shader","Guid":"3a2336ad2876438d915c69470c31e245","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSR"
{
    // Screen-space reflections: tile classification and the half-resolution trace, in ONE compute dispatch.
    // One workgroup per screen tile (Common/SSRTiles.glslh: grid = ceil(size / 8), tiles stretched over the
    // target). The workgroup first decides whether any pixel of its tile passes the trace's G-buffer gate
    // (Common/SSRGate.glslh) and writes that verdict to the tile mask the tiled resolve reads;
    // then it traces the half-resolution texels whose source pixel the tile owns - or writes 0 for them when
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

    Compute
    {
        #include <Common/SSRGate.glslh>
        #include <Common/ReconstructPosition.glslh>

        Uniform(1) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(2) sampler2D u_GBufferDepth;    // device depth (nearest): world position via SSRTraceUB's inverse
        Uniform(3) sampler2D u_SceneColor;      // LAST frame's lit opaque scene (SSRRenderer's scene colour history)
        layout(binding = 4, rgba16f) writeonly uniform image2D u_Trace;    // half resolution
        layout(binding = 5, rgba8) writeonly uniform image2D u_TileMask;   // one texel per tile
        Uniform(7) sampler2D u_HZB;             // closest-depth hierarchy of this frame's depth (HZB.shader)
        #include <Common/HZBTrace.glslh>

        // Filled on the CPU from the view's matrices once per view frame (SSRRenderer::UploadTraceUniforms).
        // u_InvJitteredViewProjection = inverse of the matrix the G-buffer depth was rasterised with
        // (Common/ReconstructPosition.glslh); u_JitteredViewProjection = that matrix (rays are projected with it,
        // so a ray and the depth it is tested against are in one space); u_PrevViewProjection = last frame's
        // (ViewFrame::PrevViewProjection), where a hit is looked up in u_SceneColor.
        Uniform(6) SSRTraceUB
        {
        	mat4 u_InvJitteredViewProjection;
        	mat4 u_JitteredViewProjection;
        	mat4 u_PrevViewProjection;
        };

        PushConstant PushConstants
        {
        	vec4 u_CameraPos;  // xyz = camera world position, w = per-frame seed (ray sequence + 2x2 rotation)
        	vec4 u_SSRParams;  // x = rays per texel, y = HZB iterations per ray, z = intensity, w = thickness (world)
        	vec4 u_SSRParams2; // x = max ray distance (world), y = 1 when u_SceneColor holds last frame, zw unused
        };

        LocalSize(8, 8, 1);

        shared uint s_TileReflects;

        const float kPi = 3.14159265358979;

        // Per-pixel hash: decorrelates neighbouring texels' ray sequences.
        float hash12(vec2 p)
        {
        	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        	p3 += dot(p3, p3.yzx + 33.33);
        	return fract((p3.x + p3.y) * p3.z);
        }

        // The R2 low-discrepancy sequence (Roberts 2018): sample n of a 2D sequence that fills the square evenly
        // for every prefix length, offset per pixel by a Cranley-Patterson rotation. Frames and rays share one index,
        // so the temporal resolve accumulates successive, well-spread lobe samples.
        vec2 RayRandom(vec2 rotation, uint index)
        {
        	return fract(rotation + vec2(0.7548776662, 0.5698402910) * float(index));
        }

        // Orthonormal basis around n (Duff et al. 2017, "Building an Orthonormal Basis, Revisited").
        void Basis(vec3 n, out vec3 t, out vec3 b)
        {
        	const float s = n.z >= 0.0 ? 1.0 : -1.0;
        	const float a = -1.0 / (s + n.z);
        	const float c = n.x * n.y * a;
        	t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
        	b = vec3(c, s + n.y * n.y * a, -n.y);
        }

        // Heitz 2018, "Sampling the GGX Distribution of Visible Normals" (UE ImportanceSampleVisibleGGX): a microfacet
        // normal in tangent space seen from ve (tangent space, z = the surface normal), alpha = roughness^2.
        vec3 SampleGGXVNDF(vec3 ve, float alpha, vec2 u)
        {
        	const vec3  vh    = normalize(vec3(alpha * ve.x, alpha * ve.y, ve.z));
        	const float lensq = vh.x * vh.x + vh.y * vh.y;
        	const vec3  t1    = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
        	const vec3  t2    = cross(vh, t1);
        	const float r     = sqrt(u.x);
        	const float phi   = 2.0 * kPi * u.y;
        	const float p1    = r * cos(phi);
        	const float s     = 0.5 * (1.0 + vh.z);
        	const float p2    = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * r * sin(phi);
        	const vec3  nh    = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
        	return normalize(vec3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
        }

        // Smith's masking term for GGX, G1(l) - the separable Smith G2 / G1(v) that is left of BRDF * cos / pdf once
        // the visible-normal pdf has cancelled D and G1(v) (Heitz 2018 eq. 19), Fresnel excluded (the split sum's).
        float SmithG1(float nDotL, float alpha)
        {
        	const float a2 = alpha * alpha;
        	return 2.0 * nDotL / (nDotL + sqrt(a2 + (1.0 - a2) * nDotL * nDotL));
        }

        // World point -> (uv, deviceZ) through the jittered matrix. False behind the camera.
        bool ProjectToScreen(mat4 viewProj, vec3 p, out vec3 screen)
        {
        	const vec4 clip = viewProj * vec4(p, 1.0);
        	if (clip.w <= 0.0)
        		return false;
        	screen = vec3(clip.xy / clip.w * 0.5 + 0.5, clip.z / clip.w);
        	screen.y = 1.0 - screen.y; // engine renders through a Y-flipped viewport (Common/ScreenUV.glslh)
        	return true;
        }

        // How much a walk that stopped at @p hit (uv, deviceZ) is a real reflection of the surface there, in [0, 1].
        // 0 off the screen and on sky texels; 0 where the ray passed BEHIND the surface by more than the thickness
        // (the depth buffer cannot say what is behind its first layer - the next source answers it); 0 on a face
        // the ray arrives at from behind; then UE's screen-edge vignette and a soft end at the max distance.
        float SSRHitConfidence(vec3 hit, vec3 origin, vec3 L, float thickness, float maxDistance)
        {
        	if (any(lessThan(hit.xy, vec2(0.0))) || any(greaterThan(hit.xy, vec2(1.0))))
        		return 0.0;
        	const ivec2 size  = textureSize(u_GBufferDepth, 0);
        	const ivec2 texel = clamp(ivec2(hit.xy * vec2(size)), ivec2(0), size - 1);
        	const vec3  hN    = texelFetch(u_GBufferNormal, texel, 0).rgb;
        	if (dot(hN, hN) <= 0.001)
        		return 0.0; // sky: the sky is the environment's, not a screen hit
        	if (dot(normalize(hN), L) > 0.0)
        		return 0.0; // back face
        	const vec3  surface = ReconstructWorldPosition(hit.xy, texelFetch(u_GBufferDepth, texel, 0).r,
        	                                               u_InvJitteredViewProjection);
        	const vec3  rayAt   = ReconstructWorldPosition(hit.xy, hit.z, u_InvJitteredViewProjection);
        	const float behind  = distance(surface, rayAt);
        	float confidence    = 1.0 - smoothstep(0.0, thickness, behind);
        	confidence *= confidence;
        	const float travel = distance(origin, surface);
        	return confidence * HZBScreenEdgeFade(hit.xy) * (1.0 - smoothstep(0.7 * maxDistance, maxDistance, travel));
        }

        // The traced texel gPix (HALF RESOLUTION: one G-buffer pixel of a 2x2 block, rotating every frame).
        // Returns rgb = the ratio-estimated radiance of the hits, a = coverage (see the header).
        vec4 TraceFrom(ivec2 gPix, ivec2 gSize)
        {
        	if (u_SSRParams2.y < 0.5)
        		return vec4(0.0); // no previous frame to reflect: all to the next source

        	const vec2 gUV = (vec2(gPix) + 0.5) / vec2(gSize);
        	const vec4 gb  = texelFetch(u_GBufferNormal, gPix, 0);
        	if (!SSRPixelCanReflect(gb))
        		return vec4(0.0);
        	const vec3  N         = normalize(gb.rgb);
        	const float roughness = gb.a;
        	const float alpha     = max(roughness * roughness, 0.001);

        	const vec3 worldPos = ReconstructWorldPosition(gUV, texelFetch(u_GBufferDepth, gPix, 0).r,
        	                                               u_InvJitteredViewProjection);
        	const vec3 V        = normalize(u_CameraPos.xyz - worldPos);
        	vec3       T, B;
        	Basis(N, T, B);
        	const vec3 Ve = vec3(dot(V, T), dot(V, B), dot(V, N));

        	// The ray leaves 2 cm off the surface (a world distance: centimetres) so a grazing ray does not meet its
        	// own texel's neighbours in the depth buffer.
        	const vec3 origin = worldPos + N * 2.0;
        	vec3       originScreen;
        	if (!ProjectToScreen(u_JitteredViewProjection, origin, originScreen))
        		return vec4(0.0);
        	const float originW = (u_JitteredViewProjection * vec4(origin, 1.0)).w;

        	const int   rays        = int(u_SSRParams.x);
        	const int   iterations  = int(u_SSRParams.y);
        	const float thickness   = u_SSRParams.w;
        	const float maxDistance = u_SSRParams2.x;
        	const vec2  rotation    = vec2(hash12(vec2(gPix)), hash12(vec2(gPix) + 17.17));
        	const uint  seed        = uint(u_CameraPos.w);

        	vec3  radiance  = vec3(0.0); // sum of w * conf * L, Karis-weighted
        	float hitWeight = 0.0;       // sum of w * conf, Karis-weighted (the radiance's normaliser)
        	float coverage  = 0.0;       // sum of w * conf
        	float weightSum = 0.0;       // sum of w over every ray
        	for (int r = 0; r < rays; ++r)
        	{
        		const vec3 H = SampleGGXVNDF(Ve, alpha, RayRandom(rotation, seed * uint(rays) + uint(r)));
        		vec3       Lt = reflect(-Ve, H);
        		if (Lt.z <= 0.0)
        			continue; // under the surface: a lobe sample that reflects nothing (weight 0 in the estimator)
        		const vec3  L = normalize(Lt.x * T + Lt.y * B + Lt.z * N);
        		const float w = SmithG1(Lt.z, alpha);
        		weightSum += w;

        		// The screen direction: the ray's end a short way along (a quarter of the origin's view depth, cut
        		// short of the camera plane), projected. The walk extends it to the screen edge.
        		const float dw     = (u_JitteredViewProjection * vec4(L, 0.0)).w;
        		float       rayLength = 0.25 * originW;
        		if (dw < 0.0)
        			rayLength = min(rayLength, 0.5 * originW / -dw);
        		vec3 endScreen;
        		if (!ProjectToScreen(u_JitteredViewProjection, origin + L * rayLength, endScreen))
        			continue;
        		const HZBTraceResult walk = HZBTraceScreen(originScreen, endScreen - originScreen, iterations);
        		if (walk.Exhausted)
        			continue;
        		const float confidence = SSRHitConfidence(walk.Position, origin, L, thickness, maxDistance);
        		if (confidence <= 0.0)
        			continue;

        		// The hit's radiance: where the surface point was on LAST frame's screen.
        		const ivec2 dSize  = textureSize(u_GBufferDepth, 0);
        		const ivec2 dTexel = clamp(ivec2(walk.Position.xy * vec2(dSize)), ivec2(0), dSize - 1);
        		const vec3  hitWorld =
        		     ReconstructWorldPosition(walk.Position.xy, texelFetch(u_GBufferDepth, dTexel, 0).r,
        		                              u_InvJitteredViewProjection);
        		vec3 prevScreen;
        		if (!ProjectToScreen(u_PrevViewProjection, hitWorld, prevScreen) ||
        		    any(lessThan(prevScreen.xy, vec2(0.0))) || any(greaterThan(prevScreen.xy, vec2(1.0))))
        			continue;
        		const vec3 L_i = textureLod(u_SceneColor, prevScreen.xy, 0.0).rgb;

        		// Karis' inverse-luminance weight inside the estimator: a single ray onto a sun-lit wall does not
        		// become a firefly the temporal resolve has to average away over many frames.
        		const float karis = 1.0 / (1.0 + dot(L_i, vec3(0.2126, 0.7152, 0.0722)));
        		radiance  += L_i * (w * confidence * karis);
        		hitWeight += w * confidence * karis;
        		coverage  += w * confidence;
        	}
        	if (weightSum <= 0.0 || hitWeight <= 0.0)
        		return vec4(0.0);

        	const float fade = SSRRoughnessFade(roughness) * u_SSRParams.z;
        	return vec4(radiance / hitWeight, clamp(coverage / weightSum * fade, 0.0, 1.0));
        }

        void main()
        {
        	if (gl_LocalInvocationIndex == 0u)
        	{
        		s_TileReflects = 0u;
        	}
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
