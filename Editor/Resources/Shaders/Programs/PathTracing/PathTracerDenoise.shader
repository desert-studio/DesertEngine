// DesertAsset {"Kind":"Shader","Guid":"39fe2325352b4bf2822ee33f7f4962b3","Versions":{"SHDR":1},"Dependencies":[]}
// THE PATH TRACER'S DENOISER (System::PathTracer::AddDenoisePasses, PathTracing.Denoiser): a ReLAX-style pipeline
// (NVIDIA NRD ReLAX, the denoiser of the real-time path tracers - Cyberpunk 2077 RT Overdrive, Alan Wake 2) over the
// view's path sums (Common/PathTracingDenoise.glslh):
//  - SIGNALS SPLIT BY LOBE: the diffuse and the specular illumination, each demodulated by its own reflectance
//    in the trace, each with its own history, moments and filter, remodulated at the end ("Remodulate");
//  - TEMPORAL ACCUMULATION ("Snapshot" + "Temporal", on the frames the camera moved): the previous frame's
//    integrated signal is reprojected by the camera's motion (the specular lobe also by its virtual image's motion
//    below roughness 0.5) with a plane-distance disocclusion test per bilinear tap (normals only within the diffuse
//    lobe: one path's shading normals on a normal-mapped surface differ by tens of degrees), a per-pixel history
//    length capped at 32 paths (diffuse) or 4..32 by roughness (specular), and the slow history clamped to the
//    neighbourhood of a fast (6-path) one so a lighting change does not ghost;
//  - VARIANCE from the integrated temporal moments (SVGF 4.2; a 7 x 7 spatial estimate while the history holds
//    fewer than four paths), then a variance-guided a-trous ("Filter", SVGF 4.3, here an energy-conserving
//    exchange) whose luminance width and normal lobe widen on young histories and whose iterations up to a
//    spacing of 8 / sqrt(L) spread by geometry alone (30 pixels at one path, ReLAX's pre-pass radius), the chroma
//    up to 16 / sqrt(L) - the disocclusion pre-blur that makes the frame after a cut usable.
// THE STILL CAMERA: the path tracer's accumulation is the temporal integration (it sums every path while nothing
// changes), so on a still frame the history is not touched: the integrated signal is the history (frozen at the
// pose the camera stopped at, weight L) plus the accumulation (weight s), which wins as s grows - the converged
// image is the tracer's own. After a cut or any other restart the history is empty and the image is the
// accumulation's alone (dn1024 unchanged).
//
// THE HISTORY SPLIT (the accumulation is overwritten by the trace of a moving frame): the history textures hold the
// BASE - what the history adds to the accumulation, at the current pose. "Snapshot" runs before the trace of a
// moving frame and folds the previous frame's accumulation into the base (the previous frame's integrated signal,
// capped and clamped, at the previous pose, with its guides); "Temporal" runs after the trace and reprojects that
// snapshot onto this frame's surfaces as the new base.
Shader "PathTracerDenoise"
{
    // MOVING FRAME, BEFORE THE TRACE: the previous frame's integrated signal, base + accumulation, at the previous
    // pose. Push x = 1 when the base holds a history (else it is ignored: the snapshot is the accumulation alone).
    Pass "Snapshot"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/PathTracingDenoise.glslh>

            layout(binding = 1, rgba32f) readonly uniform image2D GuideAlbedoTexture;
            layout(binding = 2, rgba32f) readonly uniform image2D GuideNormalTexture;
            layout(binding = 3, rgba32f) readonly uniform image2D GuideSpecularTexture;
            layout(binding = 4, rgba32f) readonly uniform image2D GuideHitTexture;
            layout(binding = 5, rgba32f) readonly uniform image2D DemodulatedDiffuseTexture;
            layout(binding = 6, rgba32f) readonly uniform image2D DemodulatedSpecularTexture;
            layout(binding = 7, rgba32f) readonly uniform image2D HistoryDiffuseTexture;
            layout(binding = 8, rgba32f) readonly uniform image2D HistorySpecularTexture;
            layout(binding = 9, rgba32f) readonly uniform image2D HistoryMomentsTexture;
            layout(binding = 10, rgba32f) writeonly uniform image2D SnapshotDiffuseTexture;
            layout(binding = 11, rgba32f) writeonly uniform image2D SnapshotSpecularTexture;
            layout(binding = 12, rgba32f) writeonly uniform image2D SnapshotMomentsTexture;
            layout(binding = 13, rgba32f) writeonly uniform image2D SnapshotGuideTexture;

            PushConstant PushConstants
            {
                uvec4 History; // x = 1: the base holds a history
            };

            // ReLAX historyClampingColorBoxSigmaScale: the slow history is kept within 2 standard deviations of
            // the fast history's 3 x 3 neighbourhood.
            const float kClampSigma = 2.0;

            // One lobe of a pixel folded: base (rgb, L) + the accumulation's s paths -> (rgb, L + s); m2 and the
            // fast luminance likewise (the fast history capped at kPathTracingDenoiseFastHistory).
            struct Folded
            {
                vec4  Signal;
                float SecondMoment;
                float Fast;
            };

            Folded Fold(vec4 base, float baseMoment, float baseFast, vec4 demodulated, float paths)
            {
                Folded f;
                const float total = base.a + paths;
                const vec3  mean  = demodulated.rgb / paths;
                f.Signal          = vec4((base.a * base.rgb + demodulated.rgb) / total, total);
                f.SecondMoment    = (base.a * baseMoment + demodulated.a) / total;
                const float fast   = min(base.a, kPathTracingDenoiseFastHistory);
                f.Fast             = (fast * baseFast + paths * PathTracingDenoiseLuminance(mean)) / (fast + paths);
                return f;
            }

            void FoldAt(ivec2 q, out Folded diffuse, out Folded specular)
            {
                const float paths   = imageLoad(GuideAlbedoTexture, q).a;
                const bool  history = History.x != 0u;
                const vec4  hd      = history ? imageLoad(HistoryDiffuseTexture, q) : vec4(0.0);
                const vec4  hs      = history ? imageLoad(HistorySpecularTexture, q) : vec4(0.0);
                const vec4  hm      = history ? imageLoad(HistoryMomentsTexture, q) : vec4(0.0);
                diffuse  = Fold(hd, hm.x, hm.z, imageLoad(DemodulatedDiffuseTexture, q), paths);
                specular = Fold(hs, hm.y, hm.w, imageLoad(DemodulatedSpecularTexture, q), paths);
            }

            // The factor that keeps @p slow within kClampSigma of the fast neighbourhood (mean, deviation), in
            // luminance, hue kept. The lobe's whole history is scaled by it - its mean by the factor, its second
            // moment by the factor squared - so the variance Combine derives from them (m2 - m1^2) is the clamped
            // history's own; scaling the mean alone made it negative (floored to 0: no filtering) where the clamp
            // raised a history and inflated it where the clamp lowered one.
            float ClampToFast(vec3 slow, vec2 fastMoments)
            {
                const float deviation = sqrt(max(fastMoments.y - fastMoments.x * fastMoments.x, 0.0));
                const float luminance = PathTracingDenoiseLuminance(slow);
                const float clamped   = clamp(luminance, fastMoments.x - kClampSigma * deviation,
                                              fastMoments.x + kClampSigma * deviation);
                return luminance > 1.0e-10 ? clamped / luminance : 1.0;
            }

            void main()
            {
                const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
                const ivec2 size  = imageSize(GuideAlbedoTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                const float paths = imageLoad(GuideAlbedoTexture, pixel).a;
                if (paths < 0.5)
                {
                    imageStore(SnapshotDiffuseTexture, pixel, vec4(0.0));
                    imageStore(SnapshotSpecularTexture, pixel, vec4(0.0));
                    imageStore(SnapshotMomentsTexture, pixel, vec4(0.0));
                    imageStore(SnapshotGuideTexture, pixel, vec4(0.0, 0.0, kPathTracingMissDepth, 0.0));
                    return;
                }
                Folded diffuse;
                Folded specular;
                FoldAt(pixel, diffuse, specular);

                // The fast histories' 3 x 3 neighbourhood (surface pixels): first and second moments per lobe.
                vec4  fastMoments = vec4(0.0); // diffuse mean, mean^2, specular mean, mean^2
                float count       = 0.0;
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x)
                    {
                        const ivec2 q = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
                        if (imageLoad(GuideAlbedoTexture, q).a < 0.5)
                            continue;
                        Folded qd;
                        Folded qs;
                        FoldAt(q, qd, qs);
                        fastMoments += vec4(qd.Fast, qd.Fast * qd.Fast, qs.Fast, qs.Fast * qs.Fast);
                        count += 1.0;
                    }
                fastMoments /= max(count, 1.0);

                const float roughness = imageLoad(GuideSpecularTexture, pixel).a / paths;
                const vec4  hit       = imageLoad(GuideHitTexture, pixel);
                const vec4  normal    = imageLoad(GuideNormalTexture, pixel);
                const float diffuseScale  = ClampToFast(diffuse.Signal.rgb, fastMoments.xy);
                const float specularScale = ClampToFast(specular.Signal.rgb, fastMoments.zw);
                imageStore(SnapshotDiffuseTexture, pixel,
                           vec4(diffuse.Signal.rgb * diffuseScale, min(diffuse.Signal.a, kPathTracingDenoiseDiffuseHistory)));
                imageStore(SnapshotSpecularTexture, pixel,
                           vec4(specular.Signal.rgb * specularScale,
                                min(specular.Signal.a, PathTracingDenoiseSpecularHistory(roughness))));
                imageStore(SnapshotMomentsTexture, pixel,
                           vec4(diffuse.SecondMoment * diffuseScale * diffuseScale,
                                specular.SecondMoment * specularScale * specularScale, diffuse.Fast, specular.Fast));
                imageStore(SnapshotGuideTexture, pixel,
                           vec4(PathTracingDenoiseOctEncode(normal.xyz), normal.w / paths,
                                hit.y > 1.0e-4 ? hit.x / hit.y : 0.0));
            }
        }
    }

    // MOVING FRAME, AFTER THE TRACE: the snapshot reprojected onto this frame's surfaces - the new base. Each pixel's
    // surface point (its camera ray at its mean camera distance) is projected by the previous view-projection; the
    // four bilinear taps around it count where they are the same surface (plane distance within kPlaneShare of the
    // camera distance, normals within the diffuse lobe) and the history length is scaled by the share of the footprint
    // that survived. Below roughness 0.5 the specular lobe is reprojected by its virtual image (the reflected point
    // seen behind the surface, at the camera distance + the first bounce's length), which is where a reflection
    // moves to, blended by roughness with the surface reprojection.
    Pass "Temporal"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/CameraUB.glslh>
            #include <Common/ReconstructPosition.glslh>
            #include <Common/PathTracingDenoise.glslh>

            layout(binding = 1, rgba32f) readonly uniform image2D GuideAlbedoTexture;
            layout(binding = 2, rgba32f) readonly uniform image2D GuideNormalTexture;
            layout(binding = 3, rgba32f) readonly uniform image2D GuideSpecularTexture;
            layout(binding = 4, rgba32f) readonly uniform image2D GuideHitTexture;
            layout(binding = 5, rgba32f) readonly uniform image2D SnapshotDiffuseTexture;
            layout(binding = 6, rgba32f) readonly uniform image2D SnapshotSpecularTexture;
            layout(binding = 7, rgba32f) readonly uniform image2D SnapshotMomentsTexture;
            layout(binding = 8, rgba32f) readonly uniform image2D SnapshotGuideTexture;
            layout(binding = 9, rgba32f) writeonly uniform image2D HistoryDiffuseTexture;
            layout(binding = 10, rgba32f) writeonly uniform image2D HistorySpecularTexture;
            layout(binding = 11, rgba32f) writeonly uniform image2D HistoryMomentsTexture;

            // ReLAX disocclusionThreshold: a tap is another surface when its plane lies farther than 1 % of the
            // camera distance from the reprojected point.
            const float kPlaneShare = 0.01;
            // A tap whose normal turned past the diffuse lobe's half-angle (PathTracingDenoiseLobeHalfAngle(1),
            // ~72 degrees) is another surface. NOT a tight cone: on a moving frame both normals are ONE path's shading
            // normal at a random sub-pixel point, and a normal-mapped surface (cobbles, bark, foliage) turns its
            // shading normal by tens of degrees inside a pixel - the 25-degree test this replaces rejected a large
            // share of every footprint there each frame, so the history length settled at ~2 paths and pan8 was
            // noisier than one still path. The plane distance is the disocclusion test (ReLAX: plane distance only;
            // the normal enters its spatial weights, as here in "Filter").
            float DisocclusionNormalCos()
            {
                return cos(PathTracingDenoiseLobeHalfAngle(1.0));
            }
            // The virtual image's taps also need the reflected path length within this share.
            const float kVirtualShare = 0.05;
            // The virtual-image reprojection fades out between these roughnesses (a rough lobe moves with the
            // surface).
            const vec2 kVirtualRoughness = vec2(0.15, 0.5);

            struct Gathered
            {
                vec4  Diffuse;   // rgb, L
                vec4  Specular;  // rgb, L
                vec4  Moments;   // m2 diffuse, m2 specular, fast diffuse, fast specular
                float Footprint; // the sum of the surviving bilinear weights
            };

            // The pixel position @p world lands on in the previous frame (pixel units, centres at + 0.5); w < 0
            // when it was behind the previous camera.
            vec3 PreviousPixel(vec3 world, vec2 size)
            {
                const vec4 clip = cameraUB.PrevViewProjection * vec4(world, 1.0);
                if (clip.w <= 1.0e-6)
                    return vec3(0.0, 0.0, -1.0);
                const vec2 ndc = clip.xy / clip.w;
                const vec2 uv  = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                return vec3(uv * size, 1.0);
            }

            // The snapshot's bilinear taps around @p previous that hold the same surface as @p world / @p normal.
            // @p virtualDistance > 0: the virtual image's taps, which must also hold that reflected path length.
            Gathered Gather(vec3 previous, vec3 world, vec3 normal, float virtualDistance, ivec2 size)
            {
                Gathered g;
                g.Diffuse   = vec4(0.0);
                g.Specular  = vec4(0.0);
                g.Moments   = vec4(0.0);
                g.Footprint = 0.0;
                if (previous.z < 0.0)
                    return g;
                const vec2  position  = previous.xy - 0.5;
                const ivec2 origin    = ivec2(floor(position));
                const vec2  f         = position - vec2(origin);
                const vec3  toPoint   = world - cameraUB.PrevCameraPos;
                const float pointDistance = length(toPoint);
                const float facing        = abs(dot(normal, toPoint / max(pointDistance, 1.0e-6)));
                const float tolerance     = kPlaneShare * pointDistance;
                const float normalCos     = DisocclusionNormalCos();
                for (int i = 0; i < 4; ++i)
                {
                    const ivec2 offset = ivec2(i & 1, i >> 1);
                    const ivec2 q      = origin + offset;
                    if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y)
                        continue;
                    const vec4 guide = imageLoad(SnapshotGuideTexture, q);
                    if (guide.z >= 0.5 * kPathTracingMissDepth)
                        continue;
                    // The tap's point along the same previous camera ray, its distance off our point's plane.
                    if (abs(guide.z - pointDistance) * facing > tolerance)
                        continue;
                    if (dot(PathTracingDenoiseOctDecode(guide.xy), normal) < normalCos)
                        continue;
                    if (virtualDistance > 0.0 &&
                        abs(guide.z + guide.w - virtualDistance) > kVirtualShare * virtualDistance)
                        continue;
                    const float w = (offset.x == 1 ? f.x : 1.0 - f.x) * (offset.y == 1 ? f.y : 1.0 - f.y);
                    g.Diffuse += w * imageLoad(SnapshotDiffuseTexture, q);
                    g.Specular += w * imageLoad(SnapshotSpecularTexture, q);
                    g.Moments += w * imageLoad(SnapshotMomentsTexture, q);
                    g.Footprint += w;
                }
                if (g.Footprint > 1.0e-3)
                {
                    // The mean of the surviving taps; the history length counts only the footprint that survived.
                    g.Diffuse /= g.Footprint;
                    g.Specular /= g.Footprint;
                    g.Moments /= g.Footprint;
                    g.Diffuse.a *= g.Footprint;
                    g.Specular.a *= g.Footprint;
                }
                else
                {
                    g.Diffuse  = vec4(0.0);
                    g.Specular = vec4(0.0);
                    g.Moments  = vec4(0.0);
                }
                return g;
            }

            void main()
            {
                const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
                const ivec2 size  = imageSize(GuideAlbedoTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                const float paths = imageLoad(GuideAlbedoTexture, pixel).a;
                if (paths < 0.5)
                {
                    imageStore(HistoryDiffuseTexture, pixel, vec4(0.0));
                    imageStore(HistorySpecularTexture, pixel, vec4(0.0));
                    imageStore(HistoryMomentsTexture, pixel, vec4(0.0));
                    return;
                }
                const vec4  guide     = imageLoad(GuideNormalTexture, pixel);
                const vec3  normal    = guide.xyz / max(length(guide.xyz), 1.0e-6);
                const float depth     = guide.w / paths;
                const float roughness = imageLoad(GuideSpecularTexture, pixel).a / paths;
                const vec4  hit       = imageLoad(GuideHitTexture, pixel);
                const vec2  uv        = (vec2(pixel) + 0.5) / vec2(size);
                const vec3  dir       = normalize(
                     ReconstructWorldPosition(uv, 0.5, cameraUB.InvJitteredViewProjection) - cameraUB.CameraPos);
                const vec3 world = cameraUB.CameraPos + dir * depth;

                const Gathered surface = Gather(PreviousPixel(world, vec2(size)), world, normal, 0.0, size);
                vec4  specular        = surface.Specular;
                float specularMoment  = surface.Moments.y;
                float specularFast    = surface.Moments.w;
                const float virtualAmount =
                     1.0 - smoothstep(kVirtualRoughness.x, kVirtualRoughness.y, roughness);
                if (virtualAmount > 0.0 && hit.y > 1.0e-4)
                {
                    const float     reflected = hit.x / hit.y;
                    const vec3      image     = cameraUB.CameraPos + dir * (depth + reflected);
                    const float     unfolded  = length(image - cameraUB.PrevCameraPos);
                    const Gathered  mirrored  = Gather(PreviousPixel(image, vec2(size)), world, normal, unfolded, size);
                    if (mirrored.Footprint > 1.0e-3)
                    {
                        // Where the surface reprojection lost the tap, the virtual one stands alone.
                        const float amount = surface.Footprint > 1.0e-3 ? virtualAmount : 1.0;
                        specular           = mix(specular, mirrored.Specular, amount);
                        specularMoment     = mix(specularMoment, mirrored.Moments.y, amount);
                        specularFast       = mix(specularFast, mirrored.Moments.w, amount);
                    }
                }
                imageStore(HistoryDiffuseTexture, pixel, surface.Diffuse);
                imageStore(HistorySpecularTexture, pixel, specular);
                imageStore(HistoryMomentsTexture, pixel,
                           vec4(surface.Moments.x, specularMoment, surface.Moments.z, specularFast));
            }
        }
    }

    // EVERY FRAME, AFTER THE TRACE: each lobe's integrated illumination, base + accumulation (THE STILL CAMERA
    // above), and the variance of its mean (SVGF 4.2): from the integrated second moment once the history holds
    // kPathTracingDenoiseTemporalMomentPaths paths, from the 7 x 7 same-surface neighbourhood's accumulation below.
    // Push x = 0 when the base holds no history: it is then cleared here (so it is defined for the next frame) and
    // the integrated signal is the accumulation's alone.
    Pass "Combine"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/PathTracingDenoise.glslh>

            layout(binding = 1, rgba32f) readonly uniform image2D GuideAlbedoTexture;
            layout(binding = 2, rgba32f) readonly uniform image2D GuideNormalTexture;
            layout(binding = 3, rgba32f) readonly uniform image2D DemodulatedDiffuseTexture;
            layout(binding = 4, rgba32f) readonly uniform image2D DemodulatedSpecularTexture;
            layout(binding = 5, rgba32f) uniform image2D HistoryDiffuseTexture;
            layout(binding = 6, rgba32f) uniform image2D HistorySpecularTexture;
            layout(binding = 7, rgba32f) uniform image2D HistoryMomentsTexture;
            // rgb = the lobe's integrated illumination, a = the variance of its luminance's mean.
            layout(binding = 8, rgba32f) writeonly uniform image2D IlluminationDiffuseTexture;
            layout(binding = 9, rgba32f) writeonly uniform image2D IlluminationSpecularTexture;
            // x, y = the diffuse and the specular history length (paths), z, w = 0.
            layout(binding = 10, rgba32f) writeonly uniform image2D HistoryLengthTexture;

            PushConstant PushConstants
            {
                uvec4 History; // x = 1: the base holds a history
            };

            // SVGF 4.2: the spatial moments are gathered over a 7 x 7 neighbourhood.
            const int kSpatialRadius = 3;
            // SVGF 4.3: the normal weight is max(0, n_p . n_q)^128.
            const float kNormalPower = 128.0;
            // Neighbours farther than this share of the pixel's camera distance are another surface.
            const float kSpatialDepthShare = 0.05;

            // The per-path variance of one lobe's luminance from the same-surface neighbourhood's accumulation
            // (@p specular selects the lobe): each neighbour's paths count once.
            float SpatialPathVariance(ivec2 pixel, ivec2 size, vec4 guide, float paths, bool specular)
            {
                const vec3  n      = guide.xyz / max(length(guide.xyz), 1.0e-6);
                const float depth  = guide.w / paths;
                vec2        m      = vec2(0.0);
                float       weight = 0.0;
                for (int y = -kSpatialRadius; y <= kSpatialRadius; ++y)
                    for (int x = -kSpatialRadius; x <= kSpatialRadius; ++x)
                    {
                        const ivec2 q = pixel + ivec2(x, y);
                        if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y)
                            continue;
                        const float qPaths = imageLoad(GuideAlbedoTexture, q).a;
                        if (qPaths < 0.5)
                            continue;
                        const vec4  qGuide = imageLoad(GuideNormalTexture, q);
                        const vec3  qn     = qGuide.xyz / max(length(qGuide.xyz), 1.0e-6);
                        // The young history's normal lobe as in "Filter" (at roughness 1: a neighbourhood to take
                        // statistics over, for both lobes): with SVGF's power alone a normal-mapped surface's one-path
                        // pixels each found only themselves, whose single-path variance is 0 - and a zero width
                        // stopped every colour-weighted iteration.
                        const float cosine = max(dot(n, qn), 0.0);
                        const float w      = max(pow(cosine, kNormalPower),
                                                 PathTracingDenoiseYoungNormalWeight(cosine, 1.0, paths)) *
                                        float(abs(qGuide.w / qPaths - depth) <= kSpatialDepthShare * depth);
                        const vec4 qMoments = specular ? imageLoad(DemodulatedSpecularTexture, q)
                                                       : imageLoad(DemodulatedDiffuseTexture, q);
                        m += w * vec2(PathTracingDenoiseLuminance(qMoments.rgb), qMoments.a);
                        weight += w * qPaths;
                    }
                m /= max(weight, 1.0e-6);
                return max(m.y - m.x * m.x, 0.0);
            }

            // base (rgb, L) + the accumulation's paths -> (integrated rgb, variance of its mean); @p length out.
            vec4 Integrate(vec4 base, float baseMoment, vec4 demodulated, float paths, ivec2 pixel, ivec2 size,
                           vec4 guide, bool specular, out float historyLength)
            {
                historyLength     = base.a + paths;
                const vec3  mean  = (base.a * base.rgb + demodulated.rgb) / historyLength;
                float       pathVariance;
                if (historyLength >= kPathTracingDenoiseTemporalMomentPaths)
                {
                    const float m1 = PathTracingDenoiseLuminance(mean);
                    const float m2 = (base.a * baseMoment + demodulated.a) / historyLength;
                    pathVariance   = max(m2 - m1 * m1, 0.0);
                }
                else
                    pathVariance = SpatialPathVariance(pixel, size, guide, paths, specular);
                return vec4(mean, pathVariance / historyLength);
            }

            void main()
            {
                const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
                const ivec2 size  = imageSize(GuideAlbedoTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                const bool history = History.x != 0u;
                vec4       hd      = vec4(0.0);
                vec4       hs      = vec4(0.0);
                vec4       hm      = vec4(0.0);
                if (history)
                {
                    hd = imageLoad(HistoryDiffuseTexture, pixel);
                    hs = imageLoad(HistorySpecularTexture, pixel);
                    hm = imageLoad(HistoryMomentsTexture, pixel);
                }
                else
                {
                    imageStore(HistoryDiffuseTexture, pixel, vec4(0.0));
                    imageStore(HistorySpecularTexture, pixel, vec4(0.0));
                    imageStore(HistoryMomentsTexture, pixel, vec4(0.0));
                }
                // s: the pixel's surface paths. None (a sky pixel) leaves nothing to filter.
                const float paths = imageLoad(GuideAlbedoTexture, pixel).a;
                if (paths < 0.5)
                {
                    imageStore(IlluminationDiffuseTexture, pixel, vec4(0.0));
                    imageStore(IlluminationSpecularTexture, pixel, vec4(0.0));
                    imageStore(HistoryLengthTexture, pixel, vec4(0.0));
                    return;
                }
                const vec4 guide = imageLoad(GuideNormalTexture, pixel);
                float      diffuseLength;
                float      specularLength;
                imageStore(IlluminationDiffuseTexture, pixel,
                           Integrate(hd, hm.x, imageLoad(DemodulatedDiffuseTexture, pixel), paths, pixel, size, guide,
                                     false, diffuseLength));
                imageStore(IlluminationSpecularTexture, pixel,
                           Integrate(hs, hm.y, imageLoad(DemodulatedSpecularTexture, pixel), paths, pixel, size, guide,
                                     true, specularLength));
                imageStore(HistoryLengthTexture, pixel, vec4(diffuseLength, specularLength, 0.0, 0.0));
            }
        }
    }

    // One a-trous iteration over both lobes (SVGF 4.3): a 5 x 5 B3-spline kernel with holes of Filter.x pixels,
    // each tap weighed by normal, depth and colour distance; the variance is filtered with the squared weights so
    // the next iteration's colour width shrinks with the noise removed.
    //
    // ENERGY-CONSERVING EXCHANGE (where SVGF's normalised gather is not): every weight w(p, q) is symmetric and NOT
    // normalised: the pixel becomes I_p + sum_q k_q w(p, q) (I_q - I_p), the B3 kernel's k summing to 1, so what q
    // gains from p is exactly what p loses to q and the image's illumination sum is unchanged by every iteration
    // (on heavy-tailed one-path illumination SVGF's gather lands on the dark majority: -11 % measured at one path).
    // Symmetric weights: normal; depth over both pixels' slopes and distances; colour over the pair's joint width,
    // widened on the pair's younger history (ReLAX: young histories are blurred harder); the specular lobe also by
    // its lobe - the pair's roughness narrows the normal weight and the tap radius, and a roughness change stops it.
    // The iterations whose spacing is at most kPathTracingDenoiseSpreadRadius / sqrt(L) (L the pair's shorter
    // history) drop the colour weight: the pre-blur that spreads a firefly's energy over its surface, 30 pixels at one
    // path; up to kPathTracingDenoiseChromaSpreadRadius / sqrt(L) the pair's chroma (hue at equal luminance) is
    // exchanged by geometry alone while its luminance keeps the colour weight ("Exchange": energy-conserving per
    // channel) - the young history's hue noise spread past the luminance edges' reach.
    // A young pair's normal weight is ReLAX's lobe (PathTracingDenoiseYoungNormalWeight) where it is wider than
    // SVGF's power: one path per pixel samples a normal-mapped surface's shading normal at a random sub-pixel point,
    // which max(0, n_p . n_q)^128 alone would read as a different surface at every pixel.
    Pass "Filter"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/PathTracingDenoise.glslh>

            layout(binding = 1, rgba32f) readonly uniform image2D GuideAlbedoTexture;
            layout(binding = 2, rgba32f) readonly uniform image2D GuideNormalTexture;
            layout(binding = 3, rgba32f) readonly uniform image2D GuideSpecularTexture;
            layout(binding = 4, rgba32f) readonly uniform image2D HistoryLengthTexture;
            layout(binding = 5, rgba32f) readonly uniform image2D IlluminationDiffuseTexture;
            layout(binding = 6, rgba32f) readonly uniform image2D IlluminationSpecularTexture;
            layout(binding = 7, rgba32f) writeonly uniform image2D FilteredDiffuseTexture;
            layout(binding = 8, rgba32f) writeonly uniform image2D FilteredSpecularTexture;

            PushConstant PushConstants
            {
                uvec4 Filter; // x = the tap spacing in pixels (1, 2, 4, ...)
            };

            // SVGF 4.3: sigma_n = 128, sigma_l = 4, sigma_z = 1.
            const float kNormalPower = 128.0;
            const float kLuminanceSigma = 4.0;
            // The depth tolerance that remains on a plane facing the camera (zero depth gradient), as a share of
            // the camera distance: float noise in the averaged guide, not a surface boundary.
            const float kDepthFloorShare = 1.0e-3;
            // ReLAX: the colour width of a history of no paths is (1 + kYoungWidth) x that of a full one.
            const float kYoungWidth = 2.0;
            // The specular lobe: its normal power is kNormalPower / max(roughness^2, kSpecularLobeFloor) (a
            // mirror's lobe passes only its own plane), its tap radius 1 + kSpecularRadius x roughness pixels, and
            // a roughness difference of kSpecularRoughnessWidth divides its weight by e.
            const float kSpecularLobeFloor = 0.0625;
            const float kSpecularRadius = 31.0;
            const float kSpecularRoughnessWidth = 0.1;
            const float kKernel[3] = float[3](3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);

            struct DenoiseSurface
            {
                vec3  Normal;
                float Depth;
                float Roughness;
            };

            DenoiseSurface SurfaceAt(ivec2 q)
            {
                const ivec2 size  = imageSize(GuideAlbedoTexture);
                q                 = clamp(q, ivec2(0), size - 1);
                const float paths = imageLoad(GuideAlbedoTexture, q).a;
                const vec4  guide = imageLoad(GuideNormalTexture, q);
                DenoiseSurface s;
                s.Normal    = guide.xyz / max(length(guide.xyz), 1.0e-6);
                s.Depth     = PathTracingDenoiseSurfaceDepth(guide, paths);
                s.Roughness = paths > 0.5 ? imageLoad(GuideSpecularTexture, q).a / paths : 1.0;
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

            // The colour weight of a pair (spread: none), over the pair's joint width widened on its younger
            // history.
            float ColourWeight(vec4 centre, vec4 tap, float historyLength, bool spread)
            {
                if (spread)
                    return 1.0;
                const float young = 1.0 + kYoungWidth * clamp(1.0 - historyLength / kPathTracingDenoiseDiffuseHistory, 0.0, 1.0);
                const float width = kLuminanceSigma * young * sqrt(max(0.5 * (centre.a + tap.a), 0.0)) + 1.0e-10;
                return exp(-PathTracingDenoiseChannelDistance(centre.rgb, tap.rgb, width));
            }

            // What the centre @p centre gains from the tap @p tap: its luminance exchanged with weight @p luminanceWeight
            // and its chroma (the rest of the colour difference, at equal luminance) with @p chromaWeight. The pair's
            // joint hue (their sum over their summed luminance, lum = 1) carries the luminance part, so the term is
            // antisymmetric in (centre, tap) - what one pixel gains the other loses, per channel - and its luminance is
            // luminanceWeight x the luminance difference whatever @p chromaWeight is. Equal weights: the plain exchange.
            vec3 Exchange(vec3 centre, vec3 tap, float luminanceWeight, float chromaWeight)
            {
                const float centreLuminance = PathTracingDenoiseLuminance(centre);
                const float tapLuminance    = PathTracingDenoiseLuminance(tap);
                const vec3  hue = (centre + tap) / max(centreLuminance + tapLuminance, 1.0e-10);
                return chromaWeight * (tap - centre) - (chromaWeight - luminanceWeight) * (tapLuminance - centreLuminance) * hue;
            }

            void main()
            {
                const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
                const ivec2 size  = imageSize(GuideAlbedoTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                const vec4           diffuseCentre  = imageLoad(IlluminationDiffuseTexture, pixel);
                const vec4           specularCentre = imageLoad(IlluminationSpecularTexture, pixel);
                const DenoiseSurface surface        = SurfaceAt(pixel);
                vec4                 diffuse        = diffuseCentre;
                vec4                 specular       = specularCentre;
                // A pixel no path of which met a surface has no illumination to filter (its sky is E).
                if (surface.Depth < 0.5 * kPathTracingMissDepth)
                {
                    const vec2  lengths    = imageLoad(HistoryLengthTexture, pixel).xy;
                    const float depthSlope = DepthGradient(pixel, surface.Depth);
                    const int   spacing    = int(Filter.x);
                    vec3  diffuseExchange  = vec3(0.0);
                    vec3  specularExchange = vec3(0.0);
                    float diffuseVariance  = 0.0;
                    float specularVariance = 0.0;
                    float diffuseShare     = 0.0;
                    float specularShare    = 0.0;
                    for (int y = -2; y <= 2; ++y)
                        for (int x = -2; x <= 2; ++x)
                        {
                            const ivec2 q = pixel + ivec2(x, y) * spacing;
                            if ((x == 0 && y == 0) || q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y)
                                continue;
                            const DenoiseSurface other = SurfaceAt(q);
                            if (other.Depth >= 0.5 * kPathTracingMissDepth)
                                continue;
                            const float cosine = max(dot(surface.Normal, other.Normal), 0.0);
                            // The depth tolerance over the larger of the two pixels' slopes and distances.
                            const float slope = max(depthSlope, DepthGradient(q, other.Depth));
                            const float wz    = exp(-abs(other.Depth - surface.Depth) /
                                                 (slope * length(vec2(x, y)) * float(spacing) +
                                                  kDepthFloorShare * max(surface.Depth, other.Depth)));
                            const float k        = kKernel[abs(x)] * kKernel[abs(y)];
                            const vec2  qLengths = imageLoad(HistoryLengthTexture, q).xy;
                            const vec2  pair     = min(lengths, qLengths);

                            const vec4  diffuseTap = imageLoad(IlluminationDiffuseTexture, q);
                            const float wnd        = max(pow(cosine, kNormalPower),
                                                         PathTracingDenoiseYoungNormalWeight(cosine, 1.0, pair.x));
                            const float gd         = k * wnd * wz;
                            const float wd         = gd * ColourWeight(diffuseCentre, diffuseTap, pair.x,
                                                                 PathTracingDenoiseSpreads(spacing, pair.x,
                                                                                           kPathTracingDenoiseSpreadRadius));
                            const bool  diffuseHue = PathTracingDenoiseSpreads(spacing, pair.x,
                                                                               kPathTracingDenoiseChromaSpreadRadius);
                            diffuseExchange += Exchange(diffuseCentre.rgb, diffuseTap.rgb, wd, diffuseHue ? gd : wd);
                            diffuseVariance += wd * wd * diffuseTap.a;
                            diffuseShare += wd;

                            const float roughness  = 0.5 * (surface.Roughness + other.Roughness);
                            const float radius     = 1.0 + kSpecularRadius * roughness;
                            const float reach      = length(vec2(x, y)) * float(spacing) / radius;
                            const vec4  specularTap = imageLoad(IlluminationSpecularTexture, q);
                            const float wns = max(pow(cosine, kNormalPower / max(roughness * roughness, kSpecularLobeFloor)),
                                                  PathTracingDenoiseYoungNormalWeight(cosine, roughness, pair.y));
                            const float gs = k * wns * wz * exp(-reach * reach) *
                                             exp(-abs(surface.Roughness - other.Roughness) / kSpecularRoughnessWidth);
                            const float ws = gs * ColourWeight(specularCentre, specularTap, pair.y,
                                                               PathTracingDenoiseSpreads(spacing, pair.y,
                                                                                         kPathTracingDenoiseSpreadRadius));
                            const bool  specularHue = PathTracingDenoiseSpreads(spacing, pair.y,
                                                                                kPathTracingDenoiseChromaSpreadRadius);
                            specularExchange += Exchange(specularCentre.rgb, specularTap.rgb, ws, specularHue ? gs : ws);
                            specularVariance += ws * ws * specularTap.a;
                            specularShare += ws;
                        }
                    // The centre keeps 1 - share of itself (>= kKernel[0]^2: the off-centre kernel sums to
                    // 1 - kKernel[0]^2 and every weight is <= 1).
                    const float diffuseKeep  = 1.0 - diffuseShare;
                    const float specularKeep = 1.0 - specularShare;
                    diffuse  = vec4(diffuseCentre.rgb + diffuseExchange,
                                   diffuseKeep * diffuseKeep * diffuseCentre.a + diffuseVariance);
                    specular = vec4(specularCentre.rgb + specularExchange,
                                    specularKeep * specularKeep * specularCentre.a + specularVariance);
                }
                imageStore(FilteredDiffuseTexture, pixel, diffuse);
                imageStore(FilteredSpecularTexture, pixel, specular);
            }
        }
    }

    // THE REMODULATION: raw + (1 / count) * (sum D_d * (filtered I_d - mean I_d) + sum D_s * (filtered I_s - mean
    // I_s)), the means the accumulation's own: E and the pixel's own R pass through unfiltered; only the
    // illumination the denoiser changed changes the pixel. Writes a = 1, the form the resolve divides (sum / count)
    // unchanged.
    Pass "Remodulate"
    {
        Compute
        {
            LocalSize(8, 8, 1);

            #include <Common/PathTracingDenoise.glslh>

            layout(binding = 1, rgba32f) readonly uniform image2D AccumulationTexture;
            layout(binding = 2, rgba32f) readonly uniform image2D GuideAlbedoTexture;
            layout(binding = 3, rgba32f) readonly uniform image2D GuideSpecularTexture;
            layout(binding = 4, rgba32f) readonly uniform image2D DemodulatedDiffuseTexture;
            layout(binding = 5, rgba32f) readonly uniform image2D DemodulatedSpecularTexture;
            layout(binding = 6, rgba32f) readonly uniform image2D FilteredDiffuseTexture;
            layout(binding = 7, rgba32f) readonly uniform image2D FilteredSpecularTexture;
            layout(binding = 8, rgba32f) writeonly uniform image2D DenoisedTexture;

            // SVGF 4.3: sigma_l = 4 (the convergence fade's width, the edge-stop's).
            const float kLuminanceSigma = 4.0;

            // THE FILTER FADES OUT AS THE ACCUMULATION CONVERGES: from kPathTracingDenoiseTemporalMomentPaths
            // accumulated surface paths on, a lobe's change is kept within kLuminanceSigma standard deviations of
            // the accumulation's mean - the width the edge-stop already calls noise - per channel in proportion to
            // the mean's colour. The variance of the mean is the 3 x 3 Gaussian average of the pixel's and its
            // neighbours' own (SVGF 4.3's prefilter). As the paths sum the deviation falls as 1 / sqrt(paths) and
            // the image becomes the tracer's own: the converged frame is not blurred.
            vec3 FadedChange(vec3 change, vec3 meanIllumination, float paths, ivec2 pixel, ivec2 size, bool specular)
            {
                if (paths < kPathTracingDenoiseTemporalMomentPaths)
                    return change;
                float meanVariance = 0.0;
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x)
                    {
                        const ivec2 q      = clamp(pixel + ivec2(x, y), ivec2(0), size - 1);
                        const float qPaths = max(imageLoad(GuideAlbedoTexture, q).a, 1.0);
                        const vec4  qm     = (specular ? imageLoad(DemodulatedSpecularTexture, q)
                                                       : imageLoad(DemodulatedDiffuseTexture, q)) / qPaths;
                        const float qMean  = PathTracingDenoiseLuminance(qm.rgb);
                        meanVariance += float((2 - abs(x)) * (2 - abs(y))) / 16.0 *
                                        max(qm.a - qMean * qMean, 0.0) / qPaths;
                    }
                const float meanLuminance = PathTracingDenoiseLuminance(meanIllumination);
                const vec3  limit         = kLuminanceSigma * sqrt(meanVariance) *
                                   meanIllumination / max(meanLuminance, 1.0e-10);
                return clamp(change, -limit, limit);
            }

            void main()
            {
                const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
                const ivec2 size  = imageSize(AccumulationTexture);
                if (pixel.x >= size.x || pixel.y >= size.y)
                    return;
                const vec4  sum   = imageLoad(AccumulationTexture, pixel);
                const float count = max(sum.a, 1.0);
                vec3        image = sum.rgb / count;
                const vec4  diffuseAlbedo = imageLoad(GuideAlbedoTexture, pixel);
                const float paths         = diffuseAlbedo.a;
                if (paths > 0.5)
                {
                    const vec3 diffuseMean  = imageLoad(DemodulatedDiffuseTexture, pixel).rgb / paths;
                    const vec3 specularMean = imageLoad(DemodulatedSpecularTexture, pixel).rgb / paths;
                    const vec3 diffuseChange =
                         FadedChange(imageLoad(FilteredDiffuseTexture, pixel).rgb - diffuseMean, diffuseMean, paths,
                                     pixel, size, false);
                    const vec3 specularChange =
                         FadedChange(imageLoad(FilteredSpecularTexture, pixel).rgb - specularMean, specularMean, paths,
                                     pixel, size, true);
                    image += (diffuseAlbedo.rgb * diffuseChange +
                              imageLoad(GuideSpecularTexture, pixel).rgb * specularChange) / count;
                }
                // A sub-pixel reflectance change (a decal edge inside the pixel) makes mean(D) * mean(I) differ from
                // mean(D * I): the change is then not exactly the pixel's R, and is kept from going below 0.
                imageStore(DenoisedTexture, pixel, vec4(max(image, vec3(0.0)), 1.0));
            }
        }
    }
}
