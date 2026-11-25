// DesertAsset {"Kind":"Shader","Guid":"09c5650b313e4c018f76a93325a02472","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSDReconstructDiffuse"
{
    // The screen-space denoiser's RECONSTRUCTION of the diffuse-indirect signal (UE ScreenSpaceDenoiser, diffuse
    // indirect: SSDSpatialAccumulation.usf STAGE_RECONSTRUCTION, BILATERAL_PRESET_DIFFUSE), at full resolution
    // from the RSM-GI gather's one jittered estimate per pixel (GIResolve.shader: rgb = bounce light, a = 1 where
    // it gathered; MegaLights' demodulated local light). A (2r+1)^2 kernel (tent-weighted, r = the producer's
    // ReconstructionRadius, 2 = 5x5) where each tap is weighed by the BILATERAL weight of its surface
    // against this pixel's: its distance off this pixel's tangent plane (UE BILATERAL_POSITION_BASED(2)) and the
    // agreement of the normals (UE BILATERAL_NORMAL, N.N^4) - bounce light does not cross a crease or a depth
    // edge - and by its light (Common/SSDWeights.glslh: the Karis weight, the brightness weight of the 3x3
    // neighbourhood). A tap that answered nothing (a = 0) adds nothing and weighs nothing. Diffuse light carries no direction, so the lobe does not enter. Written premultiplied to ReconstructedSignalTexture
    // - except SSGI's signal (rgb = the bounce over the whole pixel, a = the sky's visibility,
    // DiffuseIndirectAlpha::SkyVisibility), whose rgb and a are filtered straight, side by side (Reconstruction.y).

    Compute
    {
        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound once by the pass by name and numbered by the program build (SharedSamplerBinding.hpp).
        // Declared before Common/ScreenSpaceDenoise.glslh, whose SSDLoadSceneInfo reads the G-buffer through PointClampSampler.
        Uniform sampler PointClampSampler;
        #include <Common/ScreenSpaceDenoise.glslh>

        Uniform(1) texture2D GBufferNormalTexture;   // rgb = world normal, a = roughness
        Uniform(2) texture2D GBufferDepthTexture;    // device depth (nearest)
        Uniform(3) texture2D RawSignalTexture;       // rgb = bounce light, a = answered share (SSGI: sky visibility)
        layout(binding = 6, rgba16f) writeonly uniform image2D ReconstructedSignalTexture; // premultiplied (rgb * a, a)

        // ScreenSpaceDenoiser::ViewUniforms (std140).
        Uniform(0) DenoiserViewUB
        {
        	mat4 ViewProjection;
        	mat4 PreviousViewProjection;
        	mat4 InverseJitteredViewProjection;
        };

        // DiffuseIndirectDenoiserInputs::ReconstructionRadius.
        PushConstant PushConstants
        {
        	// x = taps per side from the centre, y = 1 when a is a signal of its own (DiffuseIndirectAlpha::
        	// SkyVisibility, SSGI: the traced hemisphere's miss share) - rgb and a then filter straight, side by
        	// side; 0 when a is the answered share (RSM-GI, MegaLights) - rgb is weighed by it (premultiplied)
        	vec4 Reconstruction;
        };

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 viewSize = textureSize(sampler2D(GBufferNormalTexture, PointClampSampler), 0);
        	const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= viewSize.x || pixel.y >= viewSize.y)
        		return;
        	if (!SSDPixelHasSignal(kSSDSignalDiffuseIndirect, texelFetch(sampler2D(GBufferNormalTexture, PointClampSampler), pixel, 0), false))
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
        	const int   radius           = max(int(Reconstruction.x + 0.5), 1);
        	const float maxPlaneDistance = max(pixelRadius * 2.0 * float(radius) * sqrt(2.0), 1e-3);
        	const bool  alphaIsSignal    = Reconstruction.y >= 0.5;

        	const vec2  brightness = SSDBrightnessStats(RawSignalTexture, pixel, viewSize, !alphaIsSignal);
        	vec4  sum       = vec4(0.0); // premultiplied (straight where alphaIsSignal)
        	float weightSum = 0.0;
        	float alphaSum       = 0.0; // a that is a signal of its own: filtered by the surface alone
        	float alphaWeightSum = 0.0;
        	for (int y = -radius; y <= radius; ++y)
        		for (int x = -radius; x <= radius; ++x)
        		{
        			const ivec2 p = clamp(pixel + ivec2(x, y), ivec2(0), viewSize - 1);
        			const SSDSceneInfo other = SSDLoadSceneInfo(GBufferNormalTexture, GBufferDepthTexture,
        			                                            InverseJitteredViewProjection, p, size);
        			if (dot(other.Normal, other.Normal) <= 0.0)
        				continue; // sky
        			const float surface = (float(radius + 1) - abs(float(x))) * (float(radius + 1) - abs(float(y))) *
        			                SSDPlaneWeight(ref, other.Position, maxPlaneDistance) *
        			                SSDNormalWeight(ref.Normal, other.Normal);
        			const vec4  s = texelFetch(sampler2D(RawSignalTexture, PointClampSampler), p, 0);
        			const float w = surface * SSDKarisWeight(s.rgb) * SSDBrightnessWeight(s.rgb, brightness);
        			sum            += (alphaIsSignal ? s : vec4(s.rgb * s.a, s.a)) * w;
        			weightSum      += w;
        			alphaSum       += s.a * surface;
        			alphaWeightSum += surface;
        		}
        	vec4 reconstructed = weightSum > 0.0 ? sum / weightSum : vec4(0.0);
        	if (alphaIsSignal && alphaWeightSum > 0.0)
        		reconstructed.a = alphaSum / alphaWeightSum;
        	imageStore(ReconstructedSignalTexture, pixel, reconstructed);
        }
    }
}
