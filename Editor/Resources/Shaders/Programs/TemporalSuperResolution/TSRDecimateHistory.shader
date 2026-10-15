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
        // adapted: no hole-filling velocity, no history resurrection, one pixel per lane.
        // Writes the reprojected guide (rgb) with its validity in a (0: off screen, no history, or parallax
        // disoccluded: UE bIsOffScreen / bIsDisoccluded) and the reprojected moire history.
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

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
        }
    }
}
