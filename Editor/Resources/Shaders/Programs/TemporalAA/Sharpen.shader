// DesertAsset {"Kind":"Shader","Guid":"4bfcf5bb54bf4764a8c4cf186d3b4564","Versions":{"SHDR":1},"Dependencies":[]}
Shader "Sharpen"
{
    Compute
    {
        // SCAL-SPATIAL1. The post sharpen after the resolve (TAA, TAAU or the spatial upscale), reading
        // Resolution.Sharpness: a port of AMD FidelityFX FSR1 RCAS (Robust Contrast Adaptive Sharpening). Run by
        // Graphic::Sharpen (View/SpatialUpscale.hpp). A '+' of 5 taps; the negative lobe is the largest that keeps
        // the result inside the neighbourhood's min/max (no clipping, no ringing), scaled by u_Amount
        // (Sharpness / 100; 1 is RCAS at 0 stops, its maximum). Linear HDR goes through the reversible tonemap
        // c / (1 + max(c)) and back, as RCAS needs a [0, 1] input.
        //
        // Ported from ffx_fsr1.h, FidelityFX Super Resolution 1.0:
        // Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
        // Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
        // associated documentation files (the "Software"), to deal in the Software without restriction, including
        // without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
        // copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
        // following conditions: The above copyright notice and this permission notice shall be included in all
        // copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
        // ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
        // FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
        // LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
        // ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_Source;
        layout(binding = 1, rgba16f) restrict writeonly uniform image2D u_Destination;

        // The C++ side is Graphic::SharpenPush (View/SpatialUpscale.hpp).
        PushConstant PushConstants
        {
            int   u_Width;
            int   u_Height;
            float u_Amount;
            int   u_Pad0;
        };

        // FSR_RCAS_LIMIT: the largest negative lobe RCAS allows.
        const float kRcasLimit = 0.25 - (1.0 / 16.0);

        vec3 Tap(ivec2 p)
        {
            const vec3 c = max(texelFetch(u_Source, clamp(p, ivec2(0), ivec2(u_Width, u_Height) - 1), 0).rgb,
                               vec3(0.0));
            return c / (1.0 + max(c.r, max(c.g, c.b)));
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (pixel.x >= u_Width || pixel.y >= u_Height)
                return;

            //    b
            //  d e f
            //    h
            const vec3 b = Tap(pixel + ivec2(0, -1));
            const vec3 d = Tap(pixel + ivec2(-1, 0));
            const vec3 e = Tap(pixel);
            const vec3 f = Tap(pixel + ivec2(1, 0));
            const vec3 h = Tap(pixel + ivec2(0, 1));

            const vec3 mn4 = min(min(b, d), min(f, h));
            const vec3 mx4 = max(max(b, d), max(f, h));
            // The lobe that would just reach 0 (hitMin) or 1 (hitMax), per channel; the safer one wins.
            const vec3 hitMin = mn4 / max(4.0 * mx4, vec3(1.0 / 65504.0));
            const vec3 hitMax = (vec3(1.0) - mx4) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65504.0));
            const vec3 lobeRGB = max(-hitMin, hitMax);
            const float lobe =
                 max(-kRcasLimit, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * u_Amount;

            const vec3 t = clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), vec3(0.0), vec3(65503.0 / 65504.0));
            const vec3 linearColor = t / max(1.0 - max(t.r, max(t.g, t.b)), 1.0 / 65504.0);
            imageStore(u_Destination, pixel, vec4(linearColor, 1.0));
        }
    }
}
