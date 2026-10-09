// DesertAsset {"Kind":"Shader","Guid":"004f65036edc437d9820e9f9d60f603d","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MotionBlurGather"
{
    Compute
    {
        // MR2 (View/MotionBlur.hpp). Pass 4 of 4, one invocation per OUTPUT pixel: u_Samples taps of the resolved
        // colour along the neighbourhood's dominant velocity (McGuire 2012 reconstruction, the shape of UE's
        // MotionBlurGather): a tap in front of the centre counts where its own velocity reaches the centre, a tap
        // behind it where the centre's velocity reaches it, and both when they move together. A pixel whose
        // neighbourhood does not move returns its colour unmodified (a still frame is bit-equal).
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_SceneColor;  // linear HDR, OutputExtent
        Uniform(1) sampler2D u_Flat;        // output-pixel velocity (xy), device depth (z); RenderExtent
        Uniform(2) sampler2D u_NeighborMax; // tile extent
        layout(binding = 3, rgba16f) restrict writeonly uniform image2D u_Output;

        // The C++ side is Graphic::MotionBlurParams (View/MotionBlur.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 4) readonly buffer MotionBlurBuffer
        {
            vec2  u_RenderSize;
            vec2  u_OutputSize;
            vec2  u_TileCount;
            float u_VelocityScale;
            float u_MaxPixels;
            int   u_Samples;
            float u_Pad0;
            float u_Pad1;
            float u_Pad2;
        };

        const float kSoftDepth = 50.0; // relative depth difference over which front / back fades

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
            if (any(greaterThanEqual(pixel, ivec2(u_OutputSize))))
                return;
            const vec4  centreColor    = texelFetch(u_SceneColor, pixel, 0);
            const vec2  uv             = (vec2(pixel) + 0.5) / u_OutputSize;
            const vec2  dominant       = textureLod(u_NeighborMax, uv, 0.0).xy;
            const float dominantLength = length(dominant);
            if (dominantLength < 0.5)
            {
                imageStore(u_Output, pixel, centreColor);
                return;
            }

            const vec3  centreFlat   = textureLod(u_Flat, uv, 0.0).xyz;
            const float centreLength = max(length(centreFlat.xy), 0.5);
            // Interleaved gradient noise: the taps' offset along the velocity varies per pixel (no banding).
            const float jitter = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715)))) - 0.5;

            vec3      sum     = centreColor.rgb / centreLength;
            float     weight  = 1.0 / centreLength;
            const int samples = max(u_Samples, 1);
            for (int i = 0; i < samples; ++i)
            {
                // Taps over the open shutter, -0.5 .. 0.5 of the dominant velocity.
                const float t         = mix(-0.5, 0.5, (float(i) + 0.5 + jitter * 0.5) / float(samples));
                const vec2  offset    = dominant * t;
                const vec2  tapUv     = uv + offset / u_OutputSize;
                const float dist  = length(offset);
                const vec3  tapFlat   = textureLod(u_Flat, tapUv, 0.0).xyz;
                const float tapLength = length(tapFlat.xy);
                const float front     = InFront(tapFlat.z, centreFlat.z);
                const float back      = InFront(centreFlat.z, tapFlat.z);
                const float w = front * Cone(dist, tapLength) + back * Cone(dist, centreLength) +
                                Cylinder(dist, tapLength) * Cylinder(dist, centreLength) * 2.0;
                sum += textureLod(u_SceneColor, tapUv, 0.0).rgb * w;
                weight += w;
            }
            imageStore(u_Output, pixel, vec4(sum / weight, centreColor.a));
        }
    }
}
