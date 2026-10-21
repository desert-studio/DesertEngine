// DesertAsset {"Kind":"Shader","Guid":"a3a399c19dc94e1eb0192710a9e3a45d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTemporalResampling"
{
    // "MegaLights: Temporal Resampling" (ReSTIR DI temporal reuse; UE MegaLights' history through the velocity): the
    // sample pixel's representative surface is reprojected into the view's previous frame through the velocity (NDC
    // current - previous, View/Velocity.hpp), and the reservoirs the previous frame TRACED at the sample pixel there
    // (their visibility applied, written by "MegaLights: Trace Samples") are merged into this frame's, each re-weighed by the target function at THIS pixel
    // and its confidence capped at Shadows.MegaLightsMaxHistory frames of the current one (so a light that moved, or
    // a shadow that arrived, is forgotten in that many frames). A disocclusion (the previous frame's surface there is
    // another one: its normal or its depth differs) or an unreadable history keeps this frame's reservoirs alone.

    Compute
    {
        Uniform sampler PointClampSampler;
        #define MEGALIGHTS_SHADING
        #include <Common/MegaLights.glslh>

        Uniform(10) texture2D ReservoirsTexture;        // "MegaLights: Generate Samples"
        #define ReservoirsTexture sampler2D(ReservoirsTexture, PointClampSampler)
        Uniform(12) texture2D HistoryReservoirsTexture; // the previous frame's traced reservoirs (stacked layers)
        #define HistoryReservoirsTexture sampler2D(HistoryReservoirsTexture, PointClampSampler)
        Uniform(13) texture2D HistorySurfaceTexture;    // the previous frame's representatives: xyz = normal, w = clip w
                                                   // (0 = none); sample-pixel sized
        #define HistorySurfaceTexture sampler2D(HistorySurfaceTexture, PointClampSampler)
        Uniform(14) texture2D VelocityTexture;          // NDC current - previous, unjittered
        #define VelocityTexture sampler2D(VelocityTexture, PointClampSampler)
        layout(binding = 11, rgba32f) writeonly uniform image2D OutReservoirsTexture;

        // The surface the previous frame shaded at the reprojected pixel is this pixel's (UE MegaLights / RTXDI's
        // disocclusion test): the normals within ~25 degrees, the depth within 10 %.
        const float kNormalThreshold = 0.9;
        const float kDepthThreshold  = 0.1;

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

        	// The reprojected pixel, when the previous frame shaded this surface there.
        	bool  reprojected = false;
        	ivec2 previous    = ivec2(0);
        	if (p.Valid && Params.y > 0.5 && Params.x > 0.0)
        	{
        		const vec4 clip = ViewProjection * vec4(p.Position, 1.0);
        		if (clip.w > 0.0)
        		{
        			const vec2 prevNdc = clip.xy / clip.w - texelFetch(VelocityTexture, screen, 0).xy;
        			const vec2 prevUV  = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5); // Y-flipped
        			if (prevUV.x >= 0.0 && prevUV.x < 1.0 && prevUV.y >= 0.0 && prevUV.y < 1.0)
        			{
        				previous                = ivec2(prevUV * vec2(size)); // the previous sample pixel
        				const vec4  surface     = texelFetch(HistorySurfaceTexture, previous, 0);
        				const float expected    = (PreviousViewProjection * vec4(p.Position, 1.0)).w;
        				reprojected = surface.w > 0.0 && dot(surface.xyz, p.N) > kNormalThreshold &&
        				              abs(surface.w - expected) <= kDepthThreshold * expected;
        			}
        		}
        	}

        	uint random = MegaLightsSeed(pixel, 1u);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const MegaLightsReservoir current =
        		     MegaLightsUnpackReservoir(texelFetch(ReservoirsTexture, MegaLightsReservoirTexel(pixel, k), 0));
        		if (!reprojected)
        		{
        			imageStore(OutReservoirsTexture, MegaLightsReservoirTexel(pixel, k), MegaLightsPackReservoir(current));
        			continue;
        		}
        		MegaLightsMerge merge = MegaLightsBeginMerge();
        		MegaLightsMergeReservoir(merge, current, current.TargetPdf, current.M, MegaLightsRandom(random));
        		const MegaLightsReservoir history =
        		     MegaLightsUnpackReservoir(texelFetch(HistoryReservoirsTexture, MegaLightsReservoirTexel(previous, k), 0));
        		// The history's confidence, capped relative to this frame's (ReSTIR's M clamp).
        		const float historyM = min(history.M, Params.x * max(current.M, 1.0));
        		if (history.Light >= 0 && history.Light < MegaLightsLightCount())
        			MegaLightsMergeReservoir(merge, history, MegaLightsTargetPdf(p, history.Light), historyM,
        			                         MegaLightsRandom(random));
        		else
        			merge.Result.M += historyM; // an empty history still says how often nothing was found
        		imageStore(OutReservoirsTexture, MegaLightsReservoirTexel(pixel, k),
        		           MegaLightsPackReservoir(MegaLightsEndMerge(merge)));
        	}
        }
    }
}
