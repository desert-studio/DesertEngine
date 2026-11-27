// DesertAsset {"Kind":"Shader","Guid":"7a2d4e6f8b0c4d1e9f3a5b7c9d1e3f50","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HeterogeneousVolumeMarch"
{
    Compute
    {
        // THE VIEW MARCH OF THE HETEROGENEOUS VOLUMES (UE 5.3+ HeterogeneousVolumes RenderSingleScatteringWithLiveShading,
        // reimplemented over the lighting cache), at HALF the render resolution. One thread per half-res pixel:
        // the eye ray against every on-screen volume's box, nearest first, each segment cut at the scene's depth
        // and marched in jittered steps - extinction from the medium (Common/HeterogeneousVolume.glslh), the light
        // from ONE trilinear fetch of the lighting cache per step, emission from the authored colour and the
        // temperature's blackbody - integrated analytically per step (Frostbite 2015, energy-conserving).
        // Blended with last frame's result at the reprojected front of the medium (the jitter integrates over
        // frames). Outside the volumes' screen rectangle (HvMarch.Rect) nothing is marched.
        //
        // Writes RGBA16F: .rgb = in-scattered + emitted radiance (absolute, premultiplied), .a = transmittance; and
        // R32F guide: the scene distance the ray was cut at (the upsample's depth weight).

        #include <Common/VolumetricFog.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image2D MarchTexture;
        layout(binding = 5, r32f) restrict writeonly uniform image2D MarchGuideTexture;

        Uniform sampler LinearClampSampler;
        Uniform sampler PointClampSampler;
        // Last frame's MarchTexture (the ping-pong pair); read only when HvMarch.Counts.z says it is this view's
        // previous frame.
        Uniform(2) texture2D MarchHistoryTexture;
        #define MarchHistoryTexture sampler2D(MarchHistoryTexture, LinearClampSampler)
        // This frame's lighting cache (HeterogeneousVolumeLightingCache.shader), one slab per volume.
        Uniform(3) texture3D LightingCacheTexture;
        #define LightingCacheTexture sampler3D(LightingCacheTexture, LinearClampSampler)
        // The single-sample scene depth (reverse Z: 0 = sky), texelFetched.
        Uniform(4) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)

        // The baked sources' page tables (Graphic::kSparseVolumePagesBinding, the pipeline's own buffer) and the
        // shared brick atlas they index (HeterogeneousVolumeRenderer's residency; a 1-brick fallback while no baked
        // volume is resident). Declared before the medium text, which samples them (HETEROGENEOUS_VOLUME_MEDIUM).
        ReadBuffer(18) SparseVolumePagesBuffer { uint hvPages[]; };
        Uniform(19) texture3D SparseVolumeAtlasTexture;
        #define SparseVolumeAtlasTexture sampler3D(SparseVolumeAtlasTexture, LinearClampSampler)
        #define HETEROGENEOUS_VOLUME_MEDIUM
        #include <Common/HeterogeneousVolume.glslh>

        ReadBuffer(15) HeterogeneousVolumesBuffer { HeterogeneousVolumeGpu heterogeneousVolumes[]; };
        // Graphic::HeterogeneousVolumeMarchGpu, the pipeline's own buffer (kHeterogeneousVolumeParamsBinding).
        ReadBuffer(17) HeterogeneousVolumeMarchParams { DESERT_HeterogeneousVolumeMarchGpu_MEMBERS };

        LocalSize(8, 8, 1);

        // Interleaved gradient noise (Jimenez 2014), rotated by the golden ratio per frame.
        float StepJitter(uvec2 pixel, uint frame)
        {
            float ign = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715))));
            return fract(ign + float(frame % 64u) * 0.61803399);
        }

        void main()
        {
            uvec2 pixel = gl_GlobalInvocationID.xy;
            if (any(greaterThanEqual(vec2(pixel), HvTarget.xy)))
                return;

            vec2 uv = (vec2(pixel) + 0.5) / HvTarget.xy;
            if (any(lessThan(uv, HvRect.xy)) || any(greaterThan(uv, HvRect.zw)))
            {
                imageStore(MarchTexture, ivec2(pixel), vec4(0.0, 0.0, 0.0, 1.0));
                imageStore(MarchGuideTexture, ivec2(pixel), vec4(1e9, 0.0, 0.0, 0.0));
                return;
            }

            // The NEAREST surface of the pixel's 2x2 full-res block (reverse Z: the largest depth): the medium
            // is never drawn over a foreground edge; the upsample weighs it against each full-res pixel's own.
            ivec2 depthSize = textureSize(SceneDepthTexture, 0);
            ivec2 full      = ivec2(pixel) * 2;
            float device    = 0.0;
            for (int i = 0; i < 4; ++i)
                device = max(device, texelFetch(SceneDepthTexture,
                                                clamp(full + ivec2(i & 1, i >> 1), ivec2(0), depthSize - 1), 0).r);

            vec2 ndc   = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
            vec4 farH  = VfInvViewProjection * vec4(ndc, 0.5, 1.0);
            vec3 dir   = normalize(farH.xyz / farH.w - VfCamera.xyz);
            float sceneDistance = 1e9;
            if (device > 0.0)
            {
                vec4 h = VfInvViewProjection * vec4(ndc, device, 1.0);
                sceneDistance = length(h.xyz / h.w - VfCamera.xyz);
            }

            // The volumes this ray crosses, nearest entry first (at most HETEROGENEOUS_VOLUME_MAX: insertion sort).
            uint  order[8];
            float entry[8];
            uint  hits = 0u;
            for (uint v = 0u; v < min(HvCounts.x, 8u); ++v)
            {
                HeterogeneousVolumeGpu volume = heterogeneousVolumes[v];
                vec3 lo  = (volume.WorldToLocal * vec4(VfCamera.xyz, 1.0)).xyz;
                vec3 ld  = (volume.WorldToLocal * vec4(dir, 0.0)).xyz;
                vec2 hit = HvIntersectUnitBox(lo, ld);
                float t0 = max(hit.x, 0.0);
                if (min(hit.y, sceneDistance) <= t0)
                    continue;
                uint k = hits++;
                while (k > 0u && entry[k - 1u] > t0)
                {
                    entry[k] = entry[k - 1u];
                    order[k] = order[k - 1u];
                    --k;
                }
                entry[k] = t0;
                order[k] = v;
            }

            vec3  radiance      = vec3(0.0);
            float transmittance = 1.0;
            float front         = -1.0;
            float jitter        = StepJitter(pixel, HvCounts.y);
            for (uint h = 0u; h < hits && transmittance > 0.01; ++h)
            {
                uint  v      = order[h];
                HeterogeneousVolumeGpu volume = heterogeneousVolumes[v];
                vec3  lo     = (volume.WorldToLocal * vec4(VfCamera.xyz, 1.0)).xyz;
                vec3  ld     = (volume.WorldToLocal * vec4(dir, 0.0)).xyz;
                vec2  hit    = HvIntersectUnitBox(lo, ld);
                float t0     = max(hit.x, 0.0);
                float t1     = min(hit.y, sceneDistance);
                // Steps of a 32nd of the box's smallest half size, at most HvCounts.w per volume.
                float minHalf = min(volume.HalfSize.x, min(volume.HalfSize.y, volume.HalfSize.z));
                int   steps   = int(clamp(ceil((t1 - t0) / max(minHalf / 16.0, 1.0)), 1.0, float(HvCounts.w)));
                float dt      = (t1 - t0) / float(steps);
                for (int s = 0; s < steps && transmittance > 0.01; ++s)
                {
                    float t     = t0 + (float(s) + jitter) * dt;
                    vec3  local = lo + ld * t;
                    vec2  medium = HvMedium(volume, local);
                    float d01    = medium.x;
                    if (d01 <= 0.0)
                        continue;
                    if (front < 0.0)
                        front = t;
                    float ext       = d01 * volume.Medium.x;
                    vec3  light     = texture(LightingCacheTexture, HvCacheUVW(local, float(v))).rgb;
                    vec3  source    = volume.Albedo.rgb * ext * light + HvEmission(volume, medium.y) * ext;
                    float stepT     = exp(-ext * dt);
                    // Integrated over the step (Hillaire 2015): S (1 - e^{-sigma dt}) / sigma.
                    radiance      += transmittance * source * (1.0 - stepT) / max(ext, 1e-7);
                    transmittance *= stepT;
                }
            }

            vec4 current = vec4(radiance, transmittance);

            // Reprojection at the medium's front (or the cut distance when the ray met none) into last frame.
            if (HvCounts.z != 0u)
            {
                float d        = front >= 0.0 ? front : min(sceneDistance, 1e6);
                vec3  p        = VfCamera.xyz + dir * d;
                vec4  prevClip = VfPrevViewProjection * vec4(p, 1.0);
                if (prevClip.w > 0.0)
                {
                    vec2 prevNdc = prevClip.xy / prevClip.w;
                    vec2 prevUV  = vec2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
                    if (all(greaterThanEqual(prevUV, vec2(0.0))) && all(lessThanEqual(prevUV, vec2(1.0))))
                    {
                        vec4  history = textureLod(MarchHistoryTexture, prevUV, 0.0);
                        // A history whose opacity disagrees with this frame's is another surface's (disocclusion
                        // or a moved edge): weighed down rather than smeared.
                        float agree = clamp(1.0 - 2.0 * abs(history.a - current.a), 0.0, 1.0);
                        current = mix(current, history, HvHistory.x * agree);
                    }
                }
            }

            imageStore(MarchTexture, ivec2(pixel), current);
            imageStore(MarchGuideTexture, ivec2(pixel), vec4(sceneDistance, 0.0, 0.0, 0.0));
        }
    }
}
