// DesertAsset {"Kind":"Shader","Guid":"ed558b496e3343b397f9a20c49496f85","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsGenerateSamples"
{
    // "MegaLights: Generate Samples" (UE MegaLightsGenerateSamples, weighted reservoir sampling of the pixel's
    // froxel), per SAMPLE PIXEL at its representative render pixel (Common/MegaLights.glslh MegaLightsScreenPixel):
    // every light of the pixel's light-grid cell (every light when the cell overflowed, as the composite's
    // loop) is a candidate, weighed by the target function - its unshadowed contribution's luminance - and each of
    // the pixel's reservoirs takes one candidate in proportion to that weight (resampled importance sampling over
    // the whole cell: W = sum(p^) / p^(chosen)). No visibility here: only the samples that survive the temporal and
    // spatial reuse are traced ("MegaLights: Trace Samples", one ray each).

    Compute
    {
        Uniform sampler u_PointClampSampler;
        #define MEGALIGHTS_SHADING
        #include <Common/MegaLights.glslh>

        // The view's light grid (Programs/Deferred/LightGridBuild.shader), as the composite reads it.
        #include <Mesh/LightGrid.glslh>
        ReadBuffer(22) LightGridParamsUB
        {
        	LightGridParams lightGrid;
        };
        ReadBuffer(23) LightGridCellsUB
        {
        	uint lightGridCells[];
        };

        layout(binding = 11, rgba32f) writeonly uniform image2D u_OutReservoirs; // MegaLightsReservoir, stacked layers

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

        	int   chosen[MEGALIGHTS_MAX_SAMPLES];
        	float chosenTarget[MEGALIGHTS_MAX_SAMPLES];
        	for (uint k = 0u; k < uint(MEGALIGHTS_MAX_SAMPLES); ++k)
        	{
        		chosen[k]       = -1;
        		chosenTarget[k] = 0.0;
        	}
        	float weightSum = 0.0;
        	if (p.Valid)
        	{
        		const uint cell       = LightGridCellAt(lightGrid, vec2(screen) + 0.5, p.Position);
        		const uint count      = lightGridCells[cell];
        		const bool overflowed = LightGridCellOverflowed(lightGrid, count);
        		const uint candidates = overflowed ? uint(MegaLightsLightCount()) : count;
        		const uint list       = LightGridListOffset(lightGrid, cell);
        		uint       random     = MegaLightsSeed(pixel, 0u);
        		for (uint j = 0u; j < candidates; ++j)
        		{
        			const int   light  = overflowed ? int(j) : int(lightGridCells[list + j]);
        			const float target = MegaLightsTargetPdf(p, light);
        			if (target <= 0.0)
        				continue;
        			weightSum += target;
        			// Streaming weighted reservoir sampling, one independent draw per reservoir.
        			for (uint k = 0u; k < samples; ++k)
        				if (MegaLightsRandom(random) * weightSum < target)
        				{
        					chosen[k]       = light;
        					chosenTarget[k] = target;
        				}
        		}
        	}
        	for (uint k = 0u; k < samples; ++k)
        	{
        		MegaLightsReservoir r = MegaLightsEmptyReservoir();
        		if (chosen[k] >= 0)
        		{
        			r.Light     = chosen[k];
        			r.TargetPdf = chosenTarget[k];
        			r.W         = weightSum / chosenTarget[k];
        			r.M         = 1.0; // one frame's confidence: the whole cell was the candidate set
        		}
        		imageStore(u_OutReservoirs, MegaLightsReservoirTexel(pixel, k), MegaLightsPackReservoir(r));
        	}
        }
    }
}
