// DesertAsset {"Kind":"Shader","Guid":"e305f457201c42f5934f24b4af3ee8f2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSDHistoryConvolution"
{
    // The screen-space denoiser's HISTORY CONVOLUTION of the reflections (UE SSDSpatialAccumulation.usf
    // STAGE_POST_FILTERING): a bilateral spatial filter over this frame's accumulation whose radius shrinks as the
    // pixel's history grows - a freshly disoccluded pixel (one frame, the reconstruction's noise) is blurred over
    // a few pixels, a converged one is left alone. The radius is kMaxRadius / sqrt(sample count),
    // further bounded by the specular lobe's footprint (a sharp reflection is not blurred wider than its
    // lobe). 3x3 taps that far apart, each weighed by the bilateral weight of its surface - distance off the
    // tangent plane (UE POSITION_BASED), normal agreement (UE BILATERAL_NORMAL), and the lobes'
    // agreement (UE BILATERAL_TOKOYASHI) with Karis' inverse luminance. This is the denoiser's output; the history
    // the next frame reads is the accumulation before it (the filter does not compound over frames).
    // Writes STRAIGHT (rgb, a): what the lighting composes (DeferredLighting.shader u_SSR / u_GI).

    Compute
    {
        #include <Common/ScreenSpaceDenoise.glslh>

        Uniform(1) sampler2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(2) sampler2D u_GBufferDepth;    // device depth (nearest)
        Uniform(3) sampler2D u_History;         // this frame's accumulation, premultiplied
        Uniform(4) sampler2D u_HistoryCount;    // its sample count
        layout(binding = 6, rgba16f) writeonly uniform image2D u_Output; // straight (rgb, a)

        // ScreenSpaceDenoiser::ViewUniforms (std140).
        Uniform(0) DenoiserViewUB
        {
        	mat4 u_ViewProjection;
        	mat4 u_PrevViewProjection;
        	mat4 u_InvJitteredViewProjection;
        };

        LocalSize(8, 8, 1);

        // The radius, in pixels, of a pixel holding one frame.
        const float kMaxRadius = 3.0;

        void main()
        {
        	const ivec2 viewSize = textureSize(u_GBufferNormal, 0);
        	const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= viewSize.x || pixel.y >= viewSize.y)
        		return;
        	if (!SSDPixelHasSignal(texelFetch(u_GBufferNormal, pixel, 0)))
        	{
        		imageStore(u_Output, pixel, vec4(0.0));
        		return;
        	}
        	const vec4  centre = texelFetch(u_History, pixel, 0);
        	const float count  = max(texelFetch(u_HistoryCount, pixel, 0).r, 1.0);

        	const vec2         size = vec2(viewSize);
        	const vec2         uv   = (vec2(pixel) + 0.5) / size;
        	const SSDSceneInfo ref  = SSDLoadSceneInfo(u_GBufferNormal, u_GBufferDepth, u_InvJitteredViewProjection,
        	                                           pixel, size);
        	const vec3  V           = SSDViewVector(uv, ref.Position, u_InvJitteredViewProjection);
        	const float pixelRadius = SSDPixelWorldRadius(uv, texelFetch(u_GBufferDepth, pixel, 0).r, ref.Position,
        	                                              1.0 / size, u_InvJitteredViewProjection);
        	const vec3  nearPoint = ReconstructWorldPosition(uv, 1.0, u_InvJitteredViewProjection);
        	const vec2  radii     = SSDLobePixelRadii(ref.Roughness, abs(dot(ref.Normal, V)), pixelRadius,
        	                                          distance(nearPoint, ref.Position));
        	const float radius    = min(kMaxRadius / sqrt(count), radii.y);
        	const vec4  lobe      = SSDRoughnessLobe(ref.Roughness, ref.Normal, V);

        	vec4 result = centre;
        	if (radius >= 0.75)
        	{
        		const float maxPlaneDistance = max(pixelRadius * radius * 2.0 * sqrt(2.0), 1e-3);
        		vec4  sum       = vec4(0.0); // premultiplied, Karis-weighted for reflections
        		float weightSum = 0.0;
        		for (int y = -1; y <= 1; ++y)
        			for (int x = -1; x <= 1; ++x)
        			{
        				const ivec2 p = clamp(pixel + ivec2(round(vec2(x, y) * radius)), ivec2(0), viewSize - 1);
        				const SSDSceneInfo other = SSDLoadSceneInfo(u_GBufferNormal, u_GBufferDepth,
        				                                            u_InvJitteredViewProjection, p, size);
        				if (dot(other.Normal, other.Normal) <= 0.0)
        					continue; // sky
        				float w = (2.0 - abs(float(x))) * (2.0 - abs(float(y))) *
        				          SSDPlaneWeight(ref, other.Position, maxPlaneDistance) *
        				          SSDNormalWeight(ref.Normal, other.Normal);
        				const vec4 s = texelFetch(u_History, p, 0);
        				const vec3 otherV = SSDViewVector((vec2(p) + 0.5) / size, other.Position,
        				                                  u_InvJitteredViewProjection);
        				w *= SSDLobeSimilarity(lobe, SSDRoughnessLobe(other.Roughness, other.Normal, otherV));
        				w *= SSDKarisWeight(s.a > 0.0 ? s.rgb / s.a : vec3(0.0));
        				sum       += s * w;
        				weightSum += w;
        			}
        		if (weightSum > 0.0)
        			result = sum / weightSum;
        	}
        	imageStore(u_Output, pixel, result.a > 0.0 ? vec4(result.rgb / result.a, result.a) : vec4(0.0));
        }
    }
}
