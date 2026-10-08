// DesertAsset {"Kind":"Shader","Guid":"b33d429efe8d4b8da540c2b3f32826f4","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TemporalAA"
{
    Compute
    {
        // TAA1-B. The temporal resolve of TAA (same extent) and TAAU (OutputExtent > RenderExtent), UE's
        // TemporalAA.usf in outline. One invocation per OUTPUT pixel:
        //   1. the closest depth of the 3x3 render neighbourhood picks the velocity texel (silhouettes keep the
        //      foreground motion — UE's velocity dilation);
        //   2. a pixel nothing wrote velocity for (velocity 0 and depth 0: reversed-Z far plane, the sky) is
        //      reprojected through the camera: world direction from the unjittered inverse view-projection, previous
        //      clip from the previous view-projection;
        //   3. history at uv - velocity * (0.5, -0.5) (View/Velocity.hpp); off screen, or no valid history: the
        //      current colour alone;
        //   4. the history is rejected against the current neighbourhood (TAA_QUALITY below);
        //   5. blend 4 % current (UE r.TemporalAA.CurrentFrameWeight), in a tonemapped weight space so a bright
        //      sample cannot dominate the average (UE HdrWeight).
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

        Uniform(0) sampler2D u_SceneColor; // linear HDR, RenderExtent, jittered
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

        const float kCurrentWeight = 0.04;
        const float kClipGamma     = 1.25;

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
            const ivec2 renderPixel = clamp(ivec2(uv * u_RenderSize), ivec2(0), renderLimit);

            // 1. Velocity of the closest surface of the 3x3 (reversed-Z: the largest depth is the closest).
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

            // 2. The sky: nothing wrote velocity; reproject the view direction (a point at infinity, w = 0 kept
            // homogeneous, so the far plane needs no finite distance).
            if (velocity == vec2(0.0) && closestDepth <= 0.0)
            {
                const vec2 ndc       = ScreenUVToNdc(uv);
                const vec4 world     = u_InvViewProjection * vec4(ndc, 0.0, 1.0);
                const vec4 prevClip  = u_PrevViewProjection * world;
                if (abs(prevClip.w) > 1e-6)
                    velocity = ndc - prevClip.xy / prevClip.w;
            }

            // Current colour, unjittered: the scene at output uv sits at uv + jitter in the jittered image.
            const vec3 current = max(textureLod(u_SceneColor, uv + u_JitterUv, 0.0).rgb, vec3(0.0));

            const float adaptedLum = texelFetch(u_Exposure, ivec2(0), 0).r;
            const float exposure   = 1.0 / max(adaptedLum, 1e-4);

            const vec2 historyUv = uv - velocity * vec2(0.5, -0.5);
            const bool onScreen  = all(greaterThanEqual(historyUv, vec2(0.0))) && all(lessThanEqual(historyUv, vec2(1.0)));

            vec3 result = current;
            if (u_HistoryValid > 0.5 && onScreen)
            {
                vec3 history = SampleHistory(historyUv);
                if (any(isnan(history)) || any(isinf(history)))
                    history = current;

                // 4. Neighbourhood rejection in YCoCg.
                const vec3 historyYCoCg = RGBToYCoCg(history);
        #if TAA_QUALITY == 0
                const ivec2 cross[5] = ivec2[5](ivec2(0, 0), ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
                vec3 lo = vec3(1e30);
                vec3 hi = vec3(-1e30);
                for (int i = 0; i < 5; ++i)
                {
                    const vec3 s = RGBToYCoCg(FetchRender(renderPixel + cross[i]));
                    lo = min(lo, s);
                    hi = max(hi, s);
                }
                history = YCoCgToRGB(clamp(historyYCoCg, lo, hi));
        #else
                vec3 m1 = vec3(0.0);
                vec3 m2 = vec3(0.0);
                for (int y = -1; y <= 1; ++y)
                {
                    for (int x = -1; x <= 1; ++x)
                    {
                        const vec3 s = RGBToYCoCg(FetchRender(renderPixel + ivec2(x, y)));
                        m1 += s;
                        m2 += s * s;
                    }
                }
                const vec3 mean  = m1 / 9.0;
                const vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));
                history = YCoCgToRGB(ClipToBox(historyYCoCg, mean, kClipGamma * sigma));
        #endif
                history = max(history, vec3(0.0));

                // 5. Feedback in the weighted space.
                const float wCurrent = kCurrentWeight * HdrWeight(current, exposure);
                const float wHistory = (1.0 - kCurrentWeight) * HdrWeight(history, exposure);
                result = (current * wCurrent + history * wHistory) / max(wCurrent + wHistory, 1e-6);
            }

            imageStore(u_HistoryOut, pixel, vec4(result, 1.0));
            imageStore(u_Output, pixel, vec4(result, 1.0));
        }
    }
}
