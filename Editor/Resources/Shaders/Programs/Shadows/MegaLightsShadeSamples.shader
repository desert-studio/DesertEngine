// DesertAsset {"Kind":"Shader","Guid":"f6a297e9567f40af9a49e58a6416b044","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsShadeSamples"
{
    // "MegaLights: Shade Samples" (UE MegaLightsShadeLightSamples), at RENDER resolution: every render pixel shades
    // the traced samples of ONE of the four sample pixels around it (Common/MegaLightsSample.glslh
    // MegaLightsUpsamplePixel: drawn by the bilinear weight, the pixel's plane and its normal; none on its surface =
    // not shaded, alpha 0, the denoiser's reconstruction fills it) with ITS OWN surface: the light through the
    // texel's shading model unshadowed, times the sample's clamped weight and visibility - no ray here. The shading
    // model's specular lobe (EvaluateSpecular) and the rest (diffuse) are written apart, each the mean of the
    // samples divided by its own reflectance (Mesh/MegaLightsDemodulation.glslh): the two noisy signals the denoiser
    // filters separately.

    Compute
    {
        Uniform sampler PointClampSampler;
        #define MEGALIGHTS_SHADING
        #include <Common/MegaLights.glslh>

        // "MegaLights: Trace Samples", sample-pixel sized, stacked layers
        layout(set = 0, binding = 10) uniform utexture2D SamplesTexture;
        #define SamplesTexture usampler2D(SamplesTexture, PointClampSampler)
        layout(binding = 15, rgba16f) writeonly uniform image2D OutDiffuseTexture;  // rgb = demodulated diffuse, a = shaded
        layout(binding = 18, rgba16f) writeonly uniform image2D OutSpecularTexture; // rgb = demodulated specular, a = shaded

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy); // a render pixel
        	const ivec2 size  = MegaLightsViewSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const MegaLightsPixel p = MegaLightsLoadPixel(pixel);
        	uint  random = MegaLightsSeed(pixel, 4u);
        	ivec2 samplePixel;
        	if (!p.Valid || !MegaLightsUpsamplePixel(pixel, p, random, samplePixel))
        	{
        		imageStore(OutDiffuseTexture, pixel, vec4(0.0));
        		imageStore(OutSpecularTexture, pixel, vec4(0.0));
        		return;
        	}
        	const uint  samples     = MegaLightsSamplesPerPixel();
        	vec3        diffuse     = vec3(0.0);
        	vec3        specular    = vec3(0.0);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const MegaLightsSample light =
        		     MegaLightsUnpackSample(texelFetch(SamplesTexture, MegaLightsSampleTexel(samplePixel, k), 0).rg);
        		const float weight = MegaLightsShadingWeight(light, Params);
        		if (light.Light < 0 || light.Light >= MegaLightsLightCount() || weight <= 0.0)
        			continue;
        		vec3 d;
        		vec3 s;
        		MegaLightsUnshadowedLobes(p, light.Light, d, s);
        		diffuse += d * weight;
        		specular += s * weight;
        	}
        	const float scale = 1.0 / float(samples);
        	imageStore(OutDiffuseTexture, pixel, vec4(diffuse * scale / p.DiffuseReflectance, 1.0));
        	imageStore(OutSpecularTexture, pixel, vec4(specular * scale / p.SpecularReflectance, 1.0));
        }
    }
}
