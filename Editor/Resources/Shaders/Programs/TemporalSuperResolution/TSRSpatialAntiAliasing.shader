// DesertAsset {"Kind":"Shader","Guid":"4ed7c3750cd5483fa36fdd2211f420de","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRSpatialAntiAliasing"
{
    Compute
    {
        // TSR.SpatialAntiAliasing. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/
        // TSRSpatialAntiAliasing.usf / .ush in structure: where the history is rejected the new history starts from
        // the aliased input alone, so UpdateHistory samples the input at a sub-pixel offset across the edge the
        // pixel sits on (UE SpatialAntiAliasingOffset, applied in TSRUpdateHistory.usf :1093-1100). Adapted: the
        // edge orientation and its end points are found with an FXAA-style luma search in the guide colour space
        // (horizontal vs vertical second derivative, a walk along the edge until the luma pair leaves the edge's
        // gradient by a quarter), not UE's own search kernel; REMAINDER-TSR.md names the difference.
        //
        // u_RejectionAntiAliasingSteps (r.TSR.RejectionAntiAliasingQuality 0 / 1 / 2 -> 0 / 4 / 8): how far the
        // walk goes each way; 0 writes a zero offset everywhere (the pass still runs, so UpdateHistory's bindings
        // never change with the quality). Writes u_AntiAliasing.xy: the offset in input pixels; zero where the
        // history is kept (rejection > 3/4, where UpdateHistory's lerp to the offset is 0 anyway).

        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_SceneColor;       // linear HDR, RenderExtent, jittered
        Uniform(2) sampler2D u_Exposure;         // 1x1 previous adapted luminance
        Uniform(3) sampler2D u_HistoryRejection; // TSR.RejectShading output
        layout(binding = 4, rgba16f) restrict writeonly uniform image2D u_AntiAliasing;

        float LumaAt(ivec2 p, float exposure)
        {
            return TsrLuma(LinearToGCS(max(texelFetch(u_SceneColor, TsrClampRender(p), 0).rgb, vec3(0.0)) * exposure));
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;

            const int  kSteps    = int(u_RejectionAntiAliasingSteps);
            const vec4 rejection = texelFetch(u_HistoryRejection, pixel, 0);
            if (kSteps <= 0 || (rejection.w < 0.5 && rejection.x > 0.75))
            {
                imageStore(u_AntiAliasing, pixel, vec4(0.0));
                return;
            }

            const float exposure = TsrExposure(u_Exposure);
            const float c        = LumaAt(pixel, exposure);
            const float n        = LumaAt(pixel + ivec2(0, -1), exposure);
            const float s        = LumaAt(pixel + ivec2(0, 1), exposure);
            const float e        = LumaAt(pixel + ivec2(1, 0), exposure);
            const float w        = LumaAt(pixel + ivec2(-1, 0), exposure);
            const float ne       = LumaAt(pixel + ivec2(1, -1), exposure);
            const float nw       = LumaAt(pixel + ivec2(-1, -1), exposure);
            const float se       = LumaAt(pixel + ivec2(1, 1), exposure);
            const float sw       = LumaAt(pixel + ivec2(-1, 1), exposure);

            const float range = max(c, max(max(n, s), max(e, w))) - min(c, min(min(n, s), min(e, w)));
            if (range < max(kQuantizationError * 4.0, c * 0.0625))
            {
                imageStore(u_AntiAliasing, pixel, vec4(0.0));
                return;
            }

            // A horizontal edge has its second derivative across y.
            const float edgeH = abs(nw + sw - 2.0 * w) + 2.0 * abs(n + s - 2.0 * c) + abs(ne + se - 2.0 * e);
            const float edgeV = abs(nw + ne - 2.0 * n) + 2.0 * abs(w + e - 2.0 * c) + abs(sw + se - 2.0 * s);
            const bool  horizontal = edgeH >= edgeV;

            // The side of the edge: the neighbour across it with the larger gradient.
            const float lumaA    = horizontal ? n : w;
            const float lumaB    = horizontal ? s : e;
            const bool  towardA  = abs(lumaA - c) >= abs(lumaB - c);
            const ivec2 across   = horizontal ? ivec2(0, towardA ? -1 : 1) : ivec2(towardA ? -1 : 1, 0);
            const ivec2 along    = horizontal ? ivec2(1, 0) : ivec2(0, 1);
            const float gradient = abs((towardA ? lumaA : lumaB) - c);
            const float edgeLuma = 0.5 * (c + (towardA ? lumaA : lumaB));

            // Walk along the edge both ways until the luma pair straddling it no longer matches the edge.
            int  negative = kSteps;
            int  positive = kSteps;
            bool negativeEndsInside = false;
            bool positiveEndsInside = false;
            for (int i = 1; i <= kSteps; ++i)
            {
                const ivec2 q    = pixel - along * i;
                const float pair = 0.5 * (LumaAt(q, exposure) + LumaAt(q + across, exposure));
                if (abs(pair - edgeLuma) >= gradient * 0.25)
                {
                    negative           = i;
                    negativeEndsInside = (pair - edgeLuma < 0.0) != (c - edgeLuma < 0.0);
                    break;
                }
            }
            for (int i = 1; i <= kSteps; ++i)
            {
                const ivec2 q    = pixel + along * i;
                const float pair = 0.5 * (LumaAt(q, exposure) + LumaAt(q + across, exposure));
                if (abs(pair - edgeLuma) >= gradient * 0.25)
                {
                    positive           = i;
                    positiveEndsInside = (pair - edgeLuma < 0.0) != (c - edgeLuma < 0.0);
                    break;
                }
            }

            // The closer end decides: the pixel is covered by the edge's staircase only when that end turns away
            // from this pixel's side (FXAA's correct-variation test), then the offset grows toward the step.
            const bool  negativeCloser = negative < positive;
            const bool  endsInside     = negativeCloser ? negativeEndsInside : positiveEndsInside;
            const float edgeLength     = float(negative + positive);
            const float pixelOffset    = endsInside ? 0.5 - float(min(negative, positive)) / edgeLength : 0.0;
            imageStore(u_AntiAliasing, pixel, vec4(vec2(across) * pixelOffset, 0.0, 0.0));
        }
    }
}
