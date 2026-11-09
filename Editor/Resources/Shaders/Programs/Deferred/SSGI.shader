// DesertAsset {"Kind":"Shader","Guid":"6eb284793339484f878a1a9784b4e057","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSGI"
{
    // SCREEN-SPACE DIFFUSE INDIRECT LIGHTING, GIMethod::ScreenSpace (UE ScreenSpaceRayTracing.cpp
    // RenderScreenSpaceDiffuseIndirect, SSRT/SSRTDiffuseIndirect.usf): per pixel,
    // GlobalIllumination.ScreenSpaceRays cosine-distributed rays over the pixel's hemisphere (UE
    // CosineSampleHemisphereConcentric), each walked over the view's closest-depth HZB (Common/HZBTrace.glslh) and validated into a confidence (SSGIHitConfidence, the
    // diffuse form of Common/SSRTrace.glslh SSRHitConfidence). A hit returns the LIT SCENE OF THE PREVIOUS FRAME
    // where the hit point was on that frame's screen (the hit pixel minus its velocity): everything that lit that
    // surface - the sun, every local light with its shadow, the emissive, the sky, last frame's own indirect (so
    // bounces accumulate frame over frame) - comes back with it. Nothing here evaluates a light.
    //
    // Cosine-distributed rays make the estimator a plain mean: sum(L_i) / N is the incident radiance averaged
    // over the cosine lobe, E / pi - what the ambient multiplies by the albedo (Mesh/AmbientIBL.glslh
    // ComposeAmbient's `indirect`, DeferredLighting.shader GI mode 1). A ray that misses contributes nothing here: the sky it
    // would see is the ambient environment's, occluded by the GTAO term in the composite. UE's Karis weighting
    // (CONFIG_KARIS_WEIGHTING): each sample is weighed by 1 / (1 + luma), the mean un-weighed by 1 / (1 - luma),
    // so one ray onto a sun patch is not a firefly the denoiser averages away over many frames.
    //
    // Output (full resolution, ScreenSpaceDenoiser's diffuse-indirect signal, SceneRenderer::AddFrameSSGI):
    // rgb = the estimate in linear radiance (the history's pre-exposure divided out), a = 1 on every traced pixel
    // (0 on sky and where no previous frame exists). Compute has no derivatives, so every filtered read is
    // textureLod(..., 0).
    Compute
    {
        #include <Common/ReconstructPosition.glslh>
        #include <Common/MonteCarlo.glslh>

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(0) sampler PointClampSampler;
        Uniform(1) sampler LinearClampSampler;
        Uniform(2) texture2D GBufferNormalTexture;      // rgb = world normal, a = roughness
        #define GBufferNormalTexture sampler2D(GBufferNormalTexture, PointClampSampler)
        Uniform(3) texture2D GBufferDepthTexture;       // device depth (nearest)
        #define GBufferDepthTexture sampler2D(GBufferDepthTexture, PointClampSampler)
        Uniform(4) texture2D SceneColorHistoryTexture;  // LAST frame's lit opaque scene (SceneColorHistory)
        #define SceneColorHistoryTexture sampler2D(SceneColorHistoryTexture, LinearClampSampler)
        Uniform(5) texture2D HzbTexture;                // closest-depth hierarchy of this frame's depth (HZB.shader)
        #define HzbTexture sampler2D(HzbTexture, PointClampSampler)
        Uniform(6) texture2D VelocityTexture;           // NDC current - previous per pixel, unjittered
        #define VelocityTexture sampler2D(VelocityTexture, PointClampSampler)
        layout(binding = 7, rgba16f) writeonly uniform image2D SsgiSignalTexture; // kSSGISignal
        #include <Common/HZBTrace.glslh>

        // SSGIRenderer::Uniforms, from the ViewFrame and the scalability once per frame.
        Uniform(8) SSGIUB
        {
        	mat4 InvJitteredViewProjection; // the inverse of the matrix the G-buffer depth was rasterised with
        	mat4 JitteredViewProjection;    // that matrix: rays are projected with it, in the depth's own space
        	vec4 Trace;                     // x = rays per pixel, y = HZB iterations per ray, z = thickness (world,
        	                                //     cm), w = this frame's seed (ray sequence)
        	vec4 History;                   // x = 1 when SceneColorHistoryTexture holds last frame, y = 1 /
        	                                //     PrevPreExposure (the history's scale), zw unused
        };

        LocalSize(8, 8, 1);

        // How far past the ray's projected end the screen segment is extended: the walk clips it to the screen box
        // (HZBTraceScreen), so a diffuse ray runs to the screen's edge or the depth range's (UE's diffuse
        // InitScreenSpaceRay).
        const float kScreenSegmentExtent = 1000.0;

        // UE MonteCarlo.ush UniformSampleDiskConcentric + CosineSampleHemisphereConcentric (Shirley-Chiu): the
        // concentric map keeps the R2 sequence's even spread on the disk, lifted onto the hemisphere (z = cos).
        vec3 CosineSampleHemisphereConcentric(vec2 e)
        {
        	const vec2 p = 2.0 * e - 1.0;
        	if (p.x == 0.0 && p.y == 0.0)
        		return vec3(0.0, 0.0, 1.0);
        	float r;
        	float theta;
        	if (abs(p.x) > abs(p.y))
        	{
        		r     = p.x;
        		theta = 0.25 * kPi * (p.y / p.x);
        	}
        	else
        	{
        		r     = p.y;
        		theta = 0.5 * kPi - 0.25 * kPi * (p.x / p.y);
        	}
        	const vec2 disk = r * vec2(cos(theta), sin(theta));
        	return vec3(disk, sqrt(max(0.0, 1.0 - dot(disk, disk))));
        }

        float Luma(vec3 c)
        {
        	return dot(c, vec3(0.2126, 0.7152, 0.0722));
        }

        // World point -> (uv, deviceZ) through the jittered matrix (SSRTrace.glslh ProjectToScreen). False behind
        // the camera.
        bool SSGIProjectToScreen(vec3 p, out vec3 screen)
        {
        	const vec4 clip = JitteredViewProjection * vec4(p, 1.0);
        	screen          = vec3(0.0);
        	if (clip.w <= 0.0)
        		return false;
        	screen   = vec3(clip.xy / clip.w * 0.5 + 0.5, clip.z / clip.w);
        	screen.y = 1.0 - screen.y; // the engine renders through a Y-flipped viewport (Common/ScreenUV.glslh)
        	return true;
        }

        // SSRTrace.glslh SSRHitConfidence without the distance fade (a diffuse ray has no max distance): how much
        // a walk that stopped at @p hit (uv, deviceZ) met a real surface, in [0, 1]. 0 off the screen and on sky
        // texels, 0 on a face the ray arrives at from behind, 0 where the ray passed BEHIND the surface by more
        // than the thickness; then UE's screen-edge vignette.
        float SSGIHitConfidence(vec3 hit, vec3 L, float thickness)
        {
        	if (any(lessThan(hit.xy, vec2(0.0))) || any(greaterThan(hit.xy, vec2(1.0))))
        		return 0.0;
        	const ivec2 size  = textureSize(GBufferDepthTexture, 0);
        	const ivec2 texel = clamp(ivec2(hit.xy * vec2(size)), ivec2(0), size - 1);
        	const vec3  hN    = texelFetch(GBufferNormalTexture, texel, 0).rgb;
        	if (dot(hN, hN) <= 0.001)
        		return 0.0; // sky: the sky is the ambient environment's, not a screen hit
        	if (dot(normalize(hN), L) > 0.0)
        		return 0.0; // back face
        	const vec3  surface = ReconstructWorldPosition(hit.xy, texelFetch(GBufferDepthTexture, texel, 0).r,
        	                                               InvJitteredViewProjection);
        	const vec3  rayAt   = ReconstructWorldPosition(hit.xy, hit.z, InvJitteredViewProjection);
        	const float behind  = distance(surface, rayAt);
        	float confidence    = 1.0 - smoothstep(0.0, thickness, behind);
        	confidence *= confidence;
        	return confidence * HZBScreenEdgeFade(hit.xy);
        }

        void main()
        {
        	const ivec2 size  = textureSize(GBufferNormalTexture, 0);
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const vec4 gb = texelFetch(GBufferNormalTexture, pixel, 0);
        	if (dot(gb.rgb, gb.rgb) <= 0.001 || History.x < 0.5)
        	{
        		imageStore(SsgiSignalTexture, pixel, vec4(0.0)); // sky, or no previous frame to bounce
        		return;
        	}

        	const vec2 uv       = (vec2(pixel) + 0.5) / vec2(size);
        	const vec3 N        = normalize(gb.rgb);
        	const vec3 worldPos = ReconstructWorldPosition(uv, texelFetch(GBufferDepthTexture, pixel, 0).r,
        	                                               InvJitteredViewProjection);
        	vec3 T;
        	vec3 B;
        	Basis(N, T, B);

        	// The ray leaves 2 cm off the surface, as SSR's: a grazing ray does not meet its own texel's neighbours in
        	// the depth buffer.
        	const vec3 origin = worldPos + N * 2.0;
        	vec3       originScreen;
        	if (!SSGIProjectToScreen(origin, originScreen))
        	{
        		imageStore(SsgiSignalTexture, pixel, vec4(0.0));
        		return;
        	}
        	const float originW = (JitteredViewProjection * vec4(origin, 1.0)).w;

        	const int   rays       = max(int(Trace.x + 0.5), 1);
        	const int   iterations = int(Trace.y + 0.5);
        	const float thickness  = Trace.z;
        	const uint  seed       = uint(Trace.w + 0.5);
        	const vec2  rotation   = vec2(hash12(vec2(pixel)), hash12(vec2(pixel) + 17.17));
        	const ivec2 vSize      = textureSize(VelocityTexture, 0);

        	vec3 sum = vec3(0.0); // sum of confidence * karis * L over all rays
        	for (int r = 0; r < rays; ++r)
        	{
        		const vec3 Lt = CosineSampleHemisphereConcentric(RayRandom(rotation, seed * uint(rays) + uint(r)));
        		const vec3 L  = normalize(Lt.x * T + Lt.y * B + Lt.z * N);

        		// The screen direction, as SSR's: the ray's point a quarter of the origin's view depth along (cut
        		// short of the camera plane), projected; the segment then extends past it to the screen box.
        		const float dw        = (JitteredViewProjection * vec4(L, 0.0)).w;
        		float       rayLength = 0.25 * originW;
        		if (dw < 0.0)
        			rayLength = min(rayLength, 0.5 * originW / -dw);
        		vec3 alongScreen;
        		if (!SSGIProjectToScreen(origin + L * rayLength, alongScreen))
        			continue;
        		const vec3           endScreen = originScreen + (alongScreen - originScreen) * kScreenSegmentExtent;
        		const HZBTraceResult walk      = HZBTraceScreen(originScreen, endScreen, iterations);
        		if (!walk.Stopped)
        			continue; // ran out of iterations or left the screen in front of the depth: the sky's
        		const float confidence = SSGIHitConfidence(walk.Position, L, thickness);
        		if (confidence <= 0.0)
        			continue;

        		// Where the surface point was on LAST frame's screen: the hit pixel minus its velocity (uv = ndc *
        		// (0.5, -0.5) under the engine's flipped viewport). A moving object is read where it was: no trail.
        		const ivec2 vTexel = clamp(ivec2(walk.Position.xy * vec2(vSize)), ivec2(0), vSize - 1);
        		const vec2  prevUV = walk.Position.xy - texelFetch(VelocityTexture, vTexel, 0).xy * vec2(0.5, -0.5);
        		if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
        			continue;
        		const vec3 L_i = max(textureLod(SceneColorHistoryTexture, prevUV, 0.0).rgb * History.y, vec3(0.0));
        		sum += L_i * (confidence / (1.0 + Luma(L_i)));
        	}
        	vec3 mean = sum / float(rays);
        	mean /= max(1.0 - Luma(mean), 1e-3); // undo the Karis weighting on the mean (UE: rcp(1 - Luminance))
        	imageStore(SsgiSignalTexture, pixel, vec4(mean, 1.0));
        }
    }
}
