// DesertAsset {"Kind":"Shader","Guid":"f6a297e9567f40af9a49e58a6416b044","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsShadeSamples"
{
    // "MegaLights: Shade Samples" (UE MegaLightsShadeLightSamples), at RENDER resolution: every render pixel shades
    // the traced reservoirs of its sample pixel (Common/MegaLights.glslh MegaLightsSamplePixelOf - the
    // half-resolution samples upsampled as UE's ShadeLightSamples does) with ITS OWN surface: the light through the
    // texel's shading model unshadowed, times the reservoir's W, which "MegaLights: Trace Samples" already scaled by
    // the sample's visibility - no ray here. The shading model's specular lobe (EvaluateSpecular) and the rest
    // (diffuse) are written apart, each the mean of the reservoirs divided by its own reflectance
    // (Mesh/MegaLightsDemodulation.glslh): the two noisy signals the denoiser filters separately.

    Compute
    {
        Uniform sampler u_PointClampSampler;
        #define MEGALIGHTS_SHADING
        #include <Common/MegaLights.glslh>

        Uniform(10) texture2D u_Reservoirs; // "MegaLights: Trace Samples", sample-pixel sized
        #define u_Reservoirs sampler2D(u_Reservoirs, u_PointClampSampler)
        layout(binding = 15, rgba16f) writeonly uniform image2D u_OutDiffuse;  // rgb = demodulated diffuse, a = 1
        layout(binding = 18, rgba16f) writeonly uniform image2D u_OutSpecular; // rgb = demodulated specular, a = 1

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy); // a render pixel
        	const ivec2 size  = MegaLightsViewSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const MegaLightsPixel p = MegaLightsLoadPixel(pixel);
        	if (!p.Valid)
        	{
        		imageStore(u_OutDiffuse, pixel, vec4(0.0));
        		imageStore(u_OutSpecular, pixel, vec4(0.0));
        		return;
        	}
        	const uint  samples     = MegaLightsSamplesPerPixel();
        	const ivec2 samplePixel = MegaLightsSamplePixelOf(pixel);
        	vec3        diffuse     = vec3(0.0);
        	vec3        specular    = vec3(0.0);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const MegaLightsReservoir r =
        		     MegaLightsUnpackReservoir(texelFetch(u_Reservoirs, MegaLightsReservoirTexel(samplePixel, k), 0));
        		if (r.Light < 0 || r.Light >= MegaLightsLightCount() || r.W <= 0.0)
        			continue;
        		vec3 d;
        		vec3 s;
        		MegaLightsUnshadowedLobes(p, r.Light, d, s);
        		diffuse += d * r.W;
        		specular += s * r.W;
        	}
        	const float scale = 1.0 / float(samples);
        	imageStore(u_OutDiffuse, pixel, vec4(diffuse * scale / p.DiffuseReflectance, 1.0));
        	imageStore(u_OutSpecular, pixel, vec4(specular * scale / p.SpecularReflectance, 1.0));
        }
    }
}
