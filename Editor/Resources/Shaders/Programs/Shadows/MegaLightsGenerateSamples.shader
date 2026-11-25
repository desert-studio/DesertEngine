// DesertAsset {"Kind":"Shader","Guid":"ed558b496e3343b397f9a20c49496f85","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsGenerateSamples"
{
    // "MegaLights: Generate Samples" (UE MegaLightsGenerateSamples, weighted reservoir sampling of the pixel's
    // froxel), per SAMPLE PIXEL at its representative render pixel (Common/MegaLights.glslh MegaLightsScreenPixel):
    // every light of the pixel's light-grid cell (every light when the cell overflowed, as the composite's
    // loop) is a candidate, weighed by the target function (the perceived brightness of its unshadowed contribution),
    // and each of the pixel's samples takes one candidate in proportion to that weight (resampled importance
    // sampling over the whole cell: weight = sum(p^) / p^(chosen)).
    //
    // GUIDED by last frame's visibility (UE MegaLightsSampling.usf): a light the reprojected tile's hash and its
    // four neighbours' ("MegaLights: Visible Light Hash") do not hold is drawn with Params.w of its weight. It
    // changes the probability only: the weight divides by the same number. Without a hash (off screen, hidden
    // last frame, the first frame), and for a light the previous frame did not have, every light keeps its whole weight.

    Compute
    {
        Uniform sampler PointClampSampler;
        #define MEGALIGHTS_SHADING
        #include <Common/MegaLights.glslh>
        #include <Common/RisReservoir.glslh>
        #include <Common/SSDHistoryDepth.glslh>

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

        layout(binding = 11, rg32ui) writeonly uniform uimage2D OutSamplesTexture; // MegaLightsSample, stacked layers
        // The light's row in the previous frame's index space (-1: it had none; ViewLightBuffers::PreviousRow).
        layout(std430, binding = 12) readonly buffer PreviousLightRowsUB
        {
        	int previousLightRows[];
        };
        // The previous frame's "MegaLights: Visible Light Hash" (Counts.w = 0: unreadable).
        layout(std430, binding = 13) readonly buffer HistoryVisibleLightsUB
        {
        	uvec4 historyVisibleLights[];
        };
        Uniform(14) texture2D VelocityTexture; // rg = this frame's NDC minus the previous frame's, unjittered
        #define VelocityTexture sampler2D(VelocityTexture, PointClampSampler)

        // The diffuse denoiser's previous count and view depth (Common/SSDHistoryDepth.glslh), render sized: the
        // surfaces the hash was made of. Black where Counts.w = 0.
        Uniform(15) texture2D HistoryCountTexture;
        #define HistoryCountTexture sampler2D(HistoryCountTexture, PointClampSampler)

        // The emissive triangles a sample pixel draws per frame (UE MegaLights' emissive candidates per pixel).
        #define MEGALIGHTS_EMISSIVE_CANDIDATES 4

        LocalSize(8, 8, 1);

        // The union of the hashes of the tile @p screen's surface was in last frame and of its four neighbours.
        // False when there is no hash, when it was off the screen, or when another surface stood there (its depth,
        // UE MegaLights' reprojection test): the hash of the occluder says nothing of what it hid.
        bool LoadVisibleHash(ivec2 screen, MegaLightsPixel p, out uvec4 tile)
        {
        	tile = uvec4(0u);
        	if (Counts.w == 0u)
        		return false;
        	const vec4 clip = ViewProjection * vec4(p.Position, 1.0);
        	const vec2 ndc  = clip.xy / clip.w - texelFetch(VelocityTexture, screen, 0).rg;
        	const vec2 uv   = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        	if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))))
        		return false;
        	const vec2  size        = vec2(MegaLightsViewSize());
        	const vec2  stored      = texelFetch(HistoryCountTexture, ivec2(uv * size), 0).rg;
        	const float before      = SSDStoredDepth((PreviousViewProjection * vec4(p.Position, 1.0)).w);
        	const vec2  rightUv     = (vec2(screen) + vec2(1.5, 0.5)) / size;
        	const float depth       = texelFetch(GBufferDepthTexture, screen, 0).r;
        	const float pixelRadius =
        	     distance(ReconstructWorldPosition(rightUv, depth, InverseJitteredViewProjection), p.Position);
        	if (stored.x <= 0.0 ||
        	    abs(stored.y - before) > SSDHistoryDepthTolerance(before, pixelRadius, dot(p.N, p.Surface.V)))
        		return false;
        	const ivec2 tiles  = MegaLightsHashTiles();
        	const ivec2 center = ivec2(uv * vec2(MegaLightsSampleSize())) / MEGALIGHTS_HASH_TILE;
        	const ivec2 offsets[5] = ivec2[5](ivec2(0, 0), ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
        	for (int i = 0; i < 5; ++i)
        	{
        		const ivec2 at = clamp(center + offsets[i], ivec2(0), tiles - 1);
        		tile |= historyVisibleLights[at.y * tiles.x + at.x];
        	}
        	return true;
        }

        // 1 / the density the candidate @p light was drawn with: every local light of the cell is a candidate, an
        // emissive triangle is one of MEGALIGHTS_EMISSIVE_CANDIDATES draws from the list's power tree.
        float InverseDensity(int light)
        {
        	if (!MegaLightsLightIsEmissive(light))
        		return 1.0;
        	const uint  triangle = uint(light - MegaLightsLocalLightCount());
        	const float pick     = EmissiveLightsPickPdf(triangle, EmissiveCounts, EmissiveDynamicCounts);
        	return 1.0 / (float(MEGALIGHTS_EMISSIVE_CANDIDATES) * pick);
        }

        // Whether @p light keeps its whole sampling weight: it casts no shadow, the pixel has no hash, the previous
        // frame did not have it, or the hash holds it.
        bool GuidedVisible(int light, bool hashed, uvec4 tile)
        {
        	if (!hashed || MegaLightsLightIsEmissive(light) || !MegaLightsLightCastsShadow(light))
        		return true;
        	const int previous = previousLightRows[light];
        	return previous < 0 || MegaLightsHashHolds(tile, uint(previous));
        }

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy); // a sample pixel
        	const ivec2 size  = MegaLightsSampleSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const uint            samples = MegaLightsSamplesPerPixel();
        	const ivec2           screen  = MegaLightsScreenPixel(pixel);
        	const MegaLightsPixel p       = MegaLightsLoadPixel(screen);

        	// One RIS stream per sample over EVERY light of the cell (M = 1: the whole cell was the candidate set, each
        	// light with source density 1), each with its own draws (Common/RisReservoir.glslh).
        	RisMerge streams[MEGALIGHTS_MAX_SAMPLES];
        	for (uint k = 0u; k < uint(MEGALIGHTS_MAX_SAMPLES); ++k)
        		streams[k] = RisBeginMerge();
        	uvec4 tile   = uvec4(0u);
        	bool  hashed = false;
        	if (p.Valid)
        	{
        		hashed                = LoadVisibleHash(screen, p, tile);
        		const uint cell       = LightGridCellAt(lightGrid, vec2(screen) + 0.5, p.Position);
        		const uint count      = lightGridCells[cell];
        		const bool overflowed = LightGridCellOverflowed(lightGrid, count);
        		const uint candidates = overflowed ? uint(MegaLightsLocalLightCount()) : count;
        		const uint list       = LightGridListOffset(lightGrid, cell);
        		uint       random     = MegaLightsSeed(pixel, 0u);
        		for (uint j = 0u; j < candidates; ++j)
        		{
        			const int   light  = overflowed ? int(j) : int(lightGridCells[list + j]);
        			float       target = MegaLightsTargetPdf(p, light);
        			if (target <= 0.0)
        				continue;
        			if (!GuidedVisible(light, hashed, tile))
        				target *= Params.w;
        			for (uint k = 0u; k < samples; ++k)
        				RisStreamCandidate(streams[k], light, target, 1.0, MegaLightsRandom(random));
        		}
        		// THE EMISSIVE TRIANGLES (Common/MegaLights.glslh): MEGALIGHTS_EMISSIVE_CANDIDATES drawn from the
        		// list's power tree, each streamed with the inverse of its draw density (1 / (count x pick pdf)), so
        		// the streams' weight sums stay unbiased estimates of the sum over every light. Their OWN stream: the
        		// local lights above drew what a frame without emissive lights draws.
        		const float emissiveTotal = MegaLightsEmissiveTriangleCount() > 0
        		                                 ? EmissiveLightsTotal(EmissiveCounts, EmissiveDynamicCounts)
        		                                 : 0.0;
        		if (emissiveTotal > 0.0)
        		{
        			uint emissiveRandom = MegaLightsPcg(MegaLightsSeed(pixel, 0u) ^ 0x6A09E667u);
        			for (uint j = 0u; j < uint(MEGALIGHTS_EMISSIVE_CANDIDATES); ++j)
        			{
        				const uint  triangle =
        				     EmissiveLightsPick(MegaLightsRandom(emissiveRandom), EmissiveCounts, EmissiveDynamicCounts);
        				const float pick = EmissiveLightsPickPdf(triangle, EmissiveCounts, EmissiveDynamicCounts);
        				if (int(triangle) >= MegaLightsEmissiveTriangleCount() || pick <= 0.0)
        					continue;
        				const int   light  = MegaLightsLocalLightCount() + int(triangle);
        				const float target = MegaLightsTargetPdf(p, light);
        				if (target <= 0.0)
        					continue;
        				const float inverseDensity = InverseDensity(light);
        				for (uint k = 0u; k < samples; ++k)
        					RisStreamCandidate(streams[k], light, target, inverseDensity,
        					                   MegaLightsRandom(emissiveRandom));
        			}
        		}
        	}
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const RisReservoir r      = RisEndStream(streams[k], 1.0);
        		float              weight = 0.0;
        		if (r.Light >= 0)
        		{
        			const float inverseDensity = InverseDensity(r.Light);
        			weight = inverseDensity * MegaLightsSelectionWeight(r.W / inverseDensity,
        			                                                    GuidedVisible(r.Light, hashed, tile), Params);
        		}
        		const uvec2 packedSample = MegaLightsPackSample(MegaLightsSample(r.Light, weight, 1.0));
        		imageStore(OutSamplesTexture, MegaLightsSampleTexel(pixel, k), uvec4(packedSample, 0u, 0u));
        	}
        }
    }
}
