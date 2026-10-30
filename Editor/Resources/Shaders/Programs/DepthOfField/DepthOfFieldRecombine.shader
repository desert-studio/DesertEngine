// DesertAsset {"Kind":"Shader","Guid":"abcc3cbbda024c19933609335b2a3e3b","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldRecombine"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 6 of 6, one invocation per OUTPUT pixel: the full-res in-focus
        // colour, the half-res background layer blended in by the pixel's own background CoC, and the foreground
        // layer over both by its opacity. A frame with nothing out of focus returns the colour unmodified.
        LocalSize(8, 8, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(6) sampler PointClampSampler;
        Uniform(7) sampler LinearClampSampler;
        Uniform(0) texture2D SceneColorTexture; // linear HDR, OutputExtent
        #define SceneColorTexture sampler2D(SceneColorTexture, PointClampSampler)
        Uniform(1) texture2D SceneDepthTexture; // reversed-Z device depth, RenderExtent
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)
        Uniform(2) texture2D ForegroundTexture; // HalfExtent: rgb, a opacity
        #define ForegroundTexture sampler2D(ForegroundTexture, LinearClampSampler)
        Uniform(3) texture2D BackgroundTexture; // HalfExtent: rgb, a presence
        #define BackgroundTexture sampler2D(BackgroundTexture, LinearClampSampler)
        layout(binding = 4, rgba16f) restrict writeonly uniform image2D RecombinedColorTexture;

        // The C++ side is Graphic::DofParams (View/DepthOfField.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 5) readonly buffer DepthOfFieldBuffer
        {
            vec2  OutputSize;
            vec2  RenderSize;
            vec2  HalfSize;
            vec2  TileCount;
            vec4  DepthToView;
            float FocalDistanceMm;
            float FocalLengthMm;
            float CocScale;
            float MaxRadius;
            int   Rings;
            int   DilateTiles;
            float Pad0;
            float Pad1;
        };

        // Reversed-Z device depth -> distance along the view axis, cm (CPU twin: Graphic::DofSceneDistanceCm).
        float SceneDistanceCm(float deviceDepth)
        {
            const float w = DepthToView.z * deviceDepth + DepthToView.w;
            if (abs(w) < 1e-12)
                return 1e30;
            return abs((DepthToView.x * deviceDepth + DepthToView.y) / w);
        }

        // The signed thin-lens CoC radius in OUTPUT pixels, negative in front of the focus plane
        // (CPU twin: Graphic::DofCocRadiusPixels): A f / 2 * W / SensorWidth * (1 - s / z) / (s - f).
        float CocRadiusOutput(float distanceCm)
        {
            const float z = distanceCm * 10.0;
            const float t = (1.0 - FocalDistanceMm / z) / (FocalDistanceMm - FocalLengthMm);
            return clamp(CocScale * t, -MaxRadius, MaxRadius);
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(OutputSize))))
                return;
            const vec4  sharp      = texelFetch(SceneColorTexture, pixel, 0);
            const vec2  uv         = (vec2(pixel) + 0.5) / OutputSize;
            const float coc        = CocRadiusOutput(SceneDistanceCm(textureLod(SceneDepthTexture, uv, 0.0).r));
            const vec4  background = textureLod(BackgroundTexture, uv, 0.0);
            const vec4  foreground = textureLod(ForegroundTexture, uv, 0.0);

            // Within half an output pixel of focus the full-res colour stands; by 2 pixels the half-res layer.
            const float backgroundBlend = smoothstep(0.5, 2.0, coc) * background.a;
            vec3        color           = mix(sharp.rgb, background.rgb, backgroundBlend);
            color                       = mix(color, foreground.rgb, foreground.a);
            imageStore(RecombinedColorTexture, pixel, vec4(color, sharp.a));
        }
    }
}
