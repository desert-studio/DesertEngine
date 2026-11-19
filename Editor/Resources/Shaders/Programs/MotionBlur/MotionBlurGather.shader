// DesertAsset {"Kind":"Shader","Guid":"004f65036edc437d9820e9f9d60f603d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MotionBlurGather"
{
    Compute
    {
        // MR2 (View/MotionBlur.hpp). Pass 4 of 4, one invocation per OUTPUT pixel: Samples taps of the resolved
        // colour along the neighbourhood's dominant velocity (McGuire 2012 reconstruction, the shape of UE's
        // MotionBlurGather): a tap in front of the centre counts where its own velocity reaches the centre, a tap
        // behind it where the centre's velocity reaches it, and both when they move together. A pixel whose
        // neighbourhood does not move returns its colour unmodified (a still frame is bit-equal).
        //
        // Tile classification (Jimenez, "Next Generation Post Processing in Call of Duty: Advanced Warfare", 2014;
        // UE's MotionBlurGather fast / slow tiles). The tap count comes from the neighbourhood's own blur length,
        // one tap per kPixelsPerTap pixels, between kMinSamples and the quality's Samples (the cap): a short blur
        // does not pay for 16 taps it cannot resolve. A neighbourhood whose slowest pixel moves at least
        // kFastRatio of its fastest moves as one (a camera turn over most of the screen): every tap's velocity is
        // the dominant one, so the slow path's weight collapses to the depth-free form below and the per-tap
        // velocity fetch is skipped. NeighborMax is per 16x16 render tile, so an 8x8 group reads one class.
        LocalSize(8, 8, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(5) sampler LinearClampSampler;
        Uniform(6) sampler PointClampSampler;
        Uniform(0) texture2D SceneColorTexture;  // linear HDR, OutputExtent
        #define SceneColorTexture sampler2D(SceneColorTexture, LinearClampSampler)
        Uniform(1) texture2D FlattenedVelocityTexture;        // output-pixel velocity (xy), device depth (z); RenderExtent
        #define FlattenedVelocityTexture sampler2D(FlattenedVelocityTexture, PointClampSampler)
        Uniform(2) texture2D NeighborMaxTexture; // tile extent
        #define NeighborMaxTexture sampler2D(NeighborMaxTexture, PointClampSampler)
        layout(binding = 3, rgba16f) restrict writeonly uniform image2D BlurredSceneColorTexture;

        // The C++ side is Graphic::MotionBlurParams (View/MotionBlur.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 4) readonly buffer MotionBlurBuffer
        {
            vec2  RenderSize;
            vec2  OutputSize;
            vec2  TileCount;
            float VelocityScale;
            float MaxPixels;
            int   Samples;
            float Pad0;
            float Pad1;
            float Pad2;
            mat4  ClipToPrevClip; // ViewFrame::ClipToPrevClip (unjittered); Flatten's sky branch
        };

        const float kSoftDepth    = 50.0; // relative depth difference over which front / back fades
        const float kPixelsPerTap = 1.0;  // blur length (output pixels) one tap covers
        const int   kMinSamples   = 4;    // fewest taps on a moving neighbourhood
        const float kFastRatio    = 0.9;  // neighbourhood min / max velocity length at or above which it moves as one

        // 1 when a is in front of (or level with) b, fading to 0 as it falls behind. Reversed-Z device depth is
        // ~ 1 / view dist, so the ratio a / b is scale-free.
        float InFront(float a, float b)
        {
            return clamp(1.0 + kSoftDepth * (a / max(b, 1e-7) - 1.0), 0.0, 1.0);
        }

        float Cone(float dist, float velocityLength)
        {
            return clamp(1.0 - dist / max(velocityLength, 1e-4), 0.0, 1.0);
        }

        float Cylinder(float dist, float velocityLength)
        {
            return 1.0 - smoothstep(0.95 * velocityLength, 1.05 * velocityLength, dist);
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(OutputSize))))
                return;
            const vec4  centreColor    = texelFetch(SceneColorTexture, pixel, 0);
            const vec2  uv             = (vec2(pixel) + 0.5) / OutputSize;
            const vec3  neighborMax    = textureLod(NeighborMaxTexture, uv, 0.0).xyz; // xy longest, z slowest
            const vec2  dominant       = neighborMax.xy;
            const float dominantLength = length(dominant);
            if (dominantLength < 0.5)
            {
                imageStore(BlurredSceneColorTexture, pixel, centreColor);
                return;
            }

            // Interleaved gradient noise: the taps' offset along the velocity varies per pixel (no banding).
            const float jitter  = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715)))) - 0.5;
            const int   cap     = max(Samples, 1);
            const int   samples = clamp(int(ceil(dominantLength / kPixelsPerTap)), min(kMinSamples, cap), cap);

            vec3  sum;
            float weight;
            if (neighborMax.z >= kFastRatio * dominantLength)
            {
                // Fast path: centre and taps move with the dominant velocity at one depth (front = back = 1), the
                // slow path's weight with tapLength = centreLength = dominantLength.
                sum    = centreColor.rgb / dominantLength;
                weight = 1.0 / dominantLength;
                for (int i = 0; i < samples; ++i)
                {
                    const float t      = mix(-0.5, 0.5, (float(i) + 0.5 + jitter * 0.5) / float(samples));
                    const vec2  offset = dominant * t;
                    const float dist   = length(offset);
                    const float cyl    = Cylinder(dist, dominantLength);
                    const float w      = 2.0 * Cone(dist, dominantLength) + cyl * cyl * 2.0;
                    sum += textureLod(SceneColorTexture, uv + offset / OutputSize, 0.0).rgb * w;
                    weight += w;
                }
            }
            else
            {
                const vec3  centreFlat   = textureLod(FlattenedVelocityTexture, uv, 0.0).xyz;
                const float centreLength = max(length(centreFlat.xy), 0.5);
                sum                      = centreColor.rgb / centreLength;
                weight                   = 1.0 / centreLength;
                for (int i = 0; i < samples; ++i)
                {
                    // Taps over the open shutter, -0.5 .. 0.5 of the dominant velocity.
                    const float t         = mix(-0.5, 0.5, (float(i) + 0.5 + jitter * 0.5) / float(samples));
                    const vec2  offset    = dominant * t;
                    const vec2  tapUv     = uv + offset / OutputSize;
                    const float dist      = length(offset);
                    const vec3  tapFlat   = textureLod(FlattenedVelocityTexture, tapUv, 0.0).xyz;
                    const float tapLength = length(tapFlat.xy);
                    const float front     = InFront(tapFlat.z, centreFlat.z);
                    const float back      = InFront(centreFlat.z, tapFlat.z);
                    const float w = front * Cone(dist, tapLength) + back * Cone(dist, centreLength) +
                                    Cylinder(dist, tapLength) * Cylinder(dist, centreLength) * 2.0;
                    sum += textureLod(SceneColorTexture, tapUv, 0.0).rgb * w;
                    weight += w;
                }
            }
            imageStore(BlurredSceneColorTexture, pixel, vec4(sum / weight, centreColor.a));
        }
    }
}
