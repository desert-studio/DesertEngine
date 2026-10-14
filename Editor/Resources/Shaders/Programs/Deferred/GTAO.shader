// DesertAsset {"Kind":"Shader","Guid":"cff2976402954e26a914ee8a5f13a392","Versions":{"SHDR":1},"Dependencies":[]}
Shader "GTAO"
{
    // GROUND-TRUTH AMBIENT OCCLUSION with a BENT NORMAL (Jimenez et al. 2016, "Practical Real-Time Strategies for
    // Accurate Indirect Occlusion"; UE PostProcessAmbientOcclusion.usf GTAOCombinedPSandCS: horizon search
    // SearchForLargestAngleDual + ComputeInnerIntegral). Per pixel, Slices screen-space slices through the pixel; in
    // each, the two horizons are searched over Steps depth taps per side (falloff and UE's thickness heuristic), the
    // cosine-weighted visible arc between them is integrated against the normal projected into the slice, and the
    // arc's bisector is the slice's bent direction. The slice directions and the step offsets rotate every frame;
    // the screen-space denoiser (ScreenSpaceDenoiser::DenoiseAmbientOcclusion) accumulates them.
    //
    // The slice is built in WORLD space from the G-buffer depth (no view-space pass of its own): the view vector V,
    // and Perp - the world direction of the slice's screen axis made perpendicular to V. An angle t in the slice is the
    // direction V cos t - Perp sin t (UE's frame: Perp = cross(ViewDir, PlaneNormal)), so the +axis horizon is at -h.
    //
    // Output (ViewTargetFormats::kGTAO): rgb = the bent normal encoded 0.5 + 0.5 n (world), a = the visibility
    // (1 = unoccluded). The denoiser premultiplies rgb by a, so the denoised bent normal is visibility-weighted.

    Compute
    {
        #include <Common/ReconstructPosition.glslh>

        Uniform(1) sampler2D u_GBufferNormal; // rgb = world normal, a = roughness
        Uniform(2) sampler2D u_GBufferDepth;  // device depth (nearest)
        layout(binding = 3, rgba8) writeonly uniform image2D u_AO;

        // GTAORenderer::Uniforms, from the ViewFrame once per frame.
        Uniform(0) GTAOUB
        {
        	mat4 u_InvJitteredViewProjection; // the G-buffer depth's inverse matrix
        	mat4 u_JitteredViewProjection;    // ... and the matrix (the world radius projected to pixels)
        };

        PushConstant PushConstants
        {
        	vec4 u_CameraPos;  // xyz = camera world position, w = per-frame seed (slice rotation)
        	vec4 u_GTAOParams; // x = slices, y = steps per side, z = world radius, w = thickness blend
        };

        LocalSize(8, 8, 1);

        const float kPi     = 3.14159265358979;
        const float kHalfPi = 1.57079632679490;
        // UE GTAO_MAX_PIXEL_SCREEN_RADIUS: the search never spans more than this many pixels.
        const float kMaxPixelRadius = 256.0;

        // UE InterleavedGradientNoise (Jimenez 2014).
        float InterleavedGradientNoise(vec2 p)
        {
        	return fract(52.9829189 * fract(p.x * 0.06711056 + p.y * 0.00583715));
        }

        vec3 WorldAt(ivec2 texel, ivec2 size)
        {
        	const vec2 uv = (vec2(texel) + 0.5) / vec2(size);
        	return ReconstructWorldPosition(uv, texelFetch(u_GBufferDepth, texel, 0).r, u_InvJitteredViewProjection);
        }

        // UE SearchForLargestAngleDual: the cosines of the two horizons along +dir / -dir (pixels), as angles from V.
        vec2 SearchHorizons(ivec2 pixel, ivec2 size, vec2 dir, int steps, float stepRadius, float offset, vec3 P,
                            vec3 V, float attenFactor, float thickness)
        {
        	vec2 best = vec2(-1.0);
        	for (int i = 0; i < steps; ++i)
        	{
        		const float fi  = float(i);
        		const vec2  off = dir * max(stepRadius * (fi + offset), fi + 1.0);
        		for (int side = 0; side < 2; ++side)
        		{
        			const ivec2 t = ivec2(vec2(pixel) + (side == 0 ? off : -off));
        			if (t.x < 0 || t.y < 0 || t.x >= size.x || t.y >= size.y)
        				continue;
        			const vec3  D     = WorldAt(t, size) - P;
        			const float lenSq = dot(D, D);
        			float       ang   = dot(D, V) * inversesqrt(lenSq + 1e-4);
        			const float falloff = clamp(lenSq * attenFactor, 0.0, 1.0);
        			ang = mix(ang, best[side], falloff);
        			best[side] = ang > best[side] ? ang : mix(ang, best[side], thickness);
        		}
        	}
        	return acos(clamp(best, -1.0, 1.0));
        }

        void main()
        {
        	const ivec2 size  = textureSize(u_GBufferNormal, 0);
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const vec4 gb = texelFetch(u_GBufferNormal, pixel, 0);
        	if (dot(gb.rgb, gb.rgb) <= 0.001)
        	{
        		imageStore(u_AO, pixel, vec4(0.5, 0.5, 0.5, 1.0)); // sky: nothing to occlude
        		return;
        	}
        	const vec3 N = normalize(gb.rgb);
        	const vec3 P = WorldAt(pixel, size);
        	const vec3 V = normalize(u_CameraPos.xyz - P);

        	const int   slices      = max(int(u_GTAOParams.x), 1);
        	const int   steps       = max(int(u_GTAOParams.y), 1);
        	const float worldRadius = u_GTAOParams.z;
        	const float thickness   = u_GTAOParams.w;

        	// The world radius in pixels at this depth: a point the radius away across the view, projected.
        	const vec3 side   = normalize(cross(V, abs(V.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
        	const vec4 c0     = u_JitteredViewProjection * vec4(P, 1.0);
        	const vec4 c1     = u_JitteredViewProjection * vec4(P + side * worldRadius, 1.0);
        	const float pixelRadius = clamp(length((c1.xy / c1.w - c0.xy / c0.w) * 0.5 * vec2(size)), float(steps),
        	                                kMaxPixelRadius);
        	const float stepRadius  = pixelRadius / (float(steps) + 1.0);
        	const float attenFactor = 2.0 / (worldRadius * worldRadius);

        	// UE GetRandomVector: the slice rotation from the pixel's gradient noise, the step offset from a 4-pixel
        	// pattern; both advance every frame (golden-ratio steps) so the accumulated slices cover the circle.
        	const float frame  = u_CameraPos.w;
        	const float noise  = fract(InterleavedGradientNoise(vec2(pixel)) + frame * 0.618034);
        	float       offset = fract(0.25 * float((pixel.y - pixel.x) & 3) + frame * 0.381966);

        	float visibility = 0.0;
        	vec3  bent       = vec3(0.0);
        	for (int s = 0; s < slices; ++s)
        	{
        		const float phi = (float(s) + noise) * kPi / float(slices);
        		const vec2  dir = vec2(cos(phi), sin(phi)); // pixels, uv orientation (y down)

        		// The slice's screen axis as a world direction at this depth, made perpendicular to V: Perp.
        		const vec2  uvStep = (vec2(pixel) + 0.5 + dir) / vec2(size);
        		const vec3  D      = ReconstructWorldPosition(uvStep, texelFetch(u_GBufferDepth, pixel, 0).r,
        		                                              u_InvJitteredViewProjection) - P;
        		const vec3  perp   = normalize(D - V * dot(D, V));
        		const vec3  planeN = normalize(cross(perp, V));
        		const vec3  projN  = N - planeN * dot(N, planeN);
        		const float lenProj = length(projN) + 1e-6;

        		const float cosAng = clamp(dot(projN, perp) / lenProj, -1.0, 1.0);
        		const float gamma  = acos(cosAng) - kHalfPi;
        		const float cosG   = dot(projN, V) / lenProj;
        		const float sinG2  = cosAng * -2.0; // 2 sin(gamma)

        		const vec2 h = SearchHorizons(pixel, size, dir, steps, stepRadius, offset, P, V, attenFactor, thickness);
        		// Clamp to the normal's hemisphere (UE ComputeInnerIntegral); the +axis horizon is at -h.x.
        		const float h1 = gamma + max(-h.x - gamma, -kHalfPi);
        		const float h2 = gamma + min(h.y - gamma, kHalfPi);
        		visibility += lenProj * 0.25 *
        		              ((h1 * sinG2 + cosG - cos(2.0 * h1 - gamma)) + (h2 * sinG2 + cosG - cos(2.0 * h2 - gamma)));
        		// The slice's bent direction: the bisector of its visible arc.
        		const float beta = 0.5 * (h1 + h2);
        		bent += lenProj * (V * cos(beta) - perp * sin(beta));
        		offset = fract(offset + 0.617);
        	}
        	visibility = clamp(visibility / float(slices), 0.0, 1.0);
        	const vec3 bentN = dot(bent, bent) > 1e-8 ? normalize(bent) : N;
        	imageStore(u_AO, pixel, vec4(bentN * 0.5 + 0.5, visibility));
        }
    }
}
