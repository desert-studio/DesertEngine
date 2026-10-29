// DesertAsset {"Kind":"Shader","Guid":"e305f457201c42f5934f24b4af3ee8f2","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSDHistoryConvolution"
{
    // The screen-space denoiser's HISTORY CONVOLUTION, every signal (UE SSDSpatialAccumulation.usf
    // STAGE_POST_FILTERING): a bilateral spatial filter over this frame's accumulation whose radius shrinks as the
    // pixel's history grows - a freshly disoccluded pixel (one frame, the reconstruction's noise) is blurred over
    // a few pixels, a converged one is left alone. The radius is kMaxRadius / sqrt(sample count); for reflections
    // it is further bounded by the specular lobe's footprint (a sharp reflection is not blurred wider than its
    // lobe). 3x3 taps that far apart, each weighed by the bilateral weight of its surface - distance off the
    // tangent plane (UE POSITION_BASED), normal agreement (UE BILATERAL_NORMAL), and for reflections the lobes'
    // agreement (UE BILATERAL_TOKOYASHI) with Karis' inverse luminance. This is the denoiser's output; the history
    // the next frame reads is the accumulation before it (the filter does not compound over frames).
    // Writes STRAIGHT (rgb, a): what the lighting composes (DeferredLighting.shader SsrTexture / Gi).

    Compute
    {
        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound once by the pass by name and numbered by the program build (SharedSamplerBinding.hpp).
        // Declared before Common/ScreenSpaceDenoise.glslh, whose SSDLoadSceneInfo reads the G-buffer through PointClampSampler.
        Uniform sampler PointClampSampler;
        #include <Common/ScreenSpaceDenoise.glslh>

        Uniform(1) texture2D GBufferNormalTexture;   // rgb = world normal, a = roughness
        Uniform(2) texture2D GBufferDepthTexture;    // device depth (nearest)
        Uniform(3) texture2D AccumulatedSignalTexture;         // this frame's accumulation, premultiplied
        #define AccumulatedSignalTexture sampler2D(AccumulatedSignalTexture, PointClampSampler)
        Uniform(4) texture2D HistoryCountTexture;    // its sample count
        #define HistoryCountTexture sampler2D(HistoryCountTexture, PointClampSampler)
        layout(binding = 6, rgba16f) writeonly uniform image2D DenoisedSignalTexture; // straight (rgb, a)

        // ScreenSpaceDenoiser::ViewUniforms (std140).
        Uniform(0) DenoiserViewUB
        {
        	mat4 ViewProjection;
        	mat4 PreviousViewProjection;
        	mat4 InverseJitteredViewProjection;
        };

        PushConstant PushConstants
        {
        	vec4 Params; // y = signal (kSSDSignal*), xzw unused
        };

        LocalSize(8, 8, 1);

        // The radius, in pixels, of a pixel holding one frame.
        const float kMaxRadius = 3.0;

        void main()
        {
        	const ivec2 viewSize = textureSize(sampler2D(GBufferNormalTexture, PointClampSampler), 0);
        	const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= viewSize.x || pixel.y >= viewSize.y)
        		return;
        	const int signal = int(Params.y + 0.5);
        	if (!SSDPixelHasSignal(signal, texelFetch(sampler2D(GBufferNormalTexture, PointClampSampler), pixel, 0)))
        	{
        		imageStore(DenoisedSignalTexture, pixel, vec4(0.0));
        		return;
        	}
        	const vec4  centre = texelFetch(AccumulatedSignalTexture, pixel, 0);
        	const float count  = max(texelFetch(HistoryCountTexture, pixel, 0).r, 1.0);

        	const vec2         size = vec2(viewSize);
        	const vec2         uv   = (vec2(pixel) + 0.5) / size;
        	const SSDSceneInfo ref  = SSDLoadSceneInfo(GBufferNormalTexture, GBufferDepthTexture, InverseJitteredViewProjection,
        	                                           pixel, size);
        	const vec3  V           = SSDViewVector(uv, ref.Position, InverseJitteredViewProjection);
        	const float pixelRadius = SSDPixelWorldRadius(uv, texelFetch(sampler2D(GBufferDepthTexture, PointClampSampler), pixel, 0).r, ref.Position,
        	                                              1.0 / size, InverseJitteredViewProjection);
        	float radius = kMaxRadius / sqrt(count);
        	vec4  lobe   = vec4(0.0);
        	if (SSDSignalIsSpecular(signal))
        	{
        		const vec3 nearPoint = ReconstructWorldPosition(uv, 1.0, InverseJitteredViewProjection);
        		const vec2 radii     = SSDLobePixelRadii(ref.Roughness, abs(dot(ref.Normal, V)), pixelRadius,
        		                                         distance(nearPoint, ref.Position));
        		radius = min(radius, radii.y);
        		lobe   = SSDRoughnessLobe(ref.Roughness, ref.Normal, V);
        	}

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
        				const SSDSceneInfo other = SSDLoadSceneInfo(GBufferNormalTexture, GBufferDepthTexture,
        				                                            InverseJitteredViewProjection, p, size);
        				if (dot(other.Normal, other.Normal) <= 0.0)
        					continue; // sky
        				float w = (2.0 - abs(float(x))) * (2.0 - abs(float(y))) *
        				          SSDPlaneWeight(ref, other.Position, maxPlaneDistance) *
        				          SSDNormalWeight(ref.Normal, other.Normal);
        				const vec4 s = texelFetch(AccumulatedSignalTexture, p, 0);
        				if (SSDSignalIsSpecular(signal))
        				{
        					const vec3 otherV = SSDViewVector((vec2(p) + 0.5) / size, other.Position,
        					                                  InverseJitteredViewProjection);
        					w *= SSDLobeSimilarity(lobe, SSDRoughnessLobe(other.Roughness, other.Normal, otherV));
        					w *= SSDKarisWeight(s.a > 0.0 ? s.rgb / s.a : vec3(0.0));
        				}
        				sum       += s * w;
        				weightSum += w;
        			}
        		if (weightSum > 0.0)
        			result = sum / weightSum;
        	}
        	imageStore(DenoisedSignalTexture, pixel, result.a > 0.0 ? vec4(result.rgb / result.a, result.a) : vec4(0.0));
        }
    }
}
