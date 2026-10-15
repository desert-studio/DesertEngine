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
        // the persistent guide is kept LINEAR (TSR.RecordPersistentFrame divides out its frame's exposure), so the
        // resurrected guide is re-exposed here with this frame's exposure instead of UE's CPU
        // ResurrectionPreExposureCorrection ratio.
        // Writes the reprojected guide (rgb) with its validity in a (0: off screen, no history, or parallax
        // disoccluded: UE bIsOffScreen / bIsDisoccluded), the reprojected moire history, and the resurrected guide
        // (rgb, GCS) with a = 1 where the resurrection frame sees the pixel on screen (UE !bIsResurrectionOffScreen;
        // 0 everywhere while u_ResurrectionEnabled is 0).
        // TSR_THIN_GEOMETRY (the project's r.TSR.ThinGeometryDetection, UE DIM_THIN_GEOMETRY_COVERAGE_REPROJECTION,
        // ReprojectPrevHistoryCoverage :65-68): also last frame's coverage history, point-sampled where the pixel
        // was (clamped to the screen, as UE's GlobalPointClampedSampler), 0 without a history.
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        #ifndef TSR_THIN_GEOMETRY
        #define TSR_THIN_GEOMETRY 0
        #endif

        Uniform(1) sampler2D u_DilatedVelocity;
        Uniform(2) sampler2D u_ClosestDepth;
        layout(std430, binding = 3) restrict readonly buffer TSRPrevAtomicBuffer
        {
            uint u_PrevAtomic[];
        };
        Uniform(4) sampler2D u_PrevGuide; // previous TSR.History.Guide, RenderExtent (bilinear)
        Uniform(5) sampler2D u_PrevMoire; // previous TSR.History.Moire, RenderExtent (bilinear)
        layout(binding = 6, rgba16f) restrict writeonly uniform image2D u_ReprojectedGuide;
        layout(binding = 7, rgba16f) restrict writeonly uniform image2D u_ReprojectedMoire;
        Uniform(8) sampler2D u_ResurrectionGuide; // the persistent TSR.History.Guide.Persistent* frame, LINEAR
        Uniform(9) sampler2D u_Exposure;          // 1x1 previous adapted luminance
        layout(binding = 10, rgba16f) restrict writeonly uniform image2D u_ResurrectedGuide;
#if TSR_THIN_GEOMETRY
        Uniform(11) sampler2D u_PrevCoverage; // previous TSR.History.Coverage, RenderExtent
        layout(binding = 12, r32f) restrict writeonly uniform image2D u_ReprojectedCoverage;
#endif

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;

            const vec2  velocity  = texelFetch(u_DilatedVelocity, pixel, 0).xy;
            const float depth     = texelFetch(u_ClosestDepth, pixel, 0).r;
            const vec2  uv        = TsrSurfaceUv(pixel);
            const vec2  prevUv    = uv - velocity * vec2(0.5, -0.5);
            const bool  offScreen = u_HistoryValid < 0.5 || any(lessThan(prevUv, vec2(0.0))) ||
                                    any(greaterThan(prevUv, vec2(1.0)));

            float parallaxMask = 1.0;
            if (!offScreen && depth > 0.0)
            {
                const vec4  world     = u_InvViewProjection * vec4(ScreenUVToNdc(uv), depth, 1.0);
                const vec4  prevClip  = u_PrevViewProjection * world;
                const float prevZ     = clamp(prevClip.z / prevClip.w, 0.0, 1.0);
                const float ownDepth  = TsrPrevViewDepth(prevZ);
                const vec2  prevPixel = prevUv * u_RenderSize - 0.5;
                const ivec2 base      = ivec2(floor(prevPixel));
                const vec2  f         = prevPixel - vec2(base);
                float       weightSum = 0.0;
                parallaxMask          = 0.0;
                for (int i = 0; i < 4; ++i)
                {
                    const ivec2 q = base + ivec2(i & 1, i >> 1);
                    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, ivec2(u_RenderSize))))
                        continue;
                    const float w = ((i & 1) != 0 ? f.x : 1.0 - f.x) * ((i >> 1) != 0 ? f.y : 1.0 - f.y);
                    const float scattered = uintBitsToFloat(u_PrevAtomic[q.y * int(u_RenderSize.x) + q.x]);
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
            const vec4  guide       = offScreen ? vec4(0.0) : textureLod(u_PrevGuide, prevUv, 0.0);
            const vec4  moire       = offScreen ? vec4(0.0) : textureLod(u_PrevMoire, prevUv, 0.0);
            imageStore(u_ReprojectedGuide, pixel, vec4(guide.rgb, validity));
            imageStore(u_ReprojectedMoire, pixel, vec4(moire.xyz, validity));
#if TSR_THIN_GEOMETRY
            const ivec2 prevCoveragePixel = clamp(ivec2(floor(prevUv * u_RenderSize)), ivec2(0), ivec2(u_RenderSize) - 1);
            const float coverage = u_HistoryValid > 0.5 ? texelFetch(u_PrevCoverage, prevCoveragePixel, 0).r : 0.0;
            imageStore(u_ReprojectedCoverage, pixel, vec4(coverage));
#endif

            vec4 resurrected = vec4(0.0);
            if (u_ResurrectionEnabled > 0.5)
            {
                const vec4 resurrectionClip   = u_ClipToResurrectionClip * vec4(ScreenUVToNdc(uv), depth, 1.0);
                const vec2 resurrectionScreen = resurrectionClip.xy / resurrectionClip.w;
                const bool resurrectionOffScreen =
                     resurrectionClip.w <= 0.0 || any(greaterThan(abs(resurrectionScreen), vec2(1.0)));
                if (!resurrectionOffScreen)
                {
                    const vec2 resurrectionUv =
                         clamp(resurrectionScreen * vec2(0.5, -0.5) + 0.5, 0.5 / u_RenderSize, 1.0 - 0.5 / u_RenderSize);
                    const vec3 linearGuide = textureLod(u_ResurrectionGuide, resurrectionUv, 0.0).rgb;
                    resurrected = vec4(LinearToGCS(linearGuide * TsrExposure(u_Exposure)), 1.0);
                }
            }
            imageStore(u_ResurrectedGuide, pixel, resurrected);
        }
    }
}
