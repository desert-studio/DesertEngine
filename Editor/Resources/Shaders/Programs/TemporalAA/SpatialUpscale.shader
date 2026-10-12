// DesertAsset {"Kind":"Shader","Guid":"ecbefae60f89405aa4386b1994678690","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SpatialUpscale"
{
    Compute
    {
        // SCAL-SPATIAL1. The spatial upscale (Upscaler::Spatial: below 100 % with no temporal method): RenderExtent
        // -> OutputExtent, a port of AMD FidelityFX FSR1 EASU (Edge Adaptive Spatial Upsampling). Run by
        // Graphic::SpatialUpscale (View/SpatialUpscale.hpp). 12 taps around the output pixel's source position
        // give a local edge direction and length (from luma); a direction-stretched, clipped Lanczos-2-like lobe
        // weighs them, and the result is clamped to the 2x2 neighbourhood's min/max (no ringing). The scene colour
        // is linear HDR: each tap goes through the reversible tonemap c / (1 + max(c)) first (FSR1 wants a
        // perceptual [0, 1] input) and the result through its inverse.
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

        // The C++ side is Graphic::SpatialUpscalePush (View/SpatialUpscale.hpp).
        PushConstant PushConstants
        {
            int u_SourceWidth;
            int u_SourceHeight;
            int u_DestinationWidth;
            int u_DestinationHeight;
        };

        vec3 Tap(ivec2 p)
        {
            const vec3 c = max(texelFetch(u_Source, clamp(p, ivec2(0), ivec2(u_SourceWidth, u_SourceHeight) - 1), 0).rgb,
                               vec3(0.0));
            return c / (1.0 + max(c.r, max(c.g, c.b)));
        }

        // FSR1's luma: B/2 + R/2 + G (only ratios matter).
        float Luma(vec3 c)
        {
            return c.b * 0.5 + (c.r * 0.5 + c.g);
        }

        // FsrEasuSetF: the direction and length one bilinear corner (weight w) contributes, from the '+' around
        // its centre c (a above, b left, d right, e below).
        void AccumulateDirection(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD,
                                 float lE)
        {
            const float dc   = lD - lC;
            const float cb   = lC - lB;
            float       lenX = max(abs(dc), abs(cb));
            lenX             = lenX > 0.0 ? 1.0 / lenX : 0.0;
            const float dirX = lD - lB;
            dir.x += dirX * w;
            lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
            lenX *= lenX;
            len += lenX * w;

            const float ec   = lE - lC;
            const float ca   = lC - lA;
            float       lenY = max(abs(ec), abs(ca));
            lenY             = lenY > 0.0 ? 1.0 / lenY : 0.0;
            const float dirY = lE - lA;
            dir.y += dirY * w;
            lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
            lenY *= lenY;
            len += lenY * w;
        }

        // FsrEasuTapF: one tap at @offset (source pixels from the sample position) under the rotated, stretched
        // lobe.
        void AccumulateTap(inout vec3 aC, inout float aW, vec2 offset, vec2 dir, vec2 len2, float lob, float clp,
                           vec3 c)
        {
            vec2 v = vec2(offset.x * dir.x + offset.y * dir.y, offset.x * (-dir.y) + offset.y * dir.x);
            v *= len2;
            float d2 = min(v.x * v.x + v.y * v.y, clp);
            float wB = (2.0 / 5.0) * d2 - 1.0;
            float wA = lob * d2 - 1.0;
            wB *= wB;
            wA *= wA;
            wB             = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
            const float w = wB * wA;
            aC += c * w;
            aW += w;
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (pixel.x >= u_DestinationWidth || pixel.y >= u_DestinationHeight)
                return;

            const vec2 scale = vec2(u_SourceWidth, u_SourceHeight) / vec2(u_DestinationWidth, u_DestinationHeight);
            vec2        pp    = (vec2(pixel) + 0.5) * scale - 0.5;
            const vec2  fp    = floor(pp);
            pp -= fp;
            const ivec2 o = ivec2(fp);

            //    b c
            //  e f g h
            //  i j k l
            //    n o
            const vec3 bC = Tap(o + ivec2(0, -1));
            const vec3 cC = Tap(o + ivec2(1, -1));
            const vec3 eC = Tap(o + ivec2(-1, 0));
            const vec3 fC = Tap(o + ivec2(0, 0));
            const vec3 gC = Tap(o + ivec2(1, 0));
            const vec3 hC = Tap(o + ivec2(2, 0));
            const vec3 iC = Tap(o + ivec2(-1, 1));
            const vec3 jC = Tap(o + ivec2(0, 1));
            const vec3 kC = Tap(o + ivec2(1, 1));
            const vec3 lC = Tap(o + ivec2(2, 1));
            const vec3 nC = Tap(o + ivec2(0, 2));
            const vec3 oC = Tap(o + ivec2(1, 2));

            const float bL = Luma(bC), cL = Luma(cC), eL = Luma(eC), fL = Luma(fC), gL = Luma(gC), hL = Luma(hC);
            const float iL = Luma(iC), jL = Luma(jC), kL = Luma(kC), lL = Luma(lC), nL = Luma(nC), oL = Luma(oC);

            vec2  dir = vec2(0.0);
            float len = 0.0;
            AccumulateDirection(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
            AccumulateDirection(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
            AccumulateDirection(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
            AccumulateDirection(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

            // Normalize the direction (a flat neighbourhood has none: the x axis).
            const vec2 dir2 = dir * dir;
            float      dirR = dir2.x + dir2.y;
            const bool zro  = dirR < 1.0 / 32768.0;
            dirR            = zro ? 1.0 : inversesqrt(dirR);
            dir.x           = zro ? 1.0 : dir.x;
            dir *= dirR;
            // Length: 0 isotropic, 1 a full edge; shaped as FSR1 does.
            len = len * 0.5;
            len *= len;
            // Stretch along the edge: 1 on an axis, sqrt(2) on a diagonal.
            const float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
            const vec2  len2    = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
            // The negative lobe shrinks with the edge length; the window is clipped where the lobe ends.
            const float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
            const float clp = 1.0 / lob;

            vec3  aC = vec3(0.0);
            float aW = 0.0;
            AccumulateTap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);
            AccumulateTap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);
            AccumulateTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);
            AccumulateTap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);
            AccumulateTap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);
            AccumulateTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);
            AccumulateTap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);
            AccumulateTap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);
            AccumulateTap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);
            AccumulateTap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);
            AccumulateTap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);
            AccumulateTap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);

            // Deringing: the 2x2 around the sample position bounds the result.
            const vec3 mn = min(min(fC, gC), min(jC, kC));
            const vec3 mx = max(max(fC, gC), max(jC, kC));
            const vec3 t  = clamp(aC / aW, mn, mx);
            // Inverse of the reversible tonemap (t's max stays below 1: every tap's does).
            const vec3 linearColor = t / max(1.0 - max(t.r, max(t.g, t.b)), 1.0 / 65504.0);
            imageStore(u_Destination, pixel, vec4(linearColor, 1.0));
        }
    }
}
