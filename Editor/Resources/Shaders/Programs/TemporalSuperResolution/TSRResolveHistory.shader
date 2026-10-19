// DesertAsset {"Kind":"Shader","Guid":"6f6c203d33f0435db2ce3f82f9d3ae14","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRResolveHistory"
{
    Compute
    {
        // TSR.ResolveHistory. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/TSRResolveHistory.usf
        // in what it computes: the output pixel as the tone-weighted (HdrWeight) average of the history pixels
        // around it under UE's 1 - 1.9 x^2 + 0.905 x^4 kernel one OUTPUT pixel wide, adapted: a 4x4 history
        // footprint (the history is at most 2x the output, r.TSR.History.ScreenPercentage <= 200), no sharpening, no
        // output mips. Only added when the history is larger than the output (TSR_WRITE_OUTPUT 0 in UpdateHistory).
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_Exposure; // 1x1 previous adapted luminance
        Uniform(2) sampler2D u_History;  // this frame's TSR.History.Color, HistoryExtent
        layout(binding = 3, rgba16f) restrict writeonly uniform image2D u_Output;

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_OutputSize))))
                return;

            const float exposure      = TsrExposure(u_Exposure);
            const vec2  historyPos    = (vec2(pixel) + 0.5) / u_OutputSize * u_HistorySize;
            const ivec2 base          = ivec2(floor(historyPos - 0.5)) - 1;
            const vec2  historyToOutput = u_OutputSize / u_HistorySize;

            vec3  sum       = vec3(0.0);
            float weightSum = 0.0;
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 4; ++x)
                {
                    const ivec2 q = clamp(base + ivec2(x, y), ivec2(0), ivec2(u_HistorySize) - 1);
                    const vec2  d = ((vec2(base + ivec2(x, y)) + 0.5) - historyPos) * historyToOutput;
                    const vec3  c = texelFetch(u_History, q, 0).rgb * exposure;
                    const float w = TsrSampleWeight(1.0, d, 0.0) * TsrHdrWeight(c);
                    sum += c * w;
                    weightSum += w;
                }
            }
            const vec3 nearest = texelFetch(u_History, clamp(ivec2(historyPos), ivec2(0), ivec2(u_HistorySize) - 1), 0).rgb;
            imageStore(u_Output, pixel, vec4(weightSum > 0.0 ? sum / (weightSum * exposure) : nearest, 1.0));
        }
    }
}
