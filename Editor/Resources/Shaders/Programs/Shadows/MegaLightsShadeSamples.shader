// DesertAsset {"Kind":"Shader","Guid":"f6a297e9567f40af9a49e58a6416b044","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsShadeSamples"
{
    // "MegaLights: Shade Samples" (UE MegaLightsShadeLightSamples): each final reservoir's light is traced once more
    // from THIS pixel (the spatial reuse brought neighbours' lights whose visibility was theirs), shaded through the
    // texel's shading model unshadowed and weighed by the reservoir's W and that visibility; the mean of the pixel's
    // reservoirs, divided by the surface's reflectance (Mesh/MegaLightsDemodulation.glslh), is the noisy local
    // lighting the denoiser filters. The shaded reservoirs - an occluded one with no weight - and the pixel's
    // surface are next frame's temporal history.

    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      7
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  8
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 9
        #include <Common/RayQueryVisibility.glslh>
        Uniform sampler u_PointClampSampler;
        #define MEGALIGHTS_SHADING
        #define MEGALIGHTS_TRACE
        #include <Common/MegaLights.glslh>

        Uniform(10) texture2D u_Reservoirs; // "MegaLights: Spatial Resampling"
        #define u_Reservoirs sampler2D(u_Reservoirs, u_PointClampSampler)
        layout(binding = 11, rgba32f) writeonly uniform image2D u_OutReservoirs; // next frame's history
        layout(binding = 15, rgba16f) writeonly uniform image2D u_OutLighting;   // rgb = demodulated light, a = 1
        layout(binding = 17, rgba16f) writeonly uniform image2D u_OutSurface;    // xyz = normal, w = clip w (0 = none)

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        	const ivec2 size  = MegaLightsViewSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const uint            samples = MegaLightsSamplesPerPixel();
        	const MegaLightsPixel p       = MegaLightsLoadPixel(pixel);
        	vec3                  light   = vec3(0.0);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const ivec2         texel = MegaLightsReservoirTexel(pixel, k);
        		MegaLightsReservoir r     = MegaLightsUnpackReservoir(texelFetch(u_Reservoirs, texel, 0));
        		if (!p.Valid)
        			r = MegaLightsEmptyReservoir();
        		else if (r.Light >= 0 && r.W > 0.0)
        		{
        			if (MegaLightsVisibility(p.Position, p.N, r.Light) > 0.0)
        				light += MegaLightsUnshadowed(p, r.Light) * r.W;
        			else
        				r.W = 0.0;
        		}
        		imageStore(u_OutReservoirs, texel, MegaLightsPackReservoir(r));
        	}
        	if (!p.Valid)
        	{
        		imageStore(u_OutLighting, pixel, vec4(0.0));
        		imageStore(u_OutSurface, pixel, vec4(0.0));
        		return;
        	}
        	imageStore(u_OutLighting, pixel, vec4(light / (float(samples) * p.Reflectance), 1.0));
        	imageStore(u_OutSurface, pixel, vec4(p.N, p.ViewDepth));
        }
    }
}
