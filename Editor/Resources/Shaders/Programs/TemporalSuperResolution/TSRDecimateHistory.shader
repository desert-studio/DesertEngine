// DesertAsset {"Kind":"Shader","Guid":"072057f1e91046c99255db843f5ea2a0","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRDecimateHistory"
{
    Compute
    {
        // TSR.DecimateHistory. Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/
        // TSRDecimateHistory.usf (reprojection of the input-resolution guide and moire histories) and
        // TSRClosestOccluder.ush ProcessPrevAtomicTexturesSamples :190 (parallax disocclusion: the bilinear 2x2 of
        // the scattered previous closest depth against this pixel's own previous depth, DepthRejection =
        // saturate(2 - |delta| / epsilon), epsilon = pixel radius * PixelDepthError 3 * RadiusToDiameter 2),
        // and the resurrection reprojection :225-250 (r.TSR.Resurrection: this pixel's closest surface through
        // ClipToResurrectionClip into the persistent guide), adapted: no hole-filling velocity, one pixel per lane;
        // the persistent guide is kept LINEAR (TSR.RecordPersistentFrame divides out its frame's exposure, leaving the
        // scene colour pre-exposed by that frame's ViewFrame::PreExposure), so the resurrected guide is brought to
        // this frame's pre-exposure (UE ResurrectionPreExposureCorrection) and re-exposed with this frame's exposure.
        // Writes the reprojected guide (rgb) with its validity in a (0: off screen, no history, or parallax
        // disoccluded: UE bIsOffScreen / bIsDisoccluded), the reprojected moire history, and the resurrected guide
        // (rgb, GCS) with a = 1 where the resurrection frame sees the pixel on screen (UE !bIsResurrectionOffScreen;
        // 0 everywhere while ResurrectionEnabled is 0).
        // TSR_THIN_GEOMETRY (the project's r.TSR.ThinGeometryDetection, UE DIM_THIN_GEOMETRY_COVERAGE_REPROJECTION,
        // ReprojectPrevHistoryCoverage :65-68): also last frame's coverage history, point-sampled where the pixel
        // was (clamped to the screen, as UE's GlobalPointClampedSampler), 0 without a history.
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        #ifndef TSR_THIN_GEOMETRY
        #define TSR_THIN_GEOMETRY 0
        #endif

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(13) sampler LinearClampSampler;
        Uniform(14) sampler PointClampSampler;
        Uniform(1) texture2D DilatedVelocityTexture;
        #define DilatedVelocityTexture sampler2D(DilatedVelocityTexture, PointClampSampler)
        Uniform(2) texture2D ClosestDepthTexture;
        #define ClosestDepthTexture sampler2D(ClosestDepthTexture, PointClampSampler)
        layout(std430, binding = 3) restrict readonly buffer TSRPrevAtomicBuffer
        {
            uint PreviousAtomicData[];
        };
        Uniform(4) texture2D PreviousGuideTexture; // previous TSR.History.Guide, RenderExtent (bilinear)
        #define PreviousGuideTexture sampler2D(PreviousGuideTexture, LinearClampSampler)
        Uniform(5) texture2D PreviousMoireTexture; // previous TSR.History.Moire, RenderExtent (bilinear)
        #define PreviousMoireTexture sampler2D(PreviousMoireTexture, LinearClampSampler)
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D ReprojectedGuideTexture;
        layout(binding = 7, rgba16f) restrict writeonly uniform image2D ReprojectedMoireTexture;
        Uniform(8) texture2D ResurrectionGuideTexture; // the persistent TSR.History.Guide.Persistent* frame, LINEAR
        #define ResurrectionGuideTexture sampler2D(ResurrectionGuideTexture, LinearClampSampler)
        Uniform(9) texture2D ExposureTexture;          // 1x1 previous adapted luminance
        layout(binding = 10, rgba16f) restrict writeonly uniform image2D ResurrectedGuideTexture;
#if TSR_THIN_GEOMETRY
        Uniform(11) texture2D PreviousCoverageTexture; // previous TSR.History.Coverage, RenderExtent
        #define PreviousCoverageTexture sampler2D(PreviousCoverageTexture, PointClampSampler)
        layout(binding = 12, r32f) restrict writeonly uniform image2D ReprojectedCoverageTexture;
#endif

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(RenderSize))))
                return;

            const vec2  velocity  = texelFetch(DilatedVelocityTexture, pixel, 0).xy;
            const float depth     = texelFetch(ClosestDepthTexture, pixel, 0).r;
            const vec2  uv        = TsrSurfaceUv(pixel);
            const vec2  prevUv    = uv - velocity * vec2(0.5, -0.5);
            const bool  offScreen = HistoryValid < 0.5 || any(lessThan(prevUv, vec2(0.0))) ||
                                    any(greaterThan(prevUv, vec2(1.0)));

            float parallaxMask = 1.0;
            if (!offScreen && depth > 0.0)
            {
                const vec4  world     = InverseViewProjection * vec4(ScreenUVToNdc(uv), depth, 1.0);
                const vec4  prevClip  = PreviousViewProjection * world;
                const float prevZ     = clamp(prevClip.z / prevClip.w, 0.0, 1.0);
                const float ownDepth  = TsrPrevViewDepth(prevZ);
                const vec2  prevPixel = prevUv * RenderSize - 0.5;
                const ivec2 base      = ivec2(floor(prevPixel));
                const vec2  f         = prevPixel - vec2(base);
                float       weightSum = 0.0;
                parallaxMask          = 0.0;
                for (int i = 0; i < 4; ++i)
                {
                    const ivec2 q = base + ivec2(i & 1, i >> 1);
                    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(RenderSize))))
                        continue;
                    const float w = ((i & 1) != 0 ? f.x : 1.0 - f.x) * ((i >> 1) != 0 ? f.y : 1.0 - f.y);
                    const float scattered = uintBitsToFloat(PreviousAtomicData[q.y * int(RenderSize.x) + q.x]);
                    // Nothing of this frame landed there: no occluder, so the history there is this surface.
                    float rejection = 1.0;
                    if (scattered > 0.0)
                    {
                        const float closestDepth = TsrPrevViewDepth(scattered);
                        const float epsilon      = TsrDepthPixelRadius(closestDepth) * 3.0 * 2.0;
                        rejection = clamp(2.0 - abs(closestDepth - ownDepth) / max(epsilon, 1e-6), 0.0, 1.0);
                    }
                    parallaxMask += w * rejection;
                    weightSum += w;
                }
                parallaxMask = weightSum > 0.0 ? parallaxMask / weightSum : 0.0;
            }

            const bool  disoccluded = !offScreen && parallaxMask < 0.5; // UE bIsParallaxRejected
            const float validity    = (offScreen || disoccluded) ? 0.0 : 1.0;
            const vec4  guide       = offScreen ? vec4(0.0) : textureLod(PreviousGuideTexture, prevUv, 0.0);
            const vec4  moire       = offScreen ? vec4(0.0) : textureLod(PreviousMoireTexture, prevUv, 0.0);
            imageStore(ReprojectedGuideTexture, pixel, vec4(guide.rgb, validity));
            imageStore(ReprojectedMoireTexture, pixel, vec4(moire.xyz, validity));
#if TSR_THIN_GEOMETRY
            const ivec2 prevCoveragePixel = clamp(ivec2(floor(prevUv * RenderSize)), ivec2(0), ivec2(RenderSize) - 1);
            const float coverage = HistoryValid > 0.5 ? texelFetch(PreviousCoverageTexture, prevCoveragePixel, 0).r : 0.0;
            imageStore(ReprojectedCoverageTexture, pixel, vec4(coverage));
#endif

            vec4 resurrected = vec4(0.0);
            if (ResurrectionEnabled > 0.5)
            {
                const vec4 resurrectionClip   = ClipToResurrectionClip * vec4(ScreenUVToNdc(uv), depth, 1.0);
                const vec2 resurrectionScreen = resurrectionClip.xy / resurrectionClip.w;
                const bool resurrectionOffScreen =
                     resurrectionClip.w <= 0.0 || any(greaterThan(abs(resurrectionScreen), vec2(1.0)));
                if (!resurrectionOffScreen)
                {
                    const vec2 resurrectionUv =
                         clamp(resurrectionScreen * vec2(0.5, -0.5) + 0.5, 0.5 / RenderSize, 1.0 - 0.5 / RenderSize);
                    const vec3 linearGuide = textureLod(ResurrectionGuideTexture, resurrectionUv, 0.0).rgb;
                    resurrected = vec4(LinearToGCS(linearGuide * ResurrectionPreExposureCorrection * TsrExposure(ExposureTexture, PointClampSampler)), 1.0);
                }
            }
            imageStore(ResurrectedGuideTexture, pixel, resurrected);
        }
    }
}
