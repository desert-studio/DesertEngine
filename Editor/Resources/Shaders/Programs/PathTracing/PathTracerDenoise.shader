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
// DEMODULATION (SVGF 4.1): the radiance is divided by the first surface's albedo before filtering and multiplied
// back after the last iteration, so texture detail - the seams between cobblestones, a decal's edge - is never
// blurred; only the lighting, which is what the paths are noisy in, is filtered.
Shader "PathTracerDenoise"
{
    // The demodulated mean radiance of each pixel and the variance of that mean (SVGF 4.2). With fewer than
    // kTemporalMomentPaths paths summed the pixel's own moments say nothing (one path has zero variance), so the
    // moments of its neighbourhood on the same surface stand in for them.
    Pass "Variance"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/PathTracingDenoise.glslh>

            layout(binding = 0, rgba32f) readonly uniform image2D AccumulationTexture;
            layout(binding = 1, rgba32f) readonly uniform image2D GuideAlbedoTexture;
            layout(binding = 2, rgba32f) readonly uniform image2D GuideNormalTexture;
            // rgb = demodulated mean radiance, a = the variance of its luminance's mean.
            layout(binding = 3, rgba32f) writeonly uniform image2D IlluminationTexture;

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
                const vec4  sum      = imageLoad(AccumulationTexture, pixel);
                const float count    = max(sum.a, 1.0);
                const vec4  albedo   = imageLoad(GuideAlbedoTexture, pixel) / count;
                const vec4  normal   = imageLoad(GuideNormalTexture, pixel) / count;
                const vec3  demod    = PathTracingDemodulationAlbedo(albedo.rgb);
                const vec3  mean     = sum.rgb / count;
                const float lumScale = max(PathTracingDenoiseLuminance(demod), kPathTracingAlbedoFloor);

                float pathVariance;
                if (sum.a >= kTemporalMomentPaths)
                {
                    const float m1 = PathTracingDenoiseLuminance(mean);
                    pathVariance   = max(albedo.a - m1 * m1, 0.0);
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
                            const vec4  qSum    = imageLoad(AccumulationTexture, q);
                            const float qCount  = max(qSum.a, 1.0);
                            const vec4  qNormal = imageLoad(GuideNormalTexture, q) / qCount;
                            const vec3  qn      = qNormal.xyz / max(length(qNormal.xyz), 1.0e-6);
                            const float w = pow(max(dot(n, qn), 0.0), kNormalPower) *
                                            float(abs(qNormal.w - normal.w) <= kSpatialDepthShare * normal.w);
                            const float l = PathTracingDenoiseLuminance(qSum.rgb / qCount);
                            m += w * vec2(l, imageLoad(GuideAlbedoTexture, q).a / qCount);
                            weight += w;
                        }
                    m /= max(weight, 1.0e-6);
                    pathVariance = max(m.y - m.x * m.x, 0.0);
                }
                // The variance of the pixel's MEAN, in the demodulated units the filter compares.
                const float variance = pathVariance / count / (lumScale * lumScale);
                imageStore(IlluminationTexture, pixel, vec4(mean / demod, variance));
            }
        }
    }

    // One a-trous iteration (SVGF 4.3): a 5 x 5 B3-spline kernel with holes of Filter.x pixels, each tap weighed by
    // the normal, depth and luminance distance to the centre; the variance is filtered with the squared weights so
    // the next iteration's luminance width shrinks with the noise removed. The last iteration (Filter.y = 1)
    // multiplies the albedo back and writes a = 1, the form the resolve divides (sum / count) unchanged.
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
                const float count = max(imageLoad(AccumulationTexture, q).a, 1.0);
                const vec4  guide = imageLoad(GuideNormalTexture, q) / count;
                DenoiseSurface s;
                s.Normal = guide.xyz / max(length(guide.xyz), 1.0e-6);
                s.Depth  = guide.w;
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
                // The sky has no lighting noise to filter (it is the environment seen directly); half the miss depth
                // separates it from any surface whatever the rounding of its guide sum / count.
                if (surface.Depth < 0.5 * kPathTracingMissDepth)
                {
                    // SVGF 4.3: the centre's variance is prefiltered by a 3 x 3 Gaussian before it sets the width.
                    float variance = 0.0;
                    for (int y = -1; y <= 1; ++y)
                        for (int x = -1; x <= 1; ++x)
                        {
                            const ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
                            variance += float((2 - abs(x)) * (2 - abs(y))) / 16.0 * imageLoad(IlluminationTexture, q).a;
                        }
                    const float luminance  = PathTracingDenoiseLuminance(centre.rgb);
                    const float lumWidth   = kLuminanceSigma * sqrt(max(variance, 0.0)) + 1.0e-10;
                    const float depthSlope = DepthGradient(pixel, surface.Depth);
                    const int   spacing    = int(Filter.x);
                    vec3        filtered   = vec3(0.0);
                    float       filteredVariance = 0.0;
                    float       weightSum        = 0.0;
                    for (int y = -2; y <= 2; ++y)
                        for (int x = -2; x <= 2; ++x)
                        {
                            const ivec2 q = pixel + ivec2(x, y) * spacing;
                            if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y)
                                continue;
                            const vec4 tap = imageLoad(IlluminationTexture, q);
                            float      w   = kKernel[abs(x)] * kKernel[abs(y)];
                            if (x != 0 || y != 0)
                            {
                                const DenoiseSurface other = SurfaceAt(q);
                                const float wn = pow(max(dot(surface.Normal, other.Normal), 0.0), kNormalPower);
                                const float wz = exp(-abs(other.Depth - surface.Depth) /
                                                     (depthSlope * length(vec2(x, y)) * float(spacing) +
                                                      kDepthFloorShare * surface.Depth));
                                const float wl = exp(-abs(PathTracingDenoiseLuminance(tap.rgb) - luminance) / lumWidth);
                                w *= wn * wz * wl;
                            }
                            filtered += w * tap.rgb;
                            filteredVariance += w * w * tap.a;
                            weightSum += w;
                        }
                    // The centre tap always weighs kKernel[0]^2 > 0.
                    result = vec4(filtered / weightSum, filteredVariance / (weightSum * weightSum));
                }
                if (Filter.y != 0u)
                {
                    const float count  = max(imageLoad(AccumulationTexture, pixel).a, 1.0);
                    const vec3  albedo = PathTracingDemodulationAlbedo(imageLoad(GuideAlbedoTexture, pixel).rgb / count);
                    result             = vec4(result.rgb * albedo, 1.0);
                }
                imageStore(FilteredTexture, pixel, result);
            }
        }
    }
}
