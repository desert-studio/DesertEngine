// DesertAsset {"Kind":"Shader","Guid":"1aae729ac1b34d11b79b642f6c9c695c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRUpdateHistory"
{
    Compute
    {
        // TSR.UpdateHistory. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/TSRUpdateHistory.usf
        // (the rejection unpack :850-862, the spatial anti-aliasing offset :1093-1100, the kernel / validity / weight
        // computation :1102-1160, the filtered input and its clamp box :1202-1236, the history clamp and the
        // velocity / stability validity clamp :1250-1290, the tone-weighted blend and the 8-bit validity
        // quantization :1292-1308), adapted: one pixel per lane (no DPV pairs), full precision, a 3x3 input kernel
        // (UE CONFIG_SAMPLES_COUNT 9), no lens distortion, no reprojection field / resurrection, no scene-colour
        // alpha. One invocation per HISTORY pixel; the history is linear HDR (rgb) with its validity (a: the
        // accumulated weight, 1 = HistorySampleCount samples).
        //
        // Responsive translucency (TemporalUpscalerInputs::ResponsiveAA): the previous weight is held at most 3x
        // the current one, so the current frame is at least UE's responsive 1/4.
        //
        // TSR_UPDATE_QUALITY (r.TSR.History.UpdateQuality): 0 bilinear history and a plus-shaped clamp box; 1
        // bilinear history and the 3x3 box; 2 and 3 a 5-tap Catmull-Rom history and the 3x3 box (UE: 3 adds the
        // history resurrection and the reprojection field, which this port does not have).
        // TSR_WRITE_OUTPUT: 1 when the history is the output's size (r.TSR.History.ScreenPercentage 100) — the
        // same value goes to u_Output, the post chain's input; 0 leaves the output to TSR.ResolveHistory.
        #ifndef TSR_UPDATE_QUALITY
        #define TSR_UPDATE_QUALITY 2
        #endif
        #ifndef TSR_WRITE_OUTPUT
        #define TSR_WRITE_OUTPUT 1
        #endif

        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_SceneColor;       // linear HDR, RenderExtent, jittered
        Uniform(2) sampler2D u_Exposure;         // 1x1 previous adapted luminance
        Uniform(3) sampler2D u_ResponsiveAA;     // R8 coverage of responsive translucency, RenderExtent
        Uniform(4) sampler2D u_DilatedVelocity;  // TSR.DilateVelocity
        Uniform(5) sampler2D u_HistoryRejection; // TSR.RejectShading
        Uniform(6) sampler2D u_AntiAliasing;     // TSR.SpatialAntiAliasing
        Uniform(7) sampler2D u_PrevHistory;      // previous TSR.History.Color, HistoryExtent (bilinear)
        layout(binding = 8, rgba16f) restrict writeonly uniform image2D u_HistoryOut;
        #if TSR_WRITE_OUTPUT
        layout(binding = 9, rgba16f) restrict writeonly uniform image2D u_Output;
        #endif

        // 5-tap Catmull-Rom (the 4x4 kernel folded onto bilinear taps, corners dropped), as TemporalAA.shader.
        vec3 SampleHistoryCatmullRom(vec2 uv)
        {
            const vec2 size     = u_HistorySize;
            const vec2 position = uv * size;
            const vec2 texPos1  = floor(position - 0.5) + 0.5;
            const vec2 f        = position - texPos1;
            const vec2 w0       = f * (-0.5 + f * (1.0 - 0.5 * f));
            const vec2 w1       = 1.0 + f * f * (-2.5 + 1.5 * f);
            const vec2 w2       = f * (0.5 + f * (2.0 - 1.5 * f));
            const vec2 w3       = f * f * (-0.5 + 0.5 * f);
            const vec2 w12      = w1 + w2;
            const vec2 tex0     = (texPos1 - 1.0) / size;
            const vec2 tex3     = (texPos1 + 2.0) / size;
            const vec2 tex12    = (texPos1 + w2 / w12) / size;
            vec3 result = textureLod(u_PrevHistory, vec2(tex12.x, tex0.y), 0.0).rgb * (w12.x * w0.y) +
                          textureLod(u_PrevHistory, vec2(tex0.x, tex12.y), 0.0).rgb * (w0.x * w12.y) +
                          textureLod(u_PrevHistory, vec2(tex12.x, tex12.y), 0.0).rgb * (w12.x * w12.y) +
                          textureLod(u_PrevHistory, vec2(tex3.x, tex12.y), 0.0).rgb * (w3.x * w12.y) +
                          textureLod(u_PrevHistory, vec2(tex12.x, tex3.y), 0.0).rgb * (w12.x * w3.y);
            const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
            return max(result / weight, vec3(0.0)); // the negative lobes may undershoot
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_HistorySize))))
                return;

            const float exposure = TsrExposure(u_Exposure);
            const vec2  uv       = (vec2(pixel) + 0.5) / u_HistorySize;
            // The history pixel's centre in the JITTERED input (UE ViewportUVToInputPPCo = uv * size + jitter).
            vec2  inputPPCo  = (uv + u_JitterUv) * u_RenderSize;
            ivec2 inputPixel = TsrClampRender(ivec2(floor(inputPPCo)));

            const vec2 velocity  = texelFetch(u_DilatedVelocity, inputPixel, 0).xy;
            const vec2 prevUv    = uv - velocity * vec2(0.5, -0.5);
            const bool offScreen = u_HistoryValid < 0.5 || any(lessThan(prevUv, vec2(0.0))) ||
                                   any(greaterThan(prevUv, vec2(1.0)));

            const vec4  rejection                  = texelFetch(u_HistoryRejection, inputPixel, 0);
            const float lowFrequencyRejection      = rejection.x;
            const float disableHistoryClamp        = rejection.y;
            const float decreaseValidityMultiplier = rejection.z;
            const bool  disoccluded                = rejection.w > 0.5;
            const bool  noHistory                  = offScreen || disoccluded;

            // The spatial anti-aliasing offset, the more the history is rejected (UE :1093-1100).
            const float spatialAntiAliasingLerp = noHistory ? 1.0 : clamp(1.0 - lowFrequencyRejection * 4.0, 0.0, 1.0);
            inputPPCo += texelFetch(u_AntiAliasing, inputPixel, 0).xy * spatialAntiAliasingLerp;
            inputPixel = TsrClampRender(ivec2(floor(inputPPCo)));
            const vec2 dInputKO = inputPPCo - (vec2(inputPixel) + 0.5);

            // The previous history.
            vec3  prevColor    = vec3(0.0);
            float prevValidity = 0.0;
            if (!offScreen)
            {
                const vec4 bilinear = textureLod(u_PrevHistory, prevUv, 0.0);
            #if TSR_UPDATE_QUALITY >= 2
                prevColor = SampleHistoryCatmullRom(prevUv);
            #else
                prevColor = bilinear.rgb;
            #endif
                prevValidity = bilinear.a;
            }

            // The kernel and the weights (UE :1102-1160).
            float currentWeight;
            float prevWeight;
            float kernelInputToHistoryFactor;
            {
                float minRejectionBlendFactor = RejectionFactorToBlendFactor(lowFrequencyRejection);
                minRejectionBlendFactor       = offScreen ? 1.0 : minRejectionBlendFactor;

                const float coarseAlignment  = TsrSampleWeight(1.0, dInputKO, 0.0);
                const float idealAlignment   = TsrSampleWeight(u_InputToHistoryFactor, dInputKO, 0.0);
                const float coarseCurrent    = coarseAlignment * u_HistoryHisteresis;
                const float idealCurrent     = idealAlignment * u_HistoryHisteresis;

                const float maxValidity              = 1.0 - u_WeightClampingRejection * decreaseValidityMultiplier;
                const float clampedPrevValidity      = min(prevValidity, maxValidity);
                const float refiningHisteresis       = clampedPrevValidity + idealCurrent > 0.0
                                                           ? idealCurrent / (clampedPrevValidity + idealCurrent)
                                                           : 1.0;
                const float coarseRejectedPrevWeight = min(coarseCurrent * ComputePrevWeightMultiplier(minRejectionBlendFactor), 1.0);
                const float coarseRefiningPrevWeight = min(coarseCurrent * ComputePrevWeightMultiplier(refiningHisteresis), 1.0);
                float       isRefining               = coarseRejectedPrevWeight < coarseRefiningPrevWeight ? 0.0 : 1.0;
                isRefining = min(isRefining, clamp(clampedPrevValidity * u_HistorySampleCount, 0.0, 1.0));

                const float kernelLerp = noHistory ? 0.0 : clamp(lowFrequencyRejection * 16.0 - 13.0, 0.0, 1.0) * isRefining;
                kernelInputToHistoryFactor = mix(1.0, u_InputToHistoryFactor, kernelLerp);

                const float alignment = TsrSampleWeight(kernelInputToHistoryFactor, dInputKO, 0.0);
                currentWeight         = alignment * u_HistoryHisteresis;
                prevWeight = min((currentWeight > 0.0 ? currentWeight : coarseCurrent) *
                                      ComputePrevWeightMultiplier(minRejectionBlendFactor),
                                 clampedPrevValidity);
                prevWeight = min(prevWeight, 1.0 - currentWeight);
            }

            // The filtered input and its clamp box (UE :1202-1236), pre-exposed.
            vec3  filtered       = vec3(0.0);
            float filteredWeight = 0.0;
            const vec3 center    = max(texelFetch(u_SceneColor, inputPixel, 0).rgb, vec3(0.0)) * exposure;
            vec3  boxMin         = center;
            vec3  boxMax         = center;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const ivec2 offset = ivec2(x, y);
                    const ivec2 q      = inputPixel + offset;
                    // An edge sample is clamped to the border: a duplicate of a sample already counted.
                    const bool duplicated = any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(u_RenderSize)));
                    const vec3 c          = max(texelFetch(u_SceneColor, TsrClampRender(q), 0).rgb, vec3(0.0)) * exposure;
                    const float spatial   = duplicated ? 0.0
                                                       : TsrSampleWeight(kernelInputToHistoryFactor, vec2(offset) - dInputKO, 0.005);
                    filtered += c * (spatial * TsrHdrWeight(c));
                    filteredWeight += spatial * TsrHdrWeight(c);
                #if TSR_UPDATE_QUALITY == 0
                    const bool inBox = !duplicated && (x == 0 || y == 0);
                #else
                    const bool inBox = !duplicated;
                #endif
                    if (inBox)
                    {
                        boxMin = min(boxMin, c);
                        boxMax = max(boxMax, c);
                    }
                }
            }
            filtered = filteredWeight > 0.0 ? filtered / filteredWeight : center;

            // The history clamped into the box, as much as the rejection allows (UE :1250-1256).
            const vec3 prevExposed  = prevColor * exposure;
            const vec3 clampedPrev  = clamp(prevExposed, boxMin, boxMax);
            const vec2 clampWeights = WeightedLerpFactors(TsrHdrWeight(clampedPrev), TsrHdrWeight(prevExposed), disableHistoryClamp);
            const vec3 blendedPrev  = clampedPrev * clampWeights.x + prevExposed * clampWeights.y;

            // The validity clamped by the output pixel velocity, raised back for stability on high-contrast edges
            // (UE :1262-1282).
            {
                const float outputPixelVelocity = length(velocity * 0.5 * u_OutputSize);
                float       maxValidity         = 1.0 - u_WeightClampingPixelSpeedAmplitude *
                                                            clamp(outputPixelVelocity * u_InvWeightClampingPixelSpeed, 0.0, 1.0);
                const float prevLuma            = TsrLuma(blendedPrev);
                const float filteredLuma        = TsrLuma(filtered);
                const float largest             = max(filteredLuma, prevLuma);
                maxValidity = max(maxValidity, largest > 0.0 ? abs(filteredLuma - prevLuma) / largest : 0.0);
                prevWeight  = min(prevWeight, maxValidity);
            }

            // Responsive translucency: the current frame is at least a quarter of the result.
            const float responsive = texelFetch(u_ResponsiveAA, inputPixel, 0).r;
            prevWeight = mix(prevWeight, min(prevWeight, 3.0 * max(currentWeight, u_HistoryHisteresis)), responsive);

            // The tone-weighted blend and the 8-bit validity quantization (UE :1284-1308).
            const float outputValidity = currentWeight + prevWeight;
            const float blendPrev      = prevWeight * TsrHdrWeight(blendedPrev);
            const float blendInput     = currentWeight * TsrHdrWeight(filtered);
            const float blendSum       = blendPrev + blendInput;
            const vec3  result         = blendSum > 0.0 ? (blendedPrev * blendPrev + filtered * blendInput) / blendSum : filtered;
            const float validity       = ceil(255.0 * clamp(outputValidity, 0.0, 1.0)) / 255.0;

            const vec3 linearResult = result / exposure;
            imageStore(u_HistoryOut, pixel, vec4(linearResult, validity));
        #if TSR_WRITE_OUTPUT
            imageStore(u_Output, pixel, vec4(linearResult, 1.0));
        #endif
        }
    }
}
