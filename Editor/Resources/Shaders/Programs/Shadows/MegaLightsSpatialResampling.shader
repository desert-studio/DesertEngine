// DesertAsset {"Kind":"Shader","Guid":"9da64fdc296b4ea788e29c6ea7e6676e","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsSpatialResampling"
{
    // "MegaLights: Spatial Resampling" (ReSTIR DI spatial reuse): each reservoir merges the same layer of
    // Shadows.MegaLightsSpatialSamples neighbouring sample pixels drawn inside a disc of the method's radius (in sample
    // pixels) around the sample pixel, each
    // re-weighed by the target function at THIS pixel. A neighbour on another surface (its normal or depth differs)
    // is skipped: its lights' weights there say nothing about this surface. Nothing is traced yet: "MegaLights: Trace
    // Samples" tests the light each reservoir ends with, once.

    Compute
    {
        Uniform sampler PointClampSampler;
        #define MEGALIGHTS_SHADING
        #include <Common/MegaLights.glslh>

        Uniform(10) texture2D ReservoirsTexture; // "MegaLights: Temporal Resampling", sample-pixel sized
        #define ReservoirsTexture sampler2D(ReservoirsTexture, PointClampSampler)
        layout(binding = 11, rgba32f) writeonly uniform image2D OutReservoirsTexture;

        // The neighbour's surface is this pixel's: the normals within ~25 degrees, the depth within 10 %.
        const float kNormalThreshold = 0.9;
        const float kDepthThreshold  = 0.1;

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy); // a sample pixel
        	const ivec2 size  = MegaLightsSampleSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const uint            samples   = MegaLightsSamplesPerPixel();
        	const uint            neighbors = Counts.w;
        	const MegaLightsPixel p         = MegaLightsLoadPixel(MegaLightsScreenPixel(pixel));
        	uint                  random    = MegaLightsSeed(pixel, 2u);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const MegaLightsReservoir center =
        		     MegaLightsUnpackReservoir(texelFetch(ReservoirsTexture, MegaLightsReservoirTexel(pixel, k), 0));
        		if (!p.Valid || neighbors == 0u)
        		{
        			imageStore(OutReservoirsTexture, MegaLightsReservoirTexel(pixel, k), MegaLightsPackReservoir(center));
        			continue;
        		}
        		MegaLightsMerge merge = MegaLightsBeginMerge();
        		MegaLightsMergeReservoir(merge, center, center.TargetPdf, center.M, MegaLightsRandom(random));
        		for (uint n = 0u; n < neighbors; ++n)
        		{
        			// A uniform point of the disc (sqrt of the radius' random: uniform in area).
        			const float radius = Params.w * sqrt(MegaLightsRandom(random));
        			const float angle  = 6.28318530718 * MegaLightsRandom(random);
        			const ivec2 other  = pixel + ivec2(round(radius * vec2(cos(angle), sin(angle))));
        			if (other == pixel || other.x < 0 || other.y < 0 || other.x >= size.x || other.y >= size.y)
        				continue;
        			const MegaLightsPixel q = MegaLightsLoadPixel(MegaLightsScreenPixel(other));
        			if (!q.Valid || dot(q.N, p.N) < kNormalThreshold ||
        			    abs(q.ViewDepth - p.ViewDepth) > kDepthThreshold * p.ViewDepth)
        				continue;
        			const MegaLightsReservoir neighbour =
        			     MegaLightsUnpackReservoir(texelFetch(ReservoirsTexture, MegaLightsReservoirTexel(other, k), 0));
        			if (neighbour.Light < 0)
        			{
        				merge.Result.M += neighbour.M;
        				continue;
        			}
        			MegaLightsMergeReservoir(merge, neighbour, MegaLightsTargetPdf(p, neighbour.Light), neighbour.M,
        			                         MegaLightsRandom(random));
        		}
        		imageStore(OutReservoirsTexture, MegaLightsReservoirTexel(pixel, k),
        		           MegaLightsPackReservoir(MegaLightsEndMerge(merge)));
        	}
        }
    }
}
