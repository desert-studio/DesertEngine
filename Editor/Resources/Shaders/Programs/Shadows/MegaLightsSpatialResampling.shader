// DesertAsset {"Kind":"Shader","Guid":"9da64fdc296b4ea788e29c6ea7e6676e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsSpatialResampling"
{
    // "MegaLights: Spatial Resampling" (ReSTIR DI spatial reuse): each reservoir merges the same layer of
    // Shadows.MegaLightsSpatialSamples neighbours drawn inside a disc of the method's radius around the pixel, each
    // re-weighed by the target function at THIS pixel. A neighbour on another surface (its normal or depth differs)
    // is skipped: its lights' weights there say nothing about this surface. The neighbours' visibility is theirs;
    // "MegaLights: Shade Samples" traces the merged light again from this pixel before it is shaded.

    Compute
    {
        Uniform sampler u_PointClampSampler;
        #define MEGALIGHTS_SHADING
        #include <Common/MegaLights.glslh>

        Uniform(10) texture2D u_Reservoirs; // "MegaLights: Temporal Resampling"
        #define u_Reservoirs sampler2D(u_Reservoirs, u_PointClampSampler)
        layout(binding = 11, rgba32f) writeonly uniform image2D u_OutReservoirs;

        // The neighbour's surface is this pixel's: the normals within ~25 degrees, the depth within 10 %.
        const float kNormalThreshold = 0.9;
        const float kDepthThreshold  = 0.1;

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        	const ivec2 size  = MegaLightsViewSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const uint            samples   = MegaLightsSamplesPerPixel();
        	const uint            neighbors = u_Counts.w;
        	const MegaLightsPixel p         = MegaLightsLoadPixel(pixel);
        	uint                  random    = MegaLightsSeed(pixel, 2u);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const MegaLightsReservoir center =
        		     MegaLightsUnpackReservoir(texelFetch(u_Reservoirs, MegaLightsReservoirTexel(pixel, k), 0));
        		if (!p.Valid || neighbors == 0u)
        		{
        			imageStore(u_OutReservoirs, MegaLightsReservoirTexel(pixel, k), MegaLightsPackReservoir(center));
        			continue;
        		}
        		MegaLightsMerge merge = MegaLightsBeginMerge();
        		MegaLightsMergeReservoir(merge, center, center.TargetPdf, center.M, MegaLightsRandom(random));
        		for (uint n = 0u; n < neighbors; ++n)
        		{
        			// A uniform point of the disc (sqrt of the radius' random: uniform in area).
        			const float radius = u_Params.w * sqrt(MegaLightsRandom(random));
        			const float angle  = 6.28318530718 * MegaLightsRandom(random);
        			const ivec2 other  = pixel + ivec2(round(radius * vec2(cos(angle), sin(angle))));
        			if (other == pixel || other.x < 0 || other.y < 0 || other.x >= size.x || other.y >= size.y)
        				continue;
        			const MegaLightsPixel q = MegaLightsLoadPixel(other);
        			if (!q.Valid || dot(q.N, p.N) < kNormalThreshold ||
        			    abs(q.ViewDepth - p.ViewDepth) > kDepthThreshold * p.ViewDepth)
        				continue;
        			const MegaLightsReservoir neighbour =
        			     MegaLightsUnpackReservoir(texelFetch(u_Reservoirs, MegaLightsReservoirTexel(other, k), 0));
        			if (neighbour.Light < 0)
        			{
        				merge.Result.M += neighbour.M;
        				continue;
        			}
        			MegaLightsMergeReservoir(merge, neighbour, MegaLightsTargetPdf(p, neighbour.Light), neighbour.M,
        			                         MegaLightsRandom(random));
        		}
        		imageStore(u_OutReservoirs, MegaLightsReservoirTexel(pixel, k),
        		           MegaLightsPackReservoir(MegaLightsEndMerge(merge)));
        	}
        }
    }
}
