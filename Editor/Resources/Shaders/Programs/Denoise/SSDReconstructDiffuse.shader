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
    // edge. Diffuse light carries no direction, so the lobe does not enter. Written premultiplied to u_Signal.

    Compute
    {
        // Shared sampler states (UE shared samplers): each input below is a separate texture read through one of
        // these, bound once by the pass by name and numbered by the program build (SharedSamplerBinding.hpp).
        // Declared before Common/ScreenSpaceDenoise.glslh, whose SSDLoadSceneInfo reads the G-buffer through u_PointClampSampler.
        Uniform sampler u_PointClampSampler;
        #include <Common/ScreenSpaceDenoise.glslh>

        Uniform(1) texture2D u_GBufferNormal;   // rgb = world normal, a = roughness
        Uniform(2) texture2D u_GBufferDepth;    // device depth (nearest)
        Uniform(3) texture2D u_Input;           // the gather: rgb = bounce light, a = 1 where it gathered
        #define u_Input sampler2D(u_Input, u_PointClampSampler)
        layout(binding = 6, rgba16f) writeonly uniform image2D u_Signal; // premultiplied (rgb * a, a)

        // ScreenSpaceDenoiser::ViewUniforms (std140).
        Uniform(0) DenoiserViewUB
        {
        	mat4 u_ViewProjection;
        	mat4 u_PrevViewProjection;
        	mat4 u_InvJitteredViewProjection;
        };

        // DiffuseIndirectDenoiserInputs::ReconstructionRadius.
        PushConstant PushConstants
        {
        	vec4 u_Reconstruction; // x = taps per side from the centre
        };

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 viewSize = textureSize(sampler2D(u_GBufferNormal, u_PointClampSampler), 0);
        	const ivec2 pixel    = ivec2(gl_GlobalInvocationID.xy);
        	if (pixel.x >= viewSize.x || pixel.y >= viewSize.y)
        		return;
        	if (!SSDPixelHasSignal(kSSDSignalDiffuseIndirect, texelFetch(sampler2D(u_GBufferNormal, u_PointClampSampler), pixel, 0)))
        	{
        		imageStore(u_Signal, pixel, vec4(0.0));
        		return;
        	}
        	const vec2         size = vec2(viewSize);
        	const vec2         uv   = (vec2(pixel) + 0.5) / size;
        	const SSDSceneInfo ref  = SSDLoadSceneInfo(u_GBufferNormal, u_GBufferDepth, u_InvJitteredViewProjection,
        	                                           pixel, size);
        	// The farthest tap is r * sqrt(2) pixels away (UE WorldBluringRadiusToBilateralWorldDistance).
        	const float pixelRadius = SSDPixelWorldRadius(uv, texelFetch(sampler2D(u_GBufferDepth, u_PointClampSampler), pixel, 0).r, ref.Position,
        	                                              1.0 / size, u_InvJitteredViewProjection);
        	const int   radius           = max(int(u_Reconstruction.x + 0.5), 1);
        	const float maxPlaneDistance = max(pixelRadius * 2.0 * float(radius) * sqrt(2.0), 1e-3);

        	vec4  sum       = vec4(0.0); // premultiplied
        	float weightSum = 0.0;
        	for (int y = -radius; y <= radius; ++y)
        		for (int x = -radius; x <= radius; ++x)
        		{
        			const ivec2 p = clamp(pixel + ivec2(x, y), ivec2(0), viewSize - 1);
        			const SSDSceneInfo other = SSDLoadSceneInfo(u_GBufferNormal, u_GBufferDepth,
        			                                            u_InvJitteredViewProjection, p, size);
        			if (dot(other.Normal, other.Normal) <= 0.0)
        				continue; // sky
        			const float w = (float(radius + 1) - abs(float(x))) * (float(radius + 1) - abs(float(y))) *
        			                SSDPlaneWeight(ref, other.Position, maxPlaneDistance) *
        			                SSDNormalWeight(ref.Normal, other.Normal);
        			const vec4 s = texelFetch(u_Input, p, 0);
        			sum       += vec4(s.rgb * s.a, s.a) * w;
        			weightSum += w;
        		}
        	imageStore(u_Signal, pixel, weightSum > 0.0 ? sum / weightSum : vec4(0.0));
        }
    }
}
