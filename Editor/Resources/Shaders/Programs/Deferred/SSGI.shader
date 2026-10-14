// DesertAsset {"Kind":"Shader","Guid":"35c233e92ecb4ee3a9b0eed7561fc26e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSGI"
{
    // SCREEN-SPACE DIFFUSE INDIRECT LIGHTING (UE ScreenSpaceRayTracing.cpp RenderScreenSpaceDiffuseIndirect,
    // SSRT/SSRTDiffuseIndirect.usf): per pixel, Ray count cosine-distributed rays over the pixel's hemisphere
    // (UE ComputeL: CosineSampleHemisphereConcentric), each walked over the view's closest-depth HZB
    // (Common/HZBTrace.glslh) and validated into a confidence (Common/ScreenTrace.glslh ScreenHitConfidence). A hit
    // returns the LIT SCENE OF THE PREVIOUS FRAME where the hit point was on that frame's screen (the hit pixel minus
    // its velocity): everything that lit that surface - the sun, every point and spot light with its shadow, the
    // emissive, the sky, last frame's own indirect (so bounces accumulate frame over frame) - comes back with it.
    // Nothing here evaluates a light.
    //
    // Cosine-distributed rays make the estimator a plain mean: sum(L_i) / N is the incident radiance averaged over
    // the cosine lobe, E / pi - what the ambient multiplies by the albedo (Mesh/AmbientIBL.glslh ComposeAmbient's
    // `indirect`). A ray that misses contributes nothing here: the sky it would see is the ambient environment's,
    // occluded by the GTAO term. UE's Karis weighting (CONFIG_KARIS_WEIGHTING): each sample is weighed by
    // 1 / (1 + luma) and the mean un-weighed by 1 / (1 - luma), so one ray onto a sun patch is not a firefly the
    // denoiser averages away over many frames.
    //
    // Output (full resolution, ScreenSpaceDenoiser's diffuse-indirect signal): rgb = the estimate times the GI
    // intensity, a = 1 on every traced pixel (0 on sky and where no previous frame exists).
    // Compute has no derivatives, so every filtered read is textureLod(..., 0).

    Compute
    {
        #include <Common/ReconstructPosition.glslh>

        Uniform(1) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(2) sampler2D u_GBufferDepth;    // device depth (nearest)
        Uniform(3) sampler2D u_SceneColor;      // LAST frame's lit opaque scene (the view's scene colour history)
        layout(binding = 4, rgba16f) writeonly uniform image2D u_Signal; // ViewTargetFormats::kGIResolve
        Uniform(7) sampler2D u_HZB;             // closest-depth hierarchy of this frame's depth (HZB.shader)
        Uniform(8) sampler2D u_Velocity;        // NDC current - previous per pixel (View/Velocity.hpp), unjittered
        #include <Common/HZBTrace.glslh>

        // SSGIRenderer::TraceUniforms, from the ViewFrame once per frame: the inverse of the matrix the G-buffer depth
        // was rasterised with, and that matrix (rays are projected with it, in the depth's own space).
        Uniform(6) SSGITraceUB
        {
        	mat4 u_InvJitteredViewProjection;
        	mat4 u_JitteredViewProjection;
        };

        PushConstant PushConstants
        {
        	vec4 u_CameraPos;   // xyz = camera world position, w = per-frame seed (ray sequence)
        	vec4 u_SSGIParams;  // x = rays per pixel, y = HZB iterations per ray, z = GI intensity, w = thickness (world)
        	vec4 u_SSGIParams2; // x = 1 when u_SceneColor holds last frame, yzw unused
        };

        #include <Common/ScreenTrace.glslh>

        LocalSize(8, 8, 1);

        const float kPi = 3.14159265358979;

        // UE MonteCarlo.ush UniformSampleDiskConcentric + CosineSampleHemisphereConcentric (Shirley-Chiu): the
        // concentric map keeps the R2 sequence's even spread on the disk, lifted onto the hemisphere (z = cos).
        vec3 CosineSampleHemisphereConcentric(vec2 e)
        {
        	const vec2 p = 2.0 * e - 1.0;
        	if (p.x == 0.0 && p.y == 0.0)
        		return vec3(0.0, 0.0, 1.0);
        	float r, theta;
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

        void main()
        {
        	const ivec2 size  = textureSize(u_GBufferNormal, 0);
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const vec4 gb = texelFetch(u_GBufferNormal, pixel, 0);
        	if (dot(gb.rgb, gb.rgb) <= 0.001 || u_SSGIParams2.x < 0.5)
        	{
        		imageStore(u_Signal, pixel, vec4(0.0)); // sky, or no previous frame to bounce
        		return;
        	}

        	const vec2 uv       = (vec2(pixel) + 0.5) / vec2(size);
        	const vec3 N        = normalize(gb.rgb);
        	const vec3 worldPos = ReconstructWorldPosition(uv, texelFetch(u_GBufferDepth, pixel, 0).r,
        	                                               u_InvJitteredViewProjection);
        	vec3 T, B;
        	Basis(N, T, B);

        	// The ray leaves 2 cm off the surface (centimetres), as SSR's: a grazing ray does not meet its own
        	// texel's neighbours in the depth buffer.
        	const vec3 origin = worldPos + N * 2.0;
        	vec3       originScreen;
        	if (!ProjectToScreen(u_JitteredViewProjection, origin, originScreen))
        	{
        		imageStore(u_Signal, pixel, vec4(0.0));
        		return;
        	}
        	const float originW = (u_JitteredViewProjection * vec4(origin, 1.0)).w;

        	const int   rays       = int(u_SSGIParams.x);
        	const int   iterations = int(u_SSGIParams.y);
        	const float thickness  = u_SSGIParams.w;
        	const vec2  rotation   = vec2(hash12(vec2(pixel)), hash12(vec2(pixel) + 17.17));
        	const uint  seed       = uint(u_CameraPos.w);
        	const ivec2 vSize      = textureSize(u_Velocity, 0);

        	vec3 sum = vec3(0.0); // sum of confidence * karis * L over all rays
        	for (int r = 0; r < rays; ++r)
        	{
        		const vec3 Lt = CosineSampleHemisphereConcentric(RayRandom(rotation, seed * uint(rays) + uint(r)));
        		const vec3 L  = normalize(Lt.x * T + Lt.y * B + Lt.z * N);

        		// The screen direction, as SSR's: the ray's end a quarter of the origin's view depth along (cut short
        		// of the camera plane), projected; the walk extends it to the screen edge.
        		const float dw        = (u_JitteredViewProjection * vec4(L, 0.0)).w;
        		float       rayLength = 0.25 * originW;
        		if (dw < 0.0)
        			rayLength = min(rayLength, 0.5 * originW / -dw);
        		vec3 endScreen;
        		if (!ProjectToScreen(u_JitteredViewProjection, origin + L * rayLength, endScreen))
        			continue;
        		const HZBTraceResult walk = HZBTraceScreen(originScreen, endScreen - originScreen, iterations);
        		if (walk.Exhausted)
        			continue;
        		vec3 surface;
        		// No distance limit: a diffuse ray runs to the screen's edge (UE's diffuse InitScreenSpaceRay); the
        		// screen-edge vignette inside the confidence still hands the border over smoothly.
        		const float confidence = ScreenHitConfidence(walk.Position, origin, L, thickness, 3.0e38, surface);
        		if (confidence <= 0.0)
        			continue;

        		// Where the surface point was on LAST frame's screen: the hit pixel minus its velocity (uv = ndc *
        		// (0.5, -0.5) under the engine's flipped viewport). A moving object is read where it was: no trail.
        		const ivec2 vTexel = clamp(ivec2(walk.Position.xy * vec2(vSize)), ivec2(0), vSize - 1);
        		const vec2  prevUV = walk.Position.xy - texelFetch(u_Velocity, vTexel, 0).xy * vec2(0.5, -0.5);
        		if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
        			continue;
        		const vec3 L_i = max(textureLod(u_SceneColor, prevUV, 0.0).rgb, vec3(0.0));
        		sum += L_i * (confidence / (1.0 + Luma(L_i)));
        	}
        	vec3 mean = sum / float(max(rays, 1));
        	mean /= max(1.0 - Luma(mean), 1e-3); // undo the Karis weighting on the mean (UE: rcp(1 - Luminance))
        	imageStore(u_Signal, pixel, vec4(mean * u_SSGIParams.z, 1.0));
        }
    }
}
