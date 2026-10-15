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
        // tensor registers / wave ops; the Blur3x3(TotalVariation) term of ClampError is left out (REMAINDER-TSR.md);
        // the new guide is the input itself.
        //
        // THIN GEOMETRY (TSR_THIN_GEOMETRY, the project's r.TSR.ThinGeometryDetection; UE DIM_THIN_GEOMETRY_DETECTION,
        // TSRRejectShading.usf FetchThinGeometryEdge :123-140 and :497-505 / :644-666, TSRShadingAnalysis.ush
        // MeasureRejection :530-560 and ComputeMoireError :416-424): u_ThinGeometry is TSR.DetectThinGeometry's
        // weight (UE R8Output[ThinGeometryTextureIndex]: weight * 127 << 1 | cluster-hole bit). Where it is above
        // 1/127 the clamp box of the filtered input is relaxed toward the 3x3 box of the filtered history by that
        // weight, then replaced by the 3x3 box of the history clamped into the relaxed box, so a sub-pixel wire or
        // leaf that the input catches only on some frames keeps its history instead of being rejected. That last
        // Min/Max3x3 is one more apron: the group writes a 6x6 tile (apron 5) instead of 8x8 (apron 4), from the
        // same 16x16 load. The flickering history counts a thin pixel's small gradients as flicker (UE
        // r.TSR.ThinGeometryDetection.AntiFlickering 1, its default). The resurrected frame is measured without the
        // relaxation, as in UE (:776).
        //
        // HISTORY RESURRECTION (r.TSR.Resurrection; UE IsResurrectedFrameCloserThanPreviousFrame :263,
        // IsWorthMeasuringRejectionOfResurrectedFrame :286, ShouldResurrectHistory :308, the pass :716-790): where
        // the resurrected guide (TSR.DecimateHistory, from the persistent frame) is closer to the input than the
        // reprojected one over a 3x3 of 3x3 sums, and the reprojected one is rejected (< 0.5) anywhere in the group,
        // the rejection is measured again against the resurrected guide; a pixel whose rejection improves by more
        // than 0.1 takes the resurrected rejection and is flagged resurrected (bit 1 of the mask), and
        // TSR.UpdateHistory then reprojects the persistent colour frame instead of the previous one. Adapted: UE
        // overwrites the reprojection field here (OverwriteReprojectionFieldWithResurrection :334); UpdateHistory
        // computes the resurrection reprojection itself from the closest depth instead, so the field stays one
        // pass's output.
        //
        // SPATIAL ANTI-ALIASING INPUT (UE ComputeSpatialAntiAliaserLumaLDR / IsAliasingVisible /
        // ShouldSpatialAntiAlias, TSRShadingAnalysis.ush :171-212): u_AntiAliasInput.x the input's LDR luma
        // l / (0.5 + l), .y 1 where TSR.SpatialAntiAliasing must run.
        //
        // In UE's guide colour space (TSRColorSpace.ush: c / (c + 0.17) of the pre-exposed colour). Writes:
        //   u_HistoryRejection: x LowFrequencyRejection (UE RejectionBlendFinal), y DisableHistoryClamp (UE
        //                       RejectionClampBlend), z DecreaseValidityMultiplier, w the bit mask / 255 (TSRCommon
        //                       kRejectionNotDisoccluded / kRejectionResurrected); a pixel off screen or parallax
        //                       disoccluded and not resurrected has 0 / 0 / 1;
        //   u_GuideOut / u_MoireOut: this frame's guide and moire histories;
        //   u_AntiAliasInput: above.
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
        Uniform(5) sampler2D u_ResurrectedGuide; // rgb guide (GCS), a 1 on screen in the resurrection frame
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D u_HistoryRejection;
        layout(binding = 7, rgba16f) restrict writeonly uniform image2D u_GuideOut;
        layout(binding = 8, rgba16f) restrict writeonly uniform image2D u_MoireOut;
        layout(binding = 9, rgba16f) restrict writeonly uniform image2D u_AntiAliasInput;

        #ifndef TSR_THIN_GEOMETRY
        #define TSR_THIN_GEOMETRY 0
        #endif

        #if TSR_THIN_GEOMETRY
        Uniform(10) usampler2D u_ThinGeometry; // TSR.DetectThinGeometry: weight * 127 << 1 | cluster-hole bit
        const int kTile  = 6;
        const int kApron = 5; // Blur3x3 -> MinMax3x3 -> lerp-clamped history's MinMax3x3 -> Median3x3 -> Min/Max3x3
        #else
        const int kTile  = 8;
        const int kApron = 4; // Blur3x3 -> MinMax3x3 -> Median3x3 -> Min/Max3x3
        #endif
        const int kLanes      = 8; // LocalSize per side; a lane past kTile only loads and filters
        const int kLaneCount  = kLanes * kLanes;
        const int kLoad       = kTile + 2 * kApron;     // 16
        const int kBlur       = kLoad - 2;              // 14, at 1 in the load tile
        const int kRawOffset  = kApron - 2;             // the raw factors' offset in the load tile: 2, or 3 thin
        const int kRaw        = kLoad - 2 * kRawOffset; // 12, or 10 thin
        const int kMedian     = kRaw - 2;               // 10, or 8 thin
        const int kMatch      = kTile + 2;              // the match sums' region, at kApron - 1 in the load tile
        const int kLerpClamp  = kBlur - 2;              // the lerp-clamped history's region (thin), at 2

        shared vec3  s_Input[kLoad * kLoad];
        shared vec3  s_History[kLoad * kLoad];
        shared vec3  s_Resurrected[kLoad * kLoad]; // the resurrected guide, GCS
        shared vec3  s_BlurInput[kBlur * kBlur];
        shared vec3  s_BlurHistory[kBlur * kBlur];
        shared vec4  s_Raw[kRaw * kRaw];   // rgb RawClampedEnergy, a RawRejection
        shared vec3  s_Delta[kRaw * kRaw];
        shared vec4  s_Median[kMedian * kMedian];
        shared float s_MatchSum[kMatch * kMatch]; // UE TotalMatchDiff (Sum3x3 of MatchDiff) around the output tile
        #if TSR_THIN_GEOMETRY
        shared vec3 s_LerpClamped[kLerpClamp * kLerpClamp]; // the filtered history clamped into the relaxed box
        #endif
        shared uint  s_WantResurrection;

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

        #if TSR_THIN_GEOMETRY
        // UE FetchThinGeometryEdge's HistoryRelaxationWeight, with MeasureRejection's cut at one code (:533).
        float ThinGeometryRelaxationAt(ivec2 p)
        {
            const float weight = float(texelFetch(u_ThinGeometry, TsrClampRender(p), 0).r >> 1u) * (1.0 / 127.0);
            return weight > 1.0 / 127.0 ? weight : 0.0;
        }
        #endif

        float MoireErrorAt(ivec2 p)
        {
            const vec4 moire = texelFetch(u_ReprojectedMoire, TsrClampRender(p), 0);
            return u_EnableFlickeringHeuristic > 0.5 && moire.w > 0.5 ? moire.z : 0.0;
        }

        // UE MeasureRejection over the group's tile against s_History (or s_Resurrected): Blur3x3 -> clamp box ->
        // Median3x3 -> MaxRGBMinA3x3. Every lane calls it (it has barriers); the result is this lane's pixel.
        // @p withMoire: the flicker error widens the box (UE passes MoireError 0 for the resurrected frame).
        // @p tileLocal: this lane's output pixel in the tile (a lane past kTile reads the last one, writes none).
        void MeasureRejectionTile(ivec2 tileLocal, int lane, ivec2 origin, bool resurrected, bool withMoire,
                                  out float blendFinal, out float clampBlend)
        {
            // 1. Blur3x3 of both (UE: 1, 0.5 cross, 0.25 diagonal, normalised by FilteringWeight).
            for (int i = lane; i < kBlur * kBlur; i += kLaneCount)
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
                        sHi += w * (resurrected ? s_Resurrected[j] : s_History[j]);
                    }
                }
                s_BlurInput[i]   = sIn * kFilteringWeight;
                s_BlurHistory[i] = sHi * kFilteringWeight;
            }
            barrier();

        #if TSR_THIN_GEOMETRY
            // 1b. UE MeasureRejection :530-547 (thin geometry; never for the resurrected frame): the filtered input's
            // 3x3 box lerped toward the filtered history's 3x3 box by the relaxation weight, and the filtered
            // history clamped into it, on the 12x12 region the raw factors' Min/Max3x3 below reads.
            for (int i = lane; i < kLerpClamp * kLerpClamp; i += kLaneCount)
            {
                const ivec2 c          = ivec2(i % kLerpClamp, i / kLerpClamp) + 2; // in the 16x16 tile
                const ivec2 b          = c - 1;                                     // in the 14x14 blurred tile
                const float relaxation = resurrected ? 0.0 : ThinGeometryRelaxationAt(origin + c);
                vec3        boxMin     = s_BlurInput[b.y * kBlur + b.x];
                vec3        boxMax     = boxMin;
                vec3        historyMin = s_BlurHistory[b.y * kBlur + b.x];
                vec3        historyMax = historyMin;
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        const int j = (b.y + y) * kBlur + b.x + x;
                        boxMin      = min(boxMin, s_BlurInput[j]);
                        boxMax      = max(boxMax, s_BlurInput[j]);
                        historyMin  = min(historyMin, s_BlurHistory[j]);
                        historyMax  = max(historyMax, s_BlurHistory[j]);
                    }
                }
                s_LerpClamped[i] = clamp(s_BlurHistory[b.y * kBlur + b.x], mix(boxMin, historyMin, relaxation),
                                         mix(boxMax, historyMax, relaxation));
            }
            barrier();
        #endif

            // 2. MeasureRejection's raw factors on the inner region (12x12, or 10x10 thin).
            for (int i = lane; i < kRaw * kRaw; i += kLaneCount)
            {
                const ivec2 c = ivec2(i % kRaw, i / kRaw) + kRawOffset; // in the 16x16 tile
                const ivec2 b = c - 1;                                   // in the 14x14 blurred tile

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
        #if TSR_THIN_GEOMETRY
                // UE :549-556: a relaxed pixel's box is the 3x3 box of the lerp-clamped history.
                if (!resurrected && ThinGeometryRelaxationAt(origin + c) > 0.0)
                {
                    const ivec2 l = c - 2; // in the 12x12 lerp-clamped tile
                    boxMin        = s_LerpClamped[l.y * kLerpClamp + l.x];
                    boxMax        = boxMin;
                    for (int y = -1; y <= 1; ++y)
                    {
                        for (int x = -1; x <= 1; ++x)
                        {
                            const vec3 clampedHistory = s_LerpClamped[(l.y + y) * kLerpClamp + l.x + x];
                            boxMin                    = min(boxMin, clampedHistory);
                            boxMax                    = max(boxMax, clampedHistory);
                        }
                    }
                }
        #endif
                const vec3 inputBoxSize    = inputMax - inputMin;
                const vec3 quantization    = vec3(kQuantizationError);
                const vec3 clampError      = max(quantization, inputBoxSize * (kFilteringWeight * 0.25)) + quantization;
                const vec3 filteredInput   = s_BlurInput[b.y * kBlur + b.x];
                const vec3 filteredHistory = s_BlurHistory[b.y * kBlur + b.x];
                boxMin -= clampError;
                boxMax += clampError;

                vec3        clamped     = clamp(filteredHistory, boxMin, boxMax);
                vec3        boxSize     = inputBoxSize * kFilteringWeight + quantization * (2.0 * kFilteringWeight);
                const float moireError  = withMoire ? MoireErrorAt(origin + c) : 0.0;
                if (moireError > 0.0)
                {
                    const vec3 moireSize = vec3(kMoireLumaToChannel * moireError);
                    clamped = clamp(filteredHistory, min(boxMin, boxMax - moireSize), max(boxMax, boxMin + moireSize));
                    const vec3  energy   = abs(clamped - filteredHistory);
                    const float sum      = energy.r + energy.g + energy.b;
                    const vec3  amount   = sum > 0.0 ? energy / sum : vec3(0.0);
                    boxSize              = max(boxSize, amount * (kFilteringWeight * 3.0 * moireError));
                }

                const vec3 delta     = max(abs(filteredInput - filteredHistory), boxSize);
                const vec3 rawEnergy = abs(clamped - filteredHistory);
                const vec3 rawFactor = clamp(1.0 - rawEnergy / delta, 0.0, 1.0);
                s_Raw[i]   = vec4(rawEnergy, min(rawFactor.r, min(rawFactor.g, rawFactor.b)));
                s_Delta[i] = delta;
            }
            barrier();

            // 3. Median3x3 of (energy, rejection) on the median region (10x10, or 8x8 thin).
            for (int i = lane; i < kMedian * kMedian; i += kLaneCount)
            {
                const ivec2 c = ivec2(i % kMedian, i / kMedian) + 1; // in the raw tile
                vec4        taps[9];
                for (int t = 0; t < 9; ++t)
                    taps[t] = s_Raw[(c.y + t / 3 - 1) * kRaw + c.x + t % 3 - 1];
                s_Median[i] = Median9(taps);
            }
            barrier();

            // 4. UE MaxRGBMinA3x3: the largest clamped energy and the smallest rejection of the 3x3.
            const ivec2 m              = tileLocal + 1; // in the median tile
            vec3        filteredEnergy = vec3(0.0);
            clampBlend                 = 1.0;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const vec4 median = s_Median[(m.y + y) * kMedian + m.x + x];
                    filteredEnergy    = max(filteredEnergy, median.rgb);
                    clampBlend        = min(clampBlend, median.a);
                }
            }
            const ivec2 r              = tileLocal + 2; // in the raw tile
            const vec3  filteredFactor = clamp(1.0 - filteredEnergy / s_Delta[r.y * kRaw + r.x], 0.0, 1.0);
            blendFinal                 = min(filteredFactor.r, min(filteredFactor.g, filteredFactor.b));
            // The tiles are reused by the next measure.
            barrier();
        }

        // UE IsResurrectedFrameCloserThanPreviousFrame's per-pixel term: how much closer the resurrected guide is
        // to the input than the reprojected one, summed over the channels, in the shading-measurement space.
        float MatchDiff(int j)
        {
            const vec3 shadingInput = GCSToSMCS(s_Input[j]);
            return dot(abs(shadingInput - GCSToSMCS(s_History[j])) - abs(shadingInput - GCSToSMCS(s_Resurrected[j])),
                       vec3(1.0));
        }

        void main()
        {
            const ivec2 local               = ivec2(gl_LocalInvocationID.xy);
            const int   lane                = local.y * kLanes + local.x;
            const ivec2 origin              = ivec2(gl_WorkGroupID.xy) * kTile - kApron;
            const float exposure            = TsrExposure(u_Exposure);
            // A lane past the output tile (thin geometry: 6x6 of 8x8) loads and filters, and writes nothing.
            const bool  ownsPixel           = all(lessThan(local, ivec2(kTile)));
            const ivec2 tileLocal           = min(local, ivec2(kTile - 1));
            const ivec2 pixel               = ivec2(gl_WorkGroupID.xy) * kTile + tileLocal;
            const bool  inside              = ownsPixel && all(lessThan(pixel, ivec2(u_RenderSize)));
            const bool  resurrectionEnabled = u_ResurrectionEnabled > 0.5;

            // 1. The input, the reprojected guide and the resurrected one of the 16x16 tile, in the guide space.
            for (int i = lane; i < kLoad * kLoad; i += kLaneCount)
            {
                const ivec2 p    = TsrClampRender(origin + ivec2(i % kLoad, i / kLoad));
                s_Input[i]       = LinearToGCS(max(texelFetch(u_SceneColor, p, 0).rgb, vec3(0.0)) * exposure);
                s_History[i]     = texelFetch(u_ReprojectedGuide, p, 0).rgb;
                s_Resurrected[i] = resurrectionEnabled ? texelFetch(u_ResurrectedGuide, p, 0).rgb : vec3(0.0);
            }
            if (lane == 0)
                s_WantResurrection = 0u;
            barrier();

            // 2. UE TotalMatchDiff = Sum3x3(MatchDiff), on the region around the tile the closer-test's Sum3x3 reads.
            if (resurrectionEnabled)
            {
                for (int i = lane; i < kMatch * kMatch; i += kLaneCount)
                {
                    const ivec2 c   = ivec2(i % kMatch, i / kMatch) + kApron - 1; // in the 16x16 tile
                    float       sum = 0.0;
                    for (int y = -1; y <= 1; ++y)
                        for (int x = -1; x <= 1; ++x)
                            sum += MatchDiff((c.y + y) * kLoad + c.x + x);
                    s_MatchSum[i] = sum;
                }
                barrier();
            }

            // 3. The rejection of the reprojected guide.
            float blendFinal;
            float clampBlend;
            MeasureRejectionTile(tileLocal, lane, origin, false, true, blendFinal, clampBlend);

            const ivec2 clampedPixel = TsrClampRender(pixel);
            const bool  valid        = texelFetch(u_ReprojectedGuide, clampedPixel, 0).a > 0.5;

            // 4. History resurrection (UE :716-790). u_ResurrectionEnabled is uniform, so are the barriers below.
            bool resurrect = false;
            if (resurrectionEnabled)
            {
                const ivec2 m      = tileLocal + 1; // in the match-sum tile
                float       closer = 0.0;
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x)
                        closer += s_MatchSum[(m.y + y) * kMatch + m.x + x] > 0.05 * 3.0 * 9.0 ? 1.0 : 0.0;
                const bool onScreen             = texelFetch(u_ResurrectedGuide, clampedPixel, 0).a > 0.5;
                const bool resurrectionIsCloser = closer > 4.0 && onScreen && inside;
                if (resurrectionIsCloser && blendFinal < 0.5)
                    atomicOr(s_WantResurrection, 1u);
                barrier();

                // The same shared value in every lane: the branch (and the measure's barriers) is group-uniform.
                if (s_WantResurrection != 0u)
                {
                    float resurrectionBlendFinal;
                    float resurrectionClampBlend;
                    MeasureRejectionTile(tileLocal, lane, origin, true, false, resurrectionBlendFinal,
                                         resurrectionClampBlend);
                    resurrect = resurrectionIsCloser && (resurrectionBlendFinal - blendFinal) > 0.1;
                    if (resurrect)
                    {
                        blendFinal = resurrectionBlendFinal;
                        clampBlend = resurrectionClampBlend;
                    }
                }
            }

            if (!inside)
                return;

            const uint bits   = (valid ? kRejectionNotDisoccluded : 0u) | (resurrect ? kRejectionResurrected : 0u);
            const bool usable = valid || resurrect;
            imageStore(u_HistoryRejection, pixel,
                       usable ? vec4(blendFinal, clampBlend, 1.0 - blendFinal, float(bits) / 255.0)
                              : vec4(0.0, 0.0, 1.0, float(bits) / 255.0));

            const vec3 current = s_Input[(tileLocal.y + kApron) * kLoad + tileLocal.x + kApron];
            imageStore(u_GuideOut, pixel, vec4(current, 1.0));

            // The spatial anti-aliaser's input: UE ComputeSpatialAntiAliaserLumaLDR (luma of the pre-exposed colour,
            // l / (0.5 + l)), IsAliasingVisible (the 3x3 input box in the shading-measurement space) and
            // ShouldSpatialAntiAlias.
            {
                const vec3  aaLumaWeights = vec3(0.299, 0.587, 0.114);
                const float linearLuma    = dot(GCSToLinear(current), aaLumaWeights);
                const float ldrLuma       = linearLuma / (0.5 + linearLuma);
                vec3        boxMin        = GCSToSMCS(current);
                vec3        boxMax        = boxMin;
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        const vec3 c =
                             GCSToSMCS(s_Input[(tileLocal.y + kApron + y) * kLoad + tileLocal.x + kApron + x]);
                        boxMin       = min(boxMin, c);
                        boxMax       = max(boxMax, c);
                    }
                }
                const bool aliasingIsVisible = dot(boxMax - boxMin, aaLumaWeights) > 0.05;
                const bool spatialAntiAlias  = aliasingIsVisible && (blendFinal < 0.25 || (!valid && !resurrect));
                imageStore(u_AntiAliasInput, pixel, vec4(ldrLuma, spatialAntiAlias ? 1.0 : 0.0, 0.0, 0.0));
            }

            // The flickering history (UE TSRShadingAnalysis.ush :263-395): the luma's running mean over the period,
            // the last frame-to-frame gradient, and the total variation of gradients that changed sign.
            const float luma = TsrLuma(GCSToSMCS(current));
            const vec4  prev = texelFetch(u_ReprojectedMoire, pixel, 0);
            vec4        moire = vec4(luma, 0.0, 0.0, 1.0);
            if (u_EnableFlickeringHeuristic > 0.5 && valid && prev.w > 0.5)
            {
                const float blend     = 1.0 / max(u_FlickeringFramePeriod, 1.0);
                const float gradient    = luma - prev.x;
                bool        withinError = abs(gradient) <= kQuantizationError || abs(prev.y) <= kQuantizationError;
            #if TSR_THIN_GEOMETRY
                // UE ComputeMoireError :416-424: on a thin-geometry pixel a gradient above 0.00007 is not within
                // the encoding error, so the residual flicker of a wire still widens its box.
                const bool thinGeometryRegion = ThinGeometryRelaxationAt(pixel) > 0.0 &&
                                                (abs(gradient) > 0.00007 || abs(prev.y) > 0.00007) && withinError;
                withinError = withinError && !thinGeometryRegion;
            #endif
                const bool flickers = gradient * prev.y < 0.0 && !withinError;
                const float variation = flickers ? min(abs(gradient), abs(prev.y)) : 0.0;
                moire = vec4(mix(prev.x, luma, blend), gradient, max(variation, prev.z * (1.0 - blend * 0.25)), 1.0);
            }
            imageStore(u_MoireOut, pixel, moire);
        }
    }
}
