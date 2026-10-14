// DesertAsset {"Kind":"Shader","Guid":"b33d429efe8d4b8da540c2b3f32826f4","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TemporalAA"
{
    Compute
    {
        // TAA1-B / TAA-FINISH. The temporal resolve of TAA (same extent) and TAAU (OutputExtent > RenderExtent),
        // ported from UE 5.8 Engine/Shaders/Private/TemporalAA.usf (FilterCurrentFrameInputSamples :1520-1693,
        // ComputeSampleWeigth / ComputePixelWeigth :1463-1512, the blend amount :2196-2320), adapted: one compute
        // pass, YCoCg variance clip instead of UE's min/max, the responsive mask a colour target instead of a
        // stencil bit (our depth has none). One invocation per OUTPUT pixel:
        //   1. the output pixel's centre in the JITTERED render image (PPCo) and the render pixel K holding it;
        //   2. the closest depth of K's 3x3 picks the velocity texel (UE's velocity dilation); a pixel nothing
        //      wrote velocity for (velocity 0 and depth 0: reversed-Z far plane, the sky) is reprojected through
        //      the camera (unjittered inverse view-projection, previous view-projection);
        //   3. the CURRENT frame filtered from K's 3x3: each sample weighted by its distance to PPCo (UE's
        //      1 - 1.9x^2 + 0.905x^4 kernel, one output pixel wide, widened when the history is missing or the
        //      pixel moves, never narrower than the render scale) and by its HdrWeight. Under TAAU the output
        //      pixel's own temporal weight is the same kernel at its distance to the nearest sample: an output
        //      pixel no sample landed near this frame keeps its history (that IS the reconstruction);
        //   4. history at uv - velocity * (0.5, -0.5) (View/Velocity.hpp), rejected against K's neighbourhood
        //      (TAA_QUALITY below); off screen or no valid history: the filtered current alone;
        //   5. blend: UE r.TemporalAA.CurrentFrameWeight 4 %, raised toward 20 % with the output-pixel motion
        //      (1/40 px), at least the anti-flicker minimum, and toward UE's responsive 1/4 by the responsive-AA
        //      mask (translucency that writes no velocity of its own: glass, particles); in a tonemapped weight
        //      space so a bright sample cannot dominate the average (UE HdrWeight).
        // Writes the same value to u_HistoryOut (next frame's history) and u_Output (the post chain's input; the
        // overlays draw into that one, never into the history).
        //
        // TAA_QUALITY is a shader variant, set by the C++ for every pipeline (TemporalAA::PipelineFor):
        //   0 Low    — 5-tap cross min/max clamp, bilinear history fetch;
        //   1 Medium — 3x3 YCoCg variance clip, bilinear history fetch;
        //   2 High   — 3x3 YCoCg variance clip, 5-tap Catmull-Rom history fetch.
        // The program compiled with no define (the shader census, the default variant) is Medium.
        #ifndef TAA_QUALITY
        #define TAA_QUALITY 1
        #endif

        LocalSize(8, 8, 1);

        #include <Common/ReconstructPosition.glslh>

        Uniform(0) sampler2D u_SceneColor; // linear HDR, RenderExtent, jittered (texel fetches only)
        Uniform(1) sampler2D u_SceneDepth; // reversed-Z device depth, RenderExtent
        Uniform(2) sampler2D u_Velocity;   // NDC current - previous, RenderExtent
        Uniform(3) sampler2D u_Exposure;   // 1x1 previous adapted luminance (AutoExposure)
        Uniform(4) sampler2D u_History;    // previous resolve, this pass's extent
        layout(binding = 5, rgba16f) restrict writeonly uniform image2D u_HistoryOut;
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D u_Output;

        // The C++ side is Graphic::TemporalAAParams (View/TemporalAA.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 7) readonly buffer TemporalAABuffer
        {
            mat4  u_InvViewProjection;  // this frame, unjittered
            mat4  u_PrevViewProjection; // previous frame, unjittered
            vec2  u_RenderSize;
            vec2  u_OutputSize;
            vec2  u_JitterUv;           // this frame's jitter in uv (JitterNdc * (0.5, -0.5))
            float u_HistoryValid;
            float u_Pad;
        };
        Uniform(8) sampler2D u_ResponsiveAA; // R8 coverage of responsive translucency, RenderExtent


        const float kCurrentWeight       = 0.04;  // UE r.TemporalAA.CurrentFrameWeight
        const float kMovingCurrentWeight = 0.2;   // UE: lerp(BlendFinal, 0.2, saturate(Velocity / 40))
        const float kMovingPixels        = 40.0;
        const float kResponsiveWeight    = 0.25;  // UE: "Responsive forces 1/4 of new frame."
        const float kClipGamma           = 1.25;

        // UE ComputeSampleWeigth / ComputePixelWeigth: 1 - 1.9 x^2 + 0.905 x^4 of x^2 = saturate(u2 * d.d), d in
        // render pixels, u2 = (upscale * filter scale)^2. Never negative (0.005 at x^2 = 1).
        float KernelWeight(vec2 d, float u2)
        {
            const float x2 = clamp(u2 * dot(d, d), 0.0, 1.0);
            return (0.905 * x2 - 1.9) * x2 + 1.0;
        }

        float Luma(vec3 c)
        {
            return dot(c, vec3(0.2126, 0.7152, 0.0722));
        }

        vec3 RGBToYCoCg(vec3 c)
        {
            return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25)));
        }

        vec3 YCoCgToRGB(vec3 c)
        {
            return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
        }

        // The tonemapped weight of a sample (UE HdrWeight): exposure from the adapted luminance the history was
        // resolved under, the same luminance SceneComposite divides its exposure key by.
        float HdrWeight(vec3 c, float exposure)
        {
            return 1.0 / (1.0 + Luma(c) * exposure);
        }

        vec3 FetchRender(ivec2 p)
        {
            return texelFetch(u_SceneColor, clamp(p, ivec2(0), ivec2(u_RenderSize) - 1), 0).rgb;
        }

        #if TAA_QUALITY >= 2
        // 5-tap Catmull-Rom (the 4x4 kernel folded onto bilinear taps, corners dropped).
        vec3 SampleHistory(vec2 uv)
        {
            const vec2 size     = vec2(textureSize(u_History, 0));
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

            vec3 sum = textureLod(u_History, vec2(tex12.x, tex0.y), 0.0).rgb * (w12.x * w0.y);
            sum += textureLod(u_History, vec2(tex0.x, tex12.y), 0.0).rgb * (w0.x * w12.y);
            sum += textureLod(u_History, tex12, 0.0).rgb * (w12.x * w12.y);
            sum += textureLod(u_History, vec2(tex3.x, tex12.y), 0.0).rgb * (w3.x * w12.y);
            sum += textureLod(u_History, vec2(tex12.x, tex3.y), 0.0).rgb * (w12.x * w3.y);
            const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
            return max(sum / weight, vec3(0.0));
        }
        #else
        vec3 SampleHistory(vec2 uv)
        {
            return textureLod(u_History, uv, 0.0).rgb;
        }
        #endif

        // Pulls @p history toward @p center until it lies inside the box of half-size @p extents (clip, not clamp:
        // the hue of the history survives).
        vec3 ClipToBox(vec3 history, vec3 center, vec3 extents)
        {
            const vec3  d    = history - center;
            const vec3  unit = abs(d / max(extents, vec3(1e-5)));
            const float m    = max(unit.x, max(unit.y, unit.z));
            return m > 1.0 ? center + d / m : history;
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_OutputSize))))
                return;

            const vec2  uv          = (vec2(pixel) + 0.5) / u_OutputSize;
            const ivec2 renderLimit = ivec2(u_RenderSize) - 1;

            // 1. The output pixel's centre in the jittered render image: the scene at output uv sits at
            // uv + jitter there. K is the render pixel holding it, dKO the offset from K's centre (UE PPCo / PPCk).
            const vec2  ppco        = uv * u_RenderSize + u_JitterUv * u_RenderSize;
            const ivec2 renderPixel = clamp(ivec2(floor(ppco)), ivec2(0), renderLimit);
            const vec2  dKO         = ppco - (vec2(renderPixel) + 0.5);
            const float upscale     = u_OutputSize.x / u_RenderSize.x; // 1 for TAA, 2 for TAAU at 50 %
            const bool  upsample    = upscale > 1.001;

            // 2. Velocity of the closest surface of the 3x3 (reversed-Z: the largest depth is the closest).
            float closestDepth = -1.0;
            ivec2 closestPixel = renderPixel;
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const ivec2 p = clamp(renderPixel + ivec2(x, y), ivec2(0), renderLimit);
                    const float d = texelFetch(u_SceneDepth, p, 0).r;
                    if (d > closestDepth)
                    {
                        closestDepth = d;
                        closestPixel = p;
                    }
                }
            }
            vec2 velocity = texelFetch(u_Velocity, closestPixel, 0).xy;

            // The sky: nothing wrote velocity; reproject the view direction (a point at infinity, w = 0 kept
            // homogeneous, so the far plane needs no finite distance).
            if (velocity == vec2(0.0) && closestDepth <= 0.0)
            {
                const vec2 ndc      = ScreenUVToNdc(uv);
                const vec4 world    = u_InvViewProjection * vec4(ndc, 0.0, 1.0);
                const vec4 prevClip = u_PrevViewProjection * world;
                if (abs(prevClip.w) > 1e-6)
                    velocity = ndc - prevClip.xy / prevClip.w;
            }
            // The motion in OUTPUT pixels (NDC spans two uv units).
            const float velocityPixels = length(velocity * 0.5 * u_OutputSize);

            const vec2 historyUv     = uv - velocity * vec2(0.5, -0.5);
            const bool onScreen      = all(greaterThanEqual(historyUv, vec2(0.0))) && all(lessThanEqual(historyUv, vec2(1.0)));
            const bool ignoreHistory = u_HistoryValid < 0.5 || !onScreen;

            const float adaptedLum = texelFetch(u_Exposure, ivec2(0), 0).r;
            const float exposure   = 1.0 / max(adaptedLum, 1e-4);

            // 3. The filtered current frame and the neighbourhood statistics of K's 3x3. The filter scale (UE
            // InvFilterScaleFactor): 1, 0 without history (as wide as the kernel goes), narrowed by the motion,
            // never below the render scale (the input is not blurred more than its own resolution).
            const float screenPercentage = 1.0 / upscale;
            float       filterScale      = ignoreHistory ? 0.0 : 1.0;
            filterScale                  = max(filterScale - velocityPixels * upscale * 0.1, screenPercentage);
            const float u2               = upscale * upscale * filterScale * filterScale;

            vec3  filteredSum    = vec3(0.0);
            float filteredWeight = 0.0;
            vec3  m1             = vec3(0.0);
            vec3  m2             = vec3(0.0);
            vec3  lo             = vec3(1e30);
            vec3  hi             = vec3(-1e30);
            for (int y = -1; y <= 1; ++y)
            {
                for (int x = -1; x <= 1; ++x)
                {
                    const vec3  c = max(FetchRender(renderPixel + ivec2(x, y)), vec3(0.0));
                    const float w = KernelWeight(vec2(x, y) - dKO, u2) * HdrWeight(c, exposure);
                    filteredSum += c * w;
                    filteredWeight += w;

                    const vec3 s = RGBToYCoCg(c);
                    m1 += s;
                    m2 += s * s;
                    if (abs(x) + abs(y) <= 1) // the 5-tap cross of TAA_QUALITY 0
                    {
                        lo = min(lo, s);
                        hi = max(hi, s);
                    }
                }
            }
            const vec3 filtered = filteredWeight > 1e-6 ? filteredSum / filteredWeight
                                                        : max(FetchRender(renderPixel), vec3(0.0));
            // Under TAAU the output pixel's own weight this frame: how near the nearest sample landed.
            const float temporalWeight = upsample ? KernelWeight(dKO, u2) : 1.0;

            vec3 result = filtered;
            if (!ignoreHistory)
            {
                vec3 history = SampleHistory(historyUv);
                if (any(isnan(history)) || any(isinf(history)))
                    history = filtered;
                const float lumaHistory = Luma(history);

                // 4. Neighbourhood rejection in YCoCg.
                const vec3 historyYCoCg = RGBToYCoCg(history);
        #if TAA_QUALITY == 0
                history = YCoCgToRGB(clamp(historyYCoCg, lo, hi));
        #else
                const vec3 mean  = m1 / 9.0;
                const vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
                history = YCoCgToRGB(ClipToBox(historyYCoCg, mean, kClipGamma * sigma));
        #endif
                history = max(history, vec3(0.0));

                // 5. The current frame's share.
                float blend = temporalWeight * kCurrentWeight;
                blend       = mix(blend, kMovingCurrentWeight, clamp(velocityPixels / kMovingPixels, 0.0, 1.0));
                // UE: "Make sure to have at least some small contribution" (from the pre-clip history luma).
                blend = max(blend, clamp(0.01 * lumaHistory / max(abs(Luma(filtered) - lumaHistory), 1e-6), 0.0, 1.0));
                const float responsive = texelFetch(u_ResponsiveAA, renderPixel, 0).r;
                blend                  = mix(blend, kResponsiveWeight, clamp(responsive, 0.0, 1.0));

                // Feedback in the weighted space (UE WeightedLerpFactors).
                const float wCurrent = blend * HdrWeight(filtered, exposure);
                const float wHistory = (1.0 - blend) * HdrWeight(history, exposure);
                result = (filtered * wCurrent + history * wHistory) / max(wCurrent + wHistory, 1e-6);
            }

            imageStore(u_HistoryOut, pixel, vec4(result, 1.0));
            imageStore(u_Output, pixel, vec4(result, 1.0));
        }
    }
}
