// DesertAsset {"Kind":"Shader","Guid":"a3a399c19dc94e1eb0192710a9e3a45d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MegaLightsTemporalResampling"
{
    // "MegaLights: Temporal Resampling" (ReSTIR DI temporal reuse; UE MegaLights' history through the velocity): the
    // pixel's surface is reprojected into the view's previous frame through the velocity (NDC current - previous,
    // View/Velocity.hpp), and the reservoirs the previous frame SHADED there (after its visibility, written by
    // "MegaLights: Shade Samples") are merged into this frame's, each re-weighed by the target function at THIS pixel
    // and its confidence capped at Shadows.MegaLightsMaxHistory frames of the current one (so a light that moved, or
    // a shadow that arrived, is forgotten in that many frames). A disocclusion (the previous frame's surface there is
    // another one: its normal or its depth differs) or an unreadable history keeps this frame's reservoirs alone.

    Compute
    {
        Uniform sampler u_PointClampSampler;
        #define MEGALIGHTS_SHADING
        #include <Common/MegaLights.glslh>

        Uniform(10) texture2D u_Reservoirs;        // "MegaLights: Trace Samples"
        #define u_Reservoirs sampler2D(u_Reservoirs, u_PointClampSampler)
        Uniform(12) texture2D u_HistoryReservoirs; // the previous frame's shaded reservoirs (stacked layers)
        #define u_HistoryReservoirs sampler2D(u_HistoryReservoirs, u_PointClampSampler)
        Uniform(13) texture2D u_HistorySurface;    // the previous frame's surface: xyz = normal, w = clip w (0 = none)
        #define u_HistorySurface sampler2D(u_HistorySurface, u_PointClampSampler)
        Uniform(14) texture2D u_Velocity;          // NDC current - previous, unjittered
        #define u_Velocity sampler2D(u_Velocity, u_PointClampSampler)
        layout(binding = 11, rgba32f) writeonly uniform image2D u_OutReservoirs;

        // The surface the previous frame shaded at the reprojected pixel is this pixel's (UE MegaLights / RTXDI's
        // disocclusion test): the normals within ~25 degrees, the depth within 10 %.
        const float kNormalThreshold = 0.9;
        const float kDepthThreshold  = 0.1;

        LocalSize(8, 8, 1);

        void main()
        {
        	const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
        	const ivec2 size  = MegaLightsViewSize();
        	if (pixel.x >= size.x || pixel.y >= size.y)
        		return;
        	const uint            samples = MegaLightsSamplesPerPixel();
        	const MegaLightsPixel p       = MegaLightsLoadPixel(pixel);

        	// The reprojected pixel, when the previous frame shaded this surface there.
        	bool  reprojected = false;
        	ivec2 previous    = ivec2(0);
        	if (p.Valid && u_Params.y > 0.5 && u_Params.x > 0.0)
        	{
        		const vec4 clip = u_ViewProjection * vec4(p.Position, 1.0);
        		if (clip.w > 0.0)
        		{
        			const vec2 prevNdc = clip.xy / clip.w - texelFetch(u_Velocity, pixel, 0).xy;
        			const vec2 prevUV  = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5); // Y-flipped
        			if (prevUV.x >= 0.0 && prevUV.x < 1.0 && prevUV.y >= 0.0 && prevUV.y < 1.0)
        			{
        				previous                = ivec2(prevUV * vec2(size));
        				const vec4  surface     = texelFetch(u_HistorySurface, previous, 0);
        				const float expected    = (u_PrevViewProjection * vec4(p.Position, 1.0)).w;
        				reprojected = surface.w > 0.0 && dot(surface.xyz, p.N) > kNormalThreshold &&
        				              abs(surface.w - expected) <= kDepthThreshold * expected;
        			}
        		}
        	}

        	uint random = MegaLightsSeed(pixel, 1u);
        	for (uint k = 0u; k < samples; ++k)
        	{
        		const MegaLightsReservoir current =
        		     MegaLightsUnpackReservoir(texelFetch(u_Reservoirs, MegaLightsReservoirTexel(pixel, k), 0));
        		if (!reprojected)
        		{
        			imageStore(u_OutReservoirs, MegaLightsReservoirTexel(pixel, k), MegaLightsPackReservoir(current));
        			continue;
        		}
        		MegaLightsMerge merge = MegaLightsBeginMerge();
        		MegaLightsMergeReservoir(merge, current, current.TargetPdf, current.M, MegaLightsRandom(random));
        		const MegaLightsReservoir history =
        		     MegaLightsUnpackReservoir(texelFetch(u_HistoryReservoirs, MegaLightsReservoirTexel(previous, k), 0));
        		// The history's confidence, capped relative to this frame's (ReSTIR's M clamp).
        		const float historyM = min(history.M, u_Params.x * max(current.M, 1.0));
        		if (history.Light >= 0 && history.Light < MegaLightsLightCount())
        			MegaLightsMergeReservoir(merge, history, MegaLightsTargetPdf(p, history.Light), historyM,
        			                         MegaLightsRandom(random));
        		else
        			merge.Result.M += historyM; // an empty history still says how often nothing was found
        		imageStore(u_OutReservoirs, MegaLightsReservoirTexel(pixel, k),
        		           MegaLightsPackReservoir(MegaLightsEndMerge(merge)));
        	}
        }
    }
}
