// DesertAsset {"Kind":"Shader","Guid":"ba4311b6d6114a04af3544c60fec6648","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSDReconstructDirectSpecular"
{
    // The screen-space denoiser's RECONSTRUCTION of MegaLights' specular direct light (UE MegaLights denoises its
    // specular lighting apart from the diffuse: MegaLightsDenoiserSpatial.usf), at full resolution from "MegaLights:
    // Shade Samples"' demodulated specular estimate (rgb = light, a = 1 on a surface). The diffuse reconstruction's
    // (2r+1)^2 bilateral kernel - tangent plane (UE BILATERAL_POSITION_BASED(2)), normals (UE BILATERAL_NORMAL) -
    // plus the agreement of the two texels' GGX lobes (UE BILATERAL_TOKOYASHI, SSDLobeSimilarity), and a kernel
    // no wider than the pixel's lobe on the screen (SSDLobePixelRadii): a mirror-like highlight is not spread over
    // its neighbours, a rough one is. Karis-weighted, so one firefly cannot drag the kernel. Written premultiplied
    // to ReconstructedSignalTexture.

    Compute
    {
        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound once by the pass by name and numbered by the program build (SharedSamplerBinding.hpp).
        // Declared before Common/ScreenSpaceDenoise.glslh, whose SSDLoadSceneInfo reads the G-buffer through PointClampSampler.
        Uniform sampler PointClampSampler;
        #include <Common/ScreenSpaceDenoise.glslh>

        Uniform(1) texture2D GBufferNormalTexture;   // rgb = world normal, a = roughness
        Uniform(2) texture2D GBufferDepthTexture;    // device depth (nearest)
        Uniform(3) texture2D RawSignalTexture;           // MegaLights' specular: rgb = light, a = 1 on a surface
        #define RawSignalTexture sampler2D(RawSignalTexture, PointClampSampler)
        layout(binding = 6, rgba16f) writeonly uniform image2D ReconstructedSignalTexture; // premultiplied (rgb * a, a)

        // ScreenSpaceDenoiser::ViewUniforms (std140).
        Uniform(0) DenoiserViewUB
        {
        	mat4 ViewProjection;
        	mat4 PreviousViewProjection;
        	mat4 InverseJitteredViewProjection;
        };

        // DirectSpecularDenoiserInputs::ReconstructionRadius.
        PushConstant PushConstants
        {
        	vec4 Reconstruction; // x = taps per side from the centre
        };

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 viewSize = textureSize(sampler2D(GBufferNormalTexture, PointClampSampler), 0);
        	const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= viewSize.x || pixel.y >= viewSize.y)
        		return;
        	if (!SSDPixelHasSignal(kSSDSignalDirectSpecular, texelFetch(sampler2D(GBufferNormalTexture, PointClampSampler), pixel, 0)))
        	{
        		imageStore(ReconstructedSignalTexture, pixel, vec4(0.0));
        		return;
        	}
        	const vec2         size = vec2(viewSize);
        	const vec2         uv   = (vec2(pixel) + 0.5) / size;
        	const SSDSceneInfo ref  = SSDLoadSceneInfo(GBufferNormalTexture, GBufferDepthTexture, InverseJitteredViewProjection,
        	                                           pixel, size);
        	// The farthest tap is r * sqrt(2) pixels away (UE WorldBluringRadiusToBilateralWorldDistance).
        	const float pixelRadius = SSDPixelWorldRadius(uv, texelFetch(sampler2D(GBufferDepthTexture, PointClampSampler), pixel, 0).r, ref.Position,
        	                                              1.0 / size, InverseJitteredViewProjection);
        	const vec3  V          = SSDViewVector(uv, ref.Position, InverseJitteredViewProjection);
        	const vec4  lobe       = SSDRoughnessLobe(ref.Roughness, ref.Normal, V);
        	const vec3  nearPoint  = ReconstructWorldPosition(uv, 1.0, InverseJitteredViewProjection);
        	const vec2  lobeRadii  = SSDLobePixelRadii(ref.Roughness, abs(dot(ref.Normal, V)), pixelRadius,
        	                                           distance(nearPoint, ref.Position));
        	// The producer's kernel, no wider than the lobe's footprint (the centre tap always stays).
        	const int   radius           = clamp(int(lobeRadii.y + 0.5), 0, max(int(Reconstruction.x + 0.5), 1));
        	const float maxPlaneDistance = max(pixelRadius * 2.0 * float(max(radius, 1)) * sqrt(2.0), 1e-3);

        	vec4  sum       = vec4(0.0); // premultiplied, Karis-weighted
        	float weightSum = 0.0;
        	for (int y = -radius; y <= radius; ++y)
        		for (int x = -radius; x <= radius; ++x)
        		{
        			const ivec2 p = clamp(pixel + ivec2(x, y), ivec2(0), viewSize - 1);
        			const SSDSceneInfo other = SSDLoadSceneInfo(GBufferNormalTexture, GBufferDepthTexture,
        			                                            InverseJitteredViewProjection, p, size);
        			if (dot(other.Normal, other.Normal) <= 0.0)
        				continue; // sky
        			const vec3 otherV = SSDViewVector((vec2(p) + 0.5) / size, other.Position,
        			                                  InverseJitteredViewProjection);
        			const vec4 s      = texelFetch(RawSignalTexture, p, 0);
        			const float w = (float(radius + 1) - abs(float(x))) * (float(radius + 1) - abs(float(y))) *
        			                SSDPlaneWeight(ref, other.Position, maxPlaneDistance) *
        			                SSDNormalWeight(ref.Normal, other.Normal) *
        			                SSDLobeSimilarity(lobe, SSDRoughnessLobe(other.Roughness, other.Normal, otherV)) *
        			                SSDKarisWeight(s.rgb);
        			sum       += vec4(s.rgb * s.a, s.a) * w;
        			weightSum += w;
        		}
        	imageStore(ReconstructedSignalTexture, pixel, weightSum > 0.0 ? sum / weightSum : vec4(0.0));
        }
    }
}
