// DesertAsset {"Kind":"Shader","Guid":"39fe2325352b4bf2822ee33f7f4962b3","Versions":{"SHDR":1},"Dependencies":[]}
// THE PATH TRACER'S SPATIAL DENOISER (System::PathTracer::AddDenoisePasses, PathTracing.Denoiser): every frame, over
// the view's accumulation divided by its count, so the first frame (one path per pixel) is already clean and the
// converging image stays the path tracer's own - the filter's edge-stopping width is the estimate's standard
// deviation, which falls as 1 / sqrt(paths), so at convergence the filter keeps every pixel as it is.
//
// The algorithm is SVGF's spatial half (Schied et al. 2017, "Spatiotemporal Variance-Guided Filtering", sections
// 4.2-4.3: variance estimation, then an edge-avoiding a-trous wavelet with normal, depth and luminance weights,
// the variance filtered with the squared weights). Its temporal half is not here: the path tracer's accumulation IS
// the temporal integration (a still camera sums every path; a moving one restarts). The inputs are the guides UE 5.8
// hands its path-tracing denoiser (PathTracing.cpp: radiance, albedo, normal per pixel, averaged over the summed
// paths; Common/PathTracingDenoise.glslh), so the denoiser reads exactly what UE's plugin interface reads.
//
// DEMODULATION (SVGF 4.1), per path (Common/PathTracingDenoise.glslh): each path's reflected radiance R is divided
// by its first surface's albedo D in the trace, so texture detail - the seams between cobblestones, a decal's edge -
// is never blurred; only the illumination I = R / D, which is what the paths are noisy in, is filtered. What the
// albedo does not modulate (E: the sky, the surface's own emission, emitters met through glass) is never
// divided, filtered or multiplied: the last iteration ADDS the filter's change of the pixel's illumination to its
// own raw mean, raw + (s / count) * mean(D) * (filtered I - mean I), so E and the pixel's exact R pass through and
// an unfiltered pixel (the converged sum: the luminance width -> 0) is exactly the tracer's estimate.
Shader "PathTracerDenoise"
{
    // The mean illumination of each pixel's surface paths and the variance of that mean (SVGF 4.2), both from the
    // moments of the filtered signal I itself. With fewer than kTemporalMomentPaths surface paths the pixel's own
    // moments say nothing (one path has zero variance), so the moments of its neighbourhood on the same surface
    // stand in for them.
    Pass "Variance"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/PathTracingDenoise.glslh>

            layout(binding = 0, rgba32f) readonly uniform image2D AccumulationTexture;
            layout(binding = 1, rgba32f) readonly uniform image2D GuideAlbedoTexture;
            layout(binding = 2, rgba32f) readonly uniform image2D GuideNormalTexture;
            // rgb = the mean illumination of the surface paths, a = the variance of its luminance's mean.
            layout(binding = 3, rgba32f) writeonly uniform image2D IlluminationTexture;
            layout(binding = 4, rgba32f) readonly uniform image2D DemodulatedTexture;

            // SVGF 4.2: below four samples the temporal moments are replaced by the spatial ones.
            const float kTemporalMomentPaths = 4.0;
            // SVGF 4.2: the spatial moments are gathered over a 7 x 7 neighbourhood.
            const int kSpatialRadius = 3;
            // SVGF 4.3: the normal weight is max(0, n_p . n_q)^128.
            const float kNormalPower = 128.0;
            // Neighbours farther than this share of the pixel's camera distance are another surface.
            const float kSpatialDepthShare = 0.05;

            void main()
            {
                const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
                const ivec2 size  = imageSize(AccumulationTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                // s: the pixel's surface paths. None (a sky pixel) leaves nothing to filter.
                const float paths = imageLoad(GuideAlbedoTexture, pixel).a;
                if (paths < 0.5)
                {
                    imageStore(IlluminationTexture, pixel, vec4(0.0));
                    return;
                }
                const vec4 moments = imageLoad(DemodulatedTexture, pixel) / paths;
                const vec4 normal  = imageLoad(GuideNormalTexture, pixel) / paths;

                float pathVariance;
                if (paths >= kTemporalMomentPaths)
                {
                    const float m1 = PathTracingDenoiseLuminance(moments.rgb);
                    pathVariance   = max(moments.a - m1 * m1, 0.0);
                }
                else
                {
                    const vec3 n      = normal.xyz / max(length(normal.xyz), 1.0e-6);
                    vec2       m      = vec2(0.0);
                    float      weight = 0.0;
                    for (int y = -kSpatialRadius; y <= kSpatialRadius; ++y)
                        for (int x = -kSpatialRadius; x <= kSpatialRadius; ++x)
                        {
                            const ivec2 q = pixel + ivec2(x, y);
                            if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y)
                                continue;
                            const float qPaths = imageLoad(GuideAlbedoTexture, q).a;
                            if (qPaths < 0.5)
                                continue;
                            const vec4  qNormal = imageLoad(GuideNormalTexture, q) / qPaths;
                            const vec3  qn      = qNormal.xyz / max(length(qNormal.xyz), 1.0e-6);
                            const float w = pow(max(dot(n, qn), 0.0), kNormalPower) *
                                            float(abs(qNormal.w - normal.w) <= kSpatialDepthShare * normal.w);
                            // The neighbour's path sums: each of its paths counts once.
                            const vec4 qMoments = imageLoad(DemodulatedTexture, q);
                            m += w * vec2(PathTracingDenoiseLuminance(qMoments.rgb), qMoments.a);
                            weight += w * qPaths;
                        }
                    m /= max(weight, 1.0e-6);
                    pathVariance = max(m.y - m.x * m.x, 0.0);
                }
                // No firefly clamp: a clamp below the pixel's neighbours drops the energy it cuts, and at one path
                // the illumination's energy IS its rare bright paths (round 5 measured -21 % Day / -29 % Night at
                // one path, nearly all of it this clamp). The filter's exchange spreads fireflies instead.
                const vec3 illumination = moments.rgb;
                // The variance of the pixel's MEAN illumination: the unit the filter compares in.
                imageStore(IlluminationTexture, pixel, vec4(illumination, pathVariance / paths));
            }
        }
    }

    // One a-trous iteration (SVGF 4.3): a 5 x 5 B3-spline kernel with holes of Filter.x pixels, each tap weighed by
    // the normal, depth and luminance distance to the centre; the variance is filtered with the squared weights so
    // the next iteration's luminance width shrinks with the noise removed.
    //
    // ENERGY-CONSERVING EXCHANGE (where SVGF's normalised gather is not): SVGF divides by each pixel's own weight sum,
    // so a pixel among dark neighbours gives its energy to none of them (they reject it) while its own result is
    // pulled towards them - on heavy-tailed one-path illumination, whose energy is its rare bright paths, the
    // gather lands on the dark majority (an edge-stopped gather measured -11 % at one path). Here every weight
    // w(p, q) is symmetric (normal, depth over both pixels' slopes, the colour distance over the pair's joint
    // width) and NOT normalised: the pixel becomes I_p + sum_q k_q w(p, q) (I_q - I_p), the B3 kernel's k summing
    // to 1, so what q gains from p is exactly what p loses to q and the image's illumination sum is unchanged by
    // every iteration; a tap the weights reject simply leaves its share of the kernel at the centre. The last iteration (Filter.y = 1)
    // adds the filter's change of the illumination, remodulated, to the pixel's raw mean (DEMODULATION above) and
    // writes a = 1, the form the resolve divides (sum / count) unchanged.
    Pass "Filter"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/PathTracingDenoise.glslh>

            layout(binding = 0, rgba32f) readonly uniform image2D AccumulationTexture;
            layout(binding = 1, rgba32f) readonly uniform image2D GuideAlbedoTexture;
            layout(binding = 2, rgba32f) readonly uniform image2D GuideNormalTexture;
            layout(binding = 3, rgba32f) readonly uniform image2D IlluminationTexture;
            layout(binding = 4, rgba32f) writeonly uniform image2D FilteredTexture;
            layout(binding = 5, rgba32f) readonly uniform image2D DemodulatedTexture;

            PushConstant PushConstants
            {
                uvec4 Filter; // x = the tap spacing in pixels (1, 2, 4, ...), y = 1 on the last iteration
            };

            // SVGF 4.3: sigma_n = 128, sigma_l = 4, sigma_z = 1.
            const float kNormalPower = 128.0;
            const float kLuminanceSigma = 4.0;
            // The depth tolerance that remains on a plane facing the camera (zero depth gradient), as a share of
            // the camera distance: float noise in the averaged guide, not a surface boundary.
            const float kDepthFloorShare = 1.0e-3;
            // Below kPathTracingDenoiseSpreadPaths surface paths the luminance weight would keep every firefly and
            // leave every dark pixel dark (a noisy image the tonemap then darkens). The first two iterations (taps
            // 1 and 2 px apart, a 13 x 13 footprint) then weigh by normal and depth only, spreading each spike's
            // energy over its own surface (the exchange conserves it); the later iterations edge-stop on the colour
            // distance over the variance those two left.
            // SVGF 4.2: below four paths the pixel's own moments say nothing (the variance pass's spatial case).
            const float kTemporalMomentPaths = 4.0;
            const float kKernel[3] = float[3](3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);

            struct DenoiseSurface
            {
                vec3  Normal;
                float Depth;
            };

            DenoiseSurface SurfaceAt(ivec2 q)
            {
                const ivec2 size  = imageSize(AccumulationTexture);
                q                 = clamp(q, ivec2(0), size - 1);
                const float paths = imageLoad(GuideAlbedoTexture, q).a;
                const vec4  guide = imageLoad(GuideNormalTexture, q);
                DenoiseSurface s;
                s.Normal = guide.xyz / max(length(guide.xyz), 1.0e-6);
                s.Depth  = PathTracingDenoiseSurfaceDepth(guide, paths);
                return s;
            }

            // The depth change per pixel along the surface: the smaller of the two one-sided differences on each
            // axis (a silhouette on one side does not widen the tolerance), the larger axis.
            float DepthGradient(ivec2 pixel, float depth)
            {
                const float dx = min(abs(SurfaceAt(pixel + ivec2(1, 0)).Depth - depth),
                                     abs(depth - SurfaceAt(pixel - ivec2(1, 0)).Depth));
                const float dy = min(abs(SurfaceAt(pixel + ivec2(0, 1)).Depth - depth),
                                     abs(depth - SurfaceAt(pixel - ivec2(0, 1)).Depth));
                return max(dx, dy);
            }

            void main()
            {
                const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
                const ivec2 size  = imageSize(IlluminationTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                const vec4           centre  = imageLoad(IlluminationTexture, pixel);
                const DenoiseSurface surface = SurfaceAt(pixel);
                vec4                 result  = centre;
                // A pixel no path of which met a surface has no illumination to filter (its sky is E).
                if (surface.Depth < 0.5 * kPathTracingMissDepth)
                {
                    const float depthSlope = DepthGradient(pixel, surface.Depth);
                    const int   spacing    = int(Filter.x);
                    const bool  spread     = imageLoad(GuideAlbedoTexture, pixel).a < kPathTracingDenoiseSpreadPaths &&
                                        spacing <= 2;
                    vec3  exchange      = vec3(0.0);
                    float tapVariance   = 0.0;
                    float exchangeShare = 0.0;
                    for (int y = -2; y <= 2; ++y)
                        for (int x = -2; x <= 2; ++x)
                        {
                            const ivec2 q = pixel + ivec2(x, y) * spacing;
                            if ((x == 0 && y == 0) || q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y)
                                continue;
                            const DenoiseSurface other = SurfaceAt(q);
                            if (other.Depth >= 0.5 * kPathTracingMissDepth)
                                continue;
                            const vec4  tap = imageLoad(IlluminationTexture, q);
                            const float wn  = pow(max(dot(surface.Normal, other.Normal), 0.0), kNormalPower);
                            // The depth tolerance over the larger of the two pixels' slopes and distances: the same
                            // from either side.
                            const float slope = max(depthSlope, DepthGradient(q, other.Depth));
                            const float wz    = exp(-abs(other.Depth - surface.Depth) /
                                                 (slope * length(vec2(x, y)) * float(spacing) +
                                                  kDepthFloorShare * max(surface.Depth, other.Depth)));
                            // The pair's joint luminance width: kLuminanceSigma standard deviations of the
                            // difference's noise, sqrt of the mean of the two variances (SVGF uses the centre's
                            // alone, which makes the weight one-sided).
                            const float lumWidth = kLuminanceSigma * sqrt(max(0.5 * (centre.a + tap.a), 0.0)) + 1.0e-10;
                            const float wl =
                                 spread ? 1.0 : exp(-PathTracingDenoiseChannelDistance(centre.rgb, tap.rgb, lumWidth));
                            const float w = kKernel[abs(x)] * kKernel[abs(y)] * wn * wz * wl;
                            exchange += w * (tap.rgb - centre.rgb);
                            tapVariance += w * w * tap.a;
                            exchangeShare += w;
                        }
                    // The centre keeps 1 - exchangeShare of itself (>= kKernel[0]^2: the off-centre kernel sums to
                    // 1 - kKernel[0]^2 and every weight is <= 1).
                    const float keep = 1.0 - exchangeShare;
                    result           = vec4(centre.rgb + exchange, keep * keep * centre.a + tapVariance);
                }
                if (Filter.y != 0u)
                {
                    // raw + (s / count) * mean(D) * (filtered I - mean I): E and the pixel's own R pass through
                    // unfiltered; only the illumination noise the filter removed changes the pixel.
                    const vec4  sum    = imageLoad(AccumulationTexture, pixel);
                    const float count  = max(sum.a, 1.0);
                    const vec4  albedo = imageLoad(GuideAlbedoTexture, pixel);
                    vec3        image  = sum.rgb / count;
                    if (albedo.a > 0.5)
                    {
                        const vec4 moments          = imageLoad(DemodulatedTexture, pixel) / albedo.a;
                        const vec3 meanIllumination = moments.rgb;
                        vec3       change           = result.rgb - meanIllumination;
                        // THE FILTER FADES OUT AS THE ESTIMATE CONVERGES: from kTemporalMomentPaths surface paths on
                        // (where the pixel's own moments measure its noise) the change is kept within
                        // kLuminanceSigma standard deviations of the pixel's mean - the width the edge-stop already
                        // calls noise - per channel in proportion to the mean's colour. The variance of the mean
                        // is the 3 x 3 Gaussian average of the pixel's and its neighbours' own (SVGF 4.3's
                        // prefilter), so a dark pixel among fireflies is not held dark by its own few dark paths.
                        // As the paths sum the deviation falls as 1 / sqrt(paths) and the image becomes the
                        // tracer's own: the converged frame is not blurred.
                        if (albedo.a >= kTemporalMomentPaths)
                        {
                            float meanVariance = 0.0;
                            for (int y = -1; y <= 1; ++y)
                                for (int x = -1; x <= 1; ++x)
                                {
                                    const ivec2 q      = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
                                    const float qPaths = max(imageLoad(GuideAlbedoTexture, q).a, 1.0);
                                    const vec4  qm     = imageLoad(DemodulatedTexture, q) / qPaths;
                                    const float qMean  = PathTracingDenoiseLuminance(qm.rgb);
                                    meanVariance += float((2 - abs(x)) * (2 - abs(y))) / 16.0 *
                                                    max(qm.a - qMean * qMean, 0.0) / qPaths;
                                }
                            const float meanLuminance = PathTracingDenoiseLuminance(meanIllumination);
                            const vec3  limit         = kLuminanceSigma * sqrt(meanVariance) *
                                               meanIllumination / max(meanLuminance, 1.0e-10);
                            change = clamp(change, -limit, limit);
                        }
                        image += albedo.rgb / count * change;
                    }
                    // A sub-pixel albedo change (a decal edge inside the pixel) makes mean(D) * mean(I) differ from
                    // mean(D * I): the change is then not exactly the pixel's R, and is kept from going below 0.
                    result = vec4(max(image, vec3(0.0)), 1.0);
                }
                imageStore(FilteredTexture, pixel, result);
            }
        }
    }
}
