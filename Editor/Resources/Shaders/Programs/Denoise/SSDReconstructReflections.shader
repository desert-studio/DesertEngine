// DesertAsset {"Kind":"Shader","Guid":"520f9f172ed14644afd05f29c91da769","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSDReconstructReflections"
{
    // The screen-space denoiser's RECONSTRUCTION of the reflections signal (UE ScreenSpaceDenoiser, reflections:
    // SSDSpatialAccumulation.usf STAGE_RECONSTRUCTION), at full resolution from SSR's half-resolution trace.
    // Each of the 5x5 trace texels around the pixel is one sample of THIS pixel's lobe - the RATIO ESTIMATOR
    // (Stachowiak 2015, "Stochastic Screen-Space Reflections"; UE SSRT): the texel lends its HIT POINT and the
    // log2 pdf it drew its ray with (u_TraceRay); the direction is re-aimed from THIS pixel at that point, and the
    // sample weighs this pixel's BRDF * cos over the neighbour's pdf (Fresnel excluded: the lighting's split sum
    // multiplies it). A mirror's BRDF is ~0 away from its own direction, so it borrows nothing and stays sharp.
    // Each sample is further weighed by
    //  - the BILATERAL weight of the surface the texel traced from (its source pixel: the 2x2 block pixel the
    //    trace rotates through, SSR.shader): its distance off this pixel's tangent plane (UE POSITION_BASED) and
    //    the agreement of the two specular lobes (UE BILATERAL_TOKOYASHI) - a texel traced from another surface
    //    (the object in front, the wall behind) answers for a different lobe and lends neither radiance nor misses;
    //  - a gaussian of the screen distance from this pixel to that source pixel (sigma kSpatialSigma pixels), so
    //    the four pixels of a block do not read one identical set (the 2x2 blocks of a plain half-res upsample),
    //    yet wide enough that every texel of the set counts (a narrow one leaves one or two samples: grain).
    //    5x5 texels = UE's reconstruction footprint of 8-16 samples over the lobe; the BRDF keeps a mirror sharp.
    // rgb = sum(w * a * k * L) / sum(w * a * k) (k = Karis' inverse luminance), a = sum(w * a) / sum(w); written
    // premultiplied to u_Signal. u_HitDistance = sum(w * a * |hit - P|) / sum(w * a): what the temporal pass
    // reprojects the reflection through (the reflected point).
    // Tiles the trace marked as reflecting nothing (u_TileMask) write 0 and stop.

    Compute
    {
        #include <Common/ScreenSpaceDenoise.glslh>

        Uniform(1) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(2) sampler2D u_GBufferDepth;    // device depth (nearest)
        Uniform(3) sampler2D u_Trace;           // half resolution: rgb = hit radiance, a = coverage (SSR.shader)
        Uniform(4) sampler2D u_TraceRay;        // half resolution: xyz = hit point, w = log2 pdf (SSR.shader)
        Uniform(5) sampler2D u_TileMask;        // one texel per SSR tile, 1 = something in it reflects
        layout(binding = 6, rgba16f) writeonly uniform image2D u_Signal;     // premultiplied (rgb * a, a)
        layout(binding = 7, r32f) writeonly uniform image2D u_HitDistance;   // world distance to what is reflected

        // ScreenSpaceDenoiser::ViewUniforms (std140).
        Uniform(0) DenoiserViewUB
        {
        	mat4 u_ViewProjection;
        	mat4 u_PrevViewProjection;
        	mat4 u_InvJitteredViewProjection;
        };

        PushConstant PushConstants
        {
        	vec4 u_Params; // x = the trace's frame seed (its 2x2 block rotation), yzw unused
        };

        LocalSize(8, 8, 1);

        // The screen-distance gaussian's sigma, in pixels.
        const float kSpatialSigma = 2.5;

        float SmithG1(float nDotX, float a2)
        {
        	return 2.0 * nDotX / (nDotX + sqrt(a2 + (1.0 - a2) * nDotX * nDotX));
        }

        void main()
        {
        	const ivec2 viewSize = textureSize(u_GBufferNormal, 0);
        	const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= viewSize.x || pixel.y >= viewSize.y)
        		return;
        	const ivec2 grid  = textureSize(u_TileMask, 0);
        	const ivec2 tile  = min(ivec2(floor((vec2(pixel) + 0.5) * vec2(grid) / vec2(viewSize))), grid - 1);
        	const vec4  gb    = texelFetch(u_GBufferNormal, pixel, 0);
        	if (texelFetch(u_TileMask, tile, 0).r < 0.5 || !SSDPixelHasSignal(kSSDSignalReflections, gb))
        	{
        		imageStore(u_Signal, pixel, vec4(0.0));
        		imageStore(u_HitDistance, pixel, vec4(0.0));
        		return;
        	}
        	const vec2         size  = vec2(viewSize);
        	const vec2         uv    = (vec2(pixel) + 0.5) / size;
        	const SSDSceneInfo ref   = SSDLoadSceneInfo(u_GBufferNormal, u_GBufferDepth, u_InvJitteredViewProjection,
        	                                            pixel, size);
        	const vec3         V     = SSDViewVector(uv, ref.Position, u_InvJitteredViewProjection);
        	const vec3         N     = ref.Normal;
        	const float        alpha = max(ref.Roughness * ref.Roughness, 0.001);
        	const float        a2    = alpha * alpha;
        	const float        nDotV = max(dot(N, V), 1e-4);
        	const float        g1V   = SmithG1(nDotV, a2);
        	const vec4         lobe  = SSDRoughnessLobe(ref.Roughness, N, V);
        	// A neighbour texel's source pixel is up to ~6 pixels away; the plane tolerates that many pixel radii.
        	const float pixelRadius = SSDPixelWorldRadius(uv, texelFetch(u_GBufferDepth, pixel, 0).r, ref.Position,
        	                                              1.0 / size, u_InvJitteredViewProjection);
        	const float maxPlaneDistance = max(pixelRadius * 6.0, 1e-3);

        	const ivec2 traceSize = textureSize(u_Trace, 0);
        	const ivec2 centre    = clamp(pixel / 2, ivec2(0), traceSize - 1);
        	const int   frame     = int(u_Params.x);
        	const ivec2 rotation  = ivec2(frame & 1, (frame >> 1) & 1);
        	vec3  radiance    = vec3(0.0);
        	float hitWeight   = 0.0;
        	float coverage    = 0.0;
        	float weightSum   = 0.0;
        	float hitDistance = 0.0;
        	for (int y = -2; y <= 2; ++y)
        		for (int x = -2; x <= 2; ++x)
        		{
        			const ivec2 t   = clamp(centre + ivec2(x, y), ivec2(0), traceSize - 1);
        			const vec4  ray = texelFetch(u_TraceRay, t, 0);
        			if (ray.w <= -125.0)
        				continue; // the texel drew no ray (it cannot reflect, or its trace was lost)
        			// The surface the texel traced from (SSR.shader: q * 2 + the frame's block rotation).
        			const ivec2        src   = min(t * 2 + rotation, viewSize - 1);
        			const SSDSceneInfo other = SSDLoadSceneInfo(u_GBufferNormal, u_GBufferDepth,
        			                                            u_InvJitteredViewProjection, src, size);
        			const vec3  otherV    = SSDViewVector((vec2(src) + 0.5) / size, other.Position,
        			                                      u_InvJitteredViewProjection);
        			const float bilateral = SSDPlaneWeight(ref, other.Position, maxPlaneDistance) *
        			                        SSDLobeSimilarity(lobe, SSDRoughnessLobe(other.Roughness, other.Normal,
        			                                                                 otherV));
        			const vec2  offset    = vec2(src - pixel);
        			const float spatial   = exp(-dot(offset, offset) * (1.0 / (2.0 * kSpatialSigma * kSpatialSigma)));
        			if (bilateral * spatial <= 0.0)
        				continue;
        			// Re-aimed from THIS pixel at the neighbour's hit point (UE SSRT).
        			const vec3  toHit = ray.xyz - ref.Position;
        			const float dist  = length(toHit);
        			if (dist <= 0.0)
        				continue;
        			const vec3  L     = toHit / dist;
        			const float nDotL = dot(N, L);
        			if (nDotL <= 0.0)
        				continue; // below this pixel's horizon: outside its lobe
        			const vec3  H     = normalize(V + L);
        			const float nDotH = max(dot(N, H), 0.0);
        			const float d     = nDotH * nDotH * (a2 - 1.0) + 1.0;
        			const float D     = a2 / (kSSDPi * d * d);
        			// BRDF * cos without Fresnel: D * G1(V) * G1(L) / (4 N.V), over the drawing texel's pdf.
        			const float w = D * g1V * SmithG1(nDotL, a2) / (4.0 * nDotV) * exp2(-ray.w) * bilateral * spatial;
        			const vec4  trace = texelFetch(u_Trace, t, 0);
        			const float k     = SSDKarisWeight(trace.rgb);
        			weightSum   += w;
        			coverage    += w * trace.a;
        			hitDistance += w * trace.a * dist;
        			radiance    += trace.rgb * (w * trace.a * k);
        			hitWeight   += w * trace.a * k;
        		}
        	vec4  signal   = vec4(0.0);
        	float reflectedDistance = 0.0;
        	if (weightSum > 0.0 && hitWeight > 0.0)
        	{
        		const float a = coverage / weightSum;
        		signal        = vec4(radiance / hitWeight * a, a);
        		reflectedDistance = hitDistance / coverage;
        	}
        	imageStore(u_Signal, pixel, signal);
        	imageStore(u_HitDistance, pixel, vec4(reflectedDistance));
        }
    }
}
