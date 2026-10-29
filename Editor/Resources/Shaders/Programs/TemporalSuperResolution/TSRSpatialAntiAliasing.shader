// DesertAsset {"Kind":"Shader","Guid":"4ed7c3750cd5483fa36fdd2211f420de","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRSpatialAntiAliasing"
{
    Compute
    {
        // TSR.SpatialAntiAliasing. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/
        // TSRSpatialAntiAliasing.usf (the pass :88-235) and TSRSpatialAntiAliasing.ush (FindBrowsingDirection,
        // BrowseNeighborhoodBilinearOptimized, ComputeDistanceToEdge, EncodeSpatialAntiAliasingOffset; shared with
        // TSR.DilateVelocity through Common/TSRSpatialAntiAliasing.glslh), adapted: one pixel per lane (no DPV pairs),
        // full precision. Where TSR.RejectShading's anti-alias mask is set (UE ShouldSpatialAntiAlias: the aliasing
        // is visible and the history is rejected or disoccluded without resurrection), the edge through the pixel is
        // browsed on the input's LDR luma and the texel offset toward the edge is encoded for TSR.UpdateHistory, with
        // the noise filtering (UE NoiseFiltering) the update uses to soften its kernel.
        //
        // TSR_SPATIAL_AA_ITERATIONS (r.TSR.RejectionAntiAliasingQuality; UE DIM_QUALITY_PRESET 1 -> 3, 2 -> 8
        // CONFIG_ITERATIONS): 0 writes the encoded zero offset everywhere (quality 0: UE adds no pass and
        // UpdateHistory reads no offset; here the pass stays so UpdateHistory's bindings never change).
        // AntiAliasingTexture: bits 0..7 the encoded offset, bits 8..15 round(NoiseFiltering * 255) (UE ushort2).
        #ifndef TSR_SPATIAL_AA_ITERATIONS
        #define TSR_SPATIAL_AA_ITERATIONS 3
        #endif

        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(3) sampler LinearClampSampler;
        Uniform(1) texture2D AntiAliasInputTexture; // TSR.RejectShading: x LDR luma (UE InputSceneColorLdrLuma), y mask
        #define AntiAliasInputTexture sampler2D(AntiAliasInputTexture, LinearClampSampler)
        layout(binding = 2, r32ui) restrict writeonly uniform uimage2D AntiAliasingTexture;

        float LumaAt(ivec2 p)
        {
            return texelFetch(AntiAliasInputTexture, TsrClampRender(p), 0).x;
        }

        // UE BilinearSampleAAInput: the bilinear luma, clamped to the bilinear viewport.
        float BilinearLuma(vec2 position)
        {
            const vec2 p = clamp(position, vec2(0.5), RenderSize - 0.5);
            return textureLod(AntiAliasInputTexture, p / RenderSize, 0.0).x;
        }
        #define TSR_BROWSE_SAMPLE(position) BilinearLuma(position)
        #include <Common/TSRSpatialAntiAliasing.glslh>

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(RenderSize))))
                return;

            vec2  texelOffset    = vec2(0.0);
            float noiseFiltering = 0.0;
        #if TSR_SPATIAL_AA_ITERATIONS > 0
            const bool antiAlias = texelFetch(AntiAliasInputTexture, pixel, 0).y > 0.5;
            if (antiAlias)
            {
                const float c  = LumaAt(pixel);
                const float n  = LumaAt(pixel + ivec2(0, -1));
                const float s  = LumaAt(pixel + ivec2(0, 1));
                const float e  = LumaAt(pixel + ivec2(1, 0));
                const float w  = LumaAt(pixel + ivec2(-1, 0));
                const float ne = LumaAt(pixel + ivec2(1, -1));
                const float nw = LumaAt(pixel + ivec2(-1, -1));
                const float se = LumaAt(pixel + ivec2(1, 1));
                const float sw = LumaAt(pixel + ivec2(-1, 1));

                ivec2 browseDirection;
                ivec2 edgeSide;
                float edgeLuma;
                TsrFindBrowsingDirection(c, n, s, e, w, ne, nw, se, sw, noiseFiltering, browseDirection, edgeSide,
                                         edgeLuma);

                const float lumaDelta    = abs(edgeLuma - c) * 0.5;
                const bool  worthBrowing = lumaDelta > kSpatialAntiAliaserMinLuminance;
                const float browseDelta  = clamp(lumaDelta / kSpatialAntiAliaserMinLuminance, 0.0, 1.0);

                int  lengthP;
                int  lengthN;
                bool incrementP;
                bool incrementN;
                bool decrementP;
                bool decrementN;
                TsrBrowseNeighborhoodBilinear(TSR_SPATIAL_AA_ITERATIONS, c, edgeLuma, pixel, browseDirection, edgeSide,
                                              lengthP, lengthN, incrementP, incrementN, decrementP, decrementN);
                float offset = TsrComputeDistanceToEdge(incrementN, incrementP, decrementN, decrementP, lengthN,
                                                        lengthP);
                offset       = worthBrowing ? offset : 0.0;
                texelOffset  = vec2(edgeSide) * (browseDelta * offset);
            }
        #endif
            const uint encoded = EncodeSpatialAntiAliasingOffset(texelOffset) |
                                 (uint(round(noiseFiltering * 255.0)) << 8u);
            imageStore(AntiAliasingTexture, pixel, uvec4(encoded));
        }
    }
}
