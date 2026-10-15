// DesertAsset {"Kind":"Shader","Guid":"a187cddc15cb4fa886534def869569e8","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRRejectShading"
{
    Compute
    {
        // TSR.RejectShading. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/TSRRejectShading.usf
        // (the pass, its outputs :859-910) and TSRShadingAnalysis.ush MeasureRejection :496-644 (the clamp of the
        // 3x3-blurred reprojected guide into the min/max box of the 3x3-blurred input, widened by the quantization
        // and moire errors; rejection = 1 - clamped energy / delta, median-filtered) and the flickering history
        // :263-395, adapted: the convolutions run on a 16x16 shared-memory tile per 8x8 group instead of UE's
        // tensor registers / wave ops; no thin-geometry relaxation; the Blur3x3(TotalVariation) term of ClampError
        // is left out (REMAINDER-TSR.md); the new guide is the input itself.
        //
        // In UE's guide colour space (TSRColorSpace.ush: c / (c + 0.17) of the pre-exposed colour). Writes:
        //   u_HistoryRejection: x LowFrequencyRejection (UE RejectionBlendFinal), y DisableHistoryClamp (UE
        //                       RejectionClampBlend), z DecreaseValidityMultiplier, w 1 when off screen or parallax
        //                       disoccluded (all three rejections then 0 / 0 / 1);
        //   u_GuideOut / u_MoireOut: this frame's guide and moire histories.
        //
        // u_EnableFlickeringHeuristic (r.TSR.ShadingRejection.Flickering; a uniform as in UE, not a permutation):
        // 1 tracks the luma oscillation of each pixel over u_FlickeringFramePeriod frames and widens the clamp box
        // by it (UE bEnableMoireError), so a flickering detail keeps its history instead of being rejected every
        // frame; 0 writes a reset moire history.

        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_SceneColor;       // linear HDR, RenderExtent, jittered
        Uniform(2) sampler2D u_Exposure;         // 1x1 previous adapted luminance
        Uniform(3) sampler2D u_ReprojectedGuide; // rgb guide (GCS), a validity
        Uniform(4) sampler2D u_ReprojectedMoire; // x flicker luma, y gradient, z total variation, w validity
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D u_HistoryRejection;
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D u_GuideOut;
        layout(binding = 7, rgba16f) restrict writeonly uniform image2D u_MoireOut;

        const int kTile   = 8;
        const int kApron  = 4; // Blur3x3 -> MinMax3x3 -> Median3x3 -> Min/Max3x3
        const int kLoad   = kTile + 2 * kApron; // 16
        const int kBlur   = kLoad - 2;          // 14
        const int kRaw    = kLoad - 4;          // 12
        const int kMedian = kLoad - 6;          // 10

        shared vec3 s_Input[kLoad * kLoad];
        shared vec3 s_History[kLoad * kLoad];
        shared vec3 s_BlurInput[kBlur * kBlur];
        shared vec3 s_BlurHistory[kBlur * kBlur];
        shared vec4 s_Raw[kRaw * kRaw];   // rgb RawClampedEnergy, a RawRejection
        shared vec3 s_Delta[kRaw * kRaw];
        shared vec4 s_Median[kMedian * kMedian];

        void Sort(inout vec4 a, inout vec4 b)
        {
            const vec4 lo = min(a, b);
            b             = max(a, b);
            a             = lo;
        }

        // Per-channel median of nine (the opt_med9 exchange network on component-wise min/max).
        vec4 Median9(vec4 p[9])
        {
            Sort(p[1], p[2]); Sort(p[4], p[5]); Sort(p[7], p[8]);
            Sort(p[0], p[1]); Sort(p[3], p[4]); Sort(p[6], p[7]);
            Sort(p[1], p[2]); Sort(p[4], p[5]); Sort(p[7], p[8]);
            Sort(p[0], p[3]); Sort(p[5], p[8]); Sort(p[4], p[7]);
            Sort(p[3], p[6]); Sort(p[1], p[4]); Sort(p[2], p[5]);
            Sort(p[4], p[7]); Sort(p[4], p[2]); Sort(p[6], p[4]);
            Sort(p[4], p[2]);
            return p[4];
        }

        float MoireErrorAt(ivec2 p)
        {
            const vec4 moire = texelFetch(u_ReprojectedMoire, TsrClampRender(p), 0);
            return u_EnableFlickeringHeuristic > 0.5 && moire.w > 0.5 ? moire.z : 0.0;
        }

        void main()
        {
            const ivec2 local    = ivec2(gl_LocalInvocationID.xy);
            const int   lane     = local.y * kTile + local.x;
            const ivec2 origin   = ivec2(gl_WorkGroupID.xy) * kTile - kApron;
            const float exposure = TsrExposure(u_Exposure);

            // 1. The input and the reprojected guide of the 16x16 tile, in the guide colour space.
            for (int i = lane; i < kLoad * kLoad; i += kTile * kTile)
            {
                const ivec2 p = TsrClampRender(origin + ivec2(i % kLoad, i / kLoad));
                s_Input[i]    = LinearToGCS(max(texelFetch(u_SceneColor, p, 0).rgb, vec3(0.0)) * exposure);
                s_History[i]  = texelFetch(u_ReprojectedGuide, p, 0).rgb;
            }
            barrier();

            // 2. Blur3x3 of both (UE: 1, 0.5 cross, 0.25 diagonal, normalised by FilteringWeight).
            for (int i = lane; i < kBlur * kBlur; i += kTile * kTile)
            {
                const ivec2 c   = ivec2(i % kBlur, i / kBlur) + 1;
                vec3        sIn = vec3(0.0);
                vec3        sHi = vec3(0.0);
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        const float w = (x == 0 ? 1.0 : 0.5) * (y == 0 ? 1.0 : 0.5);
                        const int   j = (c.y + y) * kLoad + c.x + x;
                        sIn += w * s_Input[j];
                        sHi += w * s_History[j];
                    }
                }
                s_BlurInput[i]   = sIn * kFilteringWeight;
                s_BlurHistory[i] = sHi * kFilteringWeight;
            }
            barrier();

            // 3. MeasureRejection's raw factors on the 12x12 inner region.
            for (int i = lane; i < kRaw * kRaw; i += kTile * kTile)
            {
                const ivec2 c = ivec2(i % kRaw, i / kRaw) + 2; // in the 16x16 tile
                const ivec2 b = c - 1;                          // in the 14x14 blurred tile

                vec3 inputMin = s_Input[c.y * kLoad + c.x];
                vec3 inputMax = inputMin;
                vec3 boxMin   = s_BlurInput[b.y * kBlur + b.x];
                vec3 boxMax   = boxMin;
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        const vec3 raw     = s_Input[(c.y + y) * kLoad + c.x + x];
                        const vec3 blurred = s_BlurInput[(b.y + y) * kBlur + b.x + x];
                        inputMin           = min(inputMin, raw);
                        inputMax           = max(inputMax, raw);
                        boxMin             = min(boxMin, blurred);
                        boxMax             = max(boxMax, blurred);
                    }
                }
                const vec3 inputBoxSize    = inputMax - inputMin;
                const vec3 quantization    = vec3(kQuantizationError);
                const vec3 clampError      = max(quantization, inputBoxSize * (kFilteringWeight * 0.25)) + quantization;
                const vec3 filteredInput   = s_BlurInput[b.y * kBlur + b.x];
                const vec3 filteredHistory = s_BlurHistory[b.y * kBlur + b.x];
                boxMin -= clampError;
                boxMax += clampError;

                vec3        clamped     = clamp(filteredHistory, boxMin, boxMax);
                vec3        boxSize     = inputBoxSize * kFilteringWeight + quantization * (2.0 * kFilteringWeight);
                const float moireError  = MoireErrorAt(origin + c);
                if (moireError > 0.0)
                {
                    const vec3 moireSize = vec3(kMoireLumaToChannel * moireError);
                    clamped = clamp(filteredHistory, min(boxMin, boxMax - moireSize), max(boxMax, boxMin + moireSize));
                    const vec3  energy   = abs(clamped - filteredHistory);
                    const float sum      = energy.r + energy.g + energy.b;
                    const vec3  amount   = sum > 0.0 ? energy / sum : vec3(0.0);
                    boxSize              = max(boxSize, amount * (kFilteringWeight * 3.0 * moireError));
                }

                const vec3 delta       = max(abs(filteredInput - filteredHistory), boxSize);
                const vec3 rawEnergy   = abs(clamped - filteredHistory);
                const vec3 rawFactor   = clamp(1.0 - rawEnergy / delta, 0.0, 1.0);
                s_Raw[i]   = vec4(rawEnergy, min(rawFactor.r, min(rawFactor.g, rawFactor.b)));
                s_Delta[i] = delta;
            }
            barrier();

            // 4. Median3x3 of (energy, rejection) on the 10x10 region.
            for (int i = lane; i < kMedian * kMedian; i += kTile * kTile)
            {
                const ivec2 c = ivec2(i % kMedian, i / kMedian) + 1; // in the 12x12 raw tile
                vec4        taps[9];
                for (int t = 0; t < 9; ++t)
                    taps[t] = s_Raw[(c.y + t / 3 - 1) * kRaw + c.x + t % 3 - 1];
                s_Median[i] = Median9(taps);
            }
            barrier();

            // 5. UE MaxRGBMinA3x3: the largest clamped energy and the smallest rejection of the 3x3.
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;
            const ivec2 m              = local + 1; // in the 10x10 median tile
            vec3        filteredEnergy = vec3(0.0);
            float       clampBlend     = 1.0;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const vec4 median = s_Median[(m.y + y) * kMedian + m.x + x];
                    filteredEnergy    = max(filteredEnergy, median.rgb);
                    clampBlend        = min(clampBlend, median.a);
                }
            }
            const ivec2 r              = local + 2; // in the 12x12 raw tile
            const vec3  filteredFactor = clamp(1.0 - filteredEnergy / s_Delta[r.y * kRaw + r.x], 0.0, 1.0);
            const float blendFinal     = min(filteredFactor.r, min(filteredFactor.g, filteredFactor.b));

            const bool valid = texelFetch(u_ReprojectedGuide, pixel, 0).a > 0.5;
            imageStore(u_HistoryRejection, pixel,
                       valid ? vec4(blendFinal, clampBlend, 1.0 - blendFinal, 0.0) : vec4(0.0, 0.0, 1.0, 1.0));

            const vec3 current = s_Input[(local.y + kApron) * kLoad + local.x + kApron];
            imageStore(u_GuideOut, pixel, vec4(current, 1.0));

            // The flickering history (UE TSRShadingAnalysis.ush :263-395): the luma's running mean over the period,
            // the last frame-to-frame gradient, and the total variation of gradients that changed sign.
            const float luma = TsrLuma(GCSToSMCS(current));
            const vec4 prev  = texelFetch(u_ReprojectedMoire, pixel, 0);
            vec4       moire = vec4(luma, 0.0, 0.0, 1.0);
            if (u_EnableFlickeringHeuristic > 0.5 && valid && prev.w > 0.5)
            {
                const float blend     = 1.0 / max(u_FlickeringFramePeriod, 1.0);
                const float gradient  = luma - prev.x;
                const bool  flickers  = gradient * prev.y < 0.0 && abs(gradient) > kQuantizationError &&
                                        abs(prev.y) > kQuantizationError;
                const float variation = flickers ? min(abs(gradient), abs(prev.y)) : 0.0;
                moire = vec4(mix(prev.x, luma, blend), gradient, max(variation, prev.z * (1.0 - blend * 0.25)), 1.0);
            }
            imageStore(u_MoireOut, pixel, moire);
        }
    }
}
