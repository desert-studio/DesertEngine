// DesertAsset {"Kind":"Shader","Guid":"3cb18fadc3404c698c689ce7693bea75","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MotionBlurFlatten"
{
    Compute
    {
        // MR2 (View/MotionBlur.hpp). Pass 1 of 4, one invocation per RENDER pixel: the velocity (NDC current -
        // previous, View/Velocity.hpp) in OUTPUT pixels of blur, scaled by u_VelocityScale (MotionBlurAmount and the
        // TargetFPS normalisation) and clamped to u_MaxPixels (MotionBlurMax), packed with the device depth for the
        // gather's depth compare. CPU twin: Graphic::MotionBlurPixelVelocity.
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_Velocity;   // RenderExtent
        Uniform(1) sampler2D u_SceneDepth; // reversed-Z device depth, RenderExtent
        layout(binding = 2, rgba16f) restrict writeonly uniform image2D u_Flat;

        // The C++ side is Graphic::MotionBlurParams (View/MotionBlur.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 3) readonly buffer MotionBlurBuffer
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

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;
            const vec2  ndc       = texelFetch(u_Velocity, pixel, 0).xy;
            vec2        blur      = ndc * vec2(0.5, -0.5) * u_OutputSize * u_VelocityScale;
            const float blurLength = length(blur);
            if (blurLength > u_MaxPixels && blurLength > 0.0)
                blur *= u_MaxPixels / blurLength;
            imageStore(u_Flat, pixel, vec4(blur, texelFetch(u_SceneDepth, pixel, 0).r, 0.0));
        }
    }
}
