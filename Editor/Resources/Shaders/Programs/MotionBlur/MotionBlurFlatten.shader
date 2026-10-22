// DesertAsset {"Kind":"Shader","Guid":"3cb18fadc3404c698c689ce7693bea75","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MotionBlurFlatten"
{
    Compute
    {
        // MR2 (View/MotionBlur.hpp). Pass 1 of 4, one invocation per RENDER pixel: the velocity (NDC current -
        // previous, View/Velocity.hpp) in OUTPUT pixels of blur, scaled by VelocityScale (MotionBlurAmount and the
        // TargetFPS normalisation) and clamped to MaxPixels (MotionBlurMax), packed with the device depth for the
        // gather's depth compare. CPU twin: Graphic::MotionBlurPixelVelocity.
        LocalSize(8, 8, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(4) sampler PointClampSampler;
        Uniform(0) texture2D VelocityTexture;   // RenderExtent
        #define VelocityTexture sampler2D(VelocityTexture, PointClampSampler)
        Uniform(1) texture2D SceneDepthTexture; // reversed-Z device depth, RenderExtent
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)
        layout(binding = 2, rgba16f) restrict writeonly uniform image2D FlattenedVelocityTexture;

        // The C++ side is Graphic::MotionBlurParams (View/MotionBlur.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 3) readonly buffer MotionBlurBuffer
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
        };

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(RenderSize))))
                return;
            const vec2  ndc       = texelFetch(VelocityTexture, pixel, 0).xy;
            vec2        blur      = ndc * vec2(0.5, -0.5) * OutputSize * VelocityScale;
            const float blurLength = length(blur);
            if (blurLength > MaxPixels && blurLength > 0.0)
                blur *= MaxPixels / blurLength;
            imageStore(FlattenedVelocityTexture, pixel, vec4(blur, texelFetch(SceneDepthTexture, pixel, 0).r, 0.0));
        }
    }
}
