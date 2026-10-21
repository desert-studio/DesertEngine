// DesertAsset {"Kind":"Shader","Guid":"c335ed30c6694ec88f30e63116010664","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTraceSamples"
{
    // "MegaLights: Trace Samples" (UE MegaLightsHardwareRayTracing + its screen traces): THE ONE RAY of each sample.
    // Per sample pixel, after the temporal and spatial reuse, the light every reservoir ended with is tested once
    // from the sample pixel's representative surface (Common/MegaLights.glslh MegaLightsVisibility): its contact
    // shadow through the G-buffer depth, then one ray query over the view's TLAS (masked segments alpha-tested)
    // toward a point of the light's source sphere. The visibility scales the reservoir's W (0 = occluded: it keeps
    // its confidence and loses its weight, RTXDI visibility reuse). These reservoirs are both what every render
    // pixel of the block shades ("MegaLights: Shade Samples", no ray) and next frame's temporal history, with the
    // representative's surface beside them.

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
        layout(binding = 11, rgba32f) writeonly uniform image2D u_OutReservoirs; // shaded now, history next frame
        layout(binding = 17, rgba16f) writeonly uniform image2D u_OutSurface;    // xyz = normal, w = clip w (0 = none)

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy); // a sample pixel
        	const ivec2 size  = MegaLightsSampleSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const uint            samples = MegaLightsSamplesPerPixel();
        	const ivec2           screen  = MegaLightsScreenPixel(pixel);
        	const MegaLightsPixel p       = MegaLightsLoadPixel(screen);
        	g_MegaLightsTracedPixel       = vec2(screen) + 0.5;
        	uint random                   = MegaLightsSeed(pixel, 3u);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const ivec2         texel = MegaLightsReservoirTexel(pixel, k);
        		MegaLightsReservoir r     = MegaLightsUnpackReservoir(texelFetch(u_Reservoirs, texel, 0));
        		if (!p.Valid)
        			r = MegaLightsEmptyReservoir();
        		else if (r.Light >= 0 && r.W > 0.0)
        			r.W *= MegaLightsVisibility(p, r.Light, random);
        		imageStore(u_OutReservoirs, texel, MegaLightsPackReservoir(r));
        	}
        	imageStore(u_OutSurface, pixel, p.Valid ? vec4(p.N, p.ViewDepth) : vec4(0.0));
        }
    }
}
