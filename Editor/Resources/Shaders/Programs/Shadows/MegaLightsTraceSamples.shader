// DesertAsset {"Kind":"Shader","Guid":"c335ed30c6694ec88f30e63116010664","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTraceSamples"
{
    // "MegaLights: Trace Samples" (UE MegaLightsHardwareRayTracing, an inline ray query): every reservoir's light is
    // tested for visibility from its pixel by one ray over the view's TLAS (Common/RayQueryVisibility.glslh, masked
    // segments alpha-tested). An occluded sample keeps its confidence and loses its weight (RTXDI visibility
    // reuse), so the temporal and spatial reuse that follow propagate only lights that reach a surface.

    Compute
    {
        #define RAY_QUERY_VISIBILITY_TLAS_BINDING      7
        #define RAY_QUERY_VISIBILITY_GEOMETRY_BINDING  8
        #define RAY_QUERY_VISIBILITY_MATERIALS_BINDING 9
        #include <Common/RayQueryVisibility.glslh>
        Uniform sampler u_PointClampSampler;
        #define MEGALIGHTS_TRACE
        #include <Common/MegaLights.glslh>

        Uniform(10) texture2D u_Reservoirs; // "MegaLights: Generate Samples"
        #define u_Reservoirs sampler2D(u_Reservoirs, u_PointClampSampler)
        layout(binding = 11, rgba32f) writeonly uniform image2D u_OutReservoirs;

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        	const ivec2 size  = MegaLightsViewSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const uint            samples = MegaLightsSamplesPerPixel();
        	const MegaLightsPixel p       = MegaLightsLoadPixel(pixel);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const ivec2         texel = MegaLightsReservoirTexel(pixel, k);
        		MegaLightsReservoir r     = MegaLightsUnpackReservoir(texelFetch(u_Reservoirs, texel, 0));
        		if (p.Valid && r.Light >= 0 && r.W > 0.0 && MegaLightsVisibility(p.Position, p.N, r.Light) <= 0.0)
        			r.W = 0.0;
        		imageStore(u_OutReservoirs, texel, MegaLightsPackReservoir(r));
        	}
        }
    }
}
