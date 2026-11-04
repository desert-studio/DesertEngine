// DesertAsset {"Kind":"Shader","Guid":"3cb18fadc3404c698c689ce7693bea75","Versions":{"SHDR":1},"Dependencies":[]}
Shader "MotionBlurFlatten"
{
    Compute
    {
        // MR2 (View/MotionBlur.hpp). Pass 1 of 4, one invocation per RENDER pixel: the velocity (NDC current -
        // previous, View/Velocity.hpp) in OUTPUT pixels of blur, scaled by VelocityScale (MotionBlurAmount and the
        // TargetFPS normalisation) and clamped to MaxPixels (MotionBlurMax), packed with the device depth for the
        // gather's depth compare. CPU twin: Graphic::MotionBlurPixelVelocity.
        //
        // A pixel nothing wrote velocity for (the sky, the clouds, the far plane: velocity 0 at reversed-Z depth 0)
        // keeps the clear value; its motion is the camera's at infinity (Common/Velocity.glslh
        // DesertCameraVelocityAtInfinity through ViewFrame::ClipToPrevClip, one formula with TemporalAA.shader's sky
        // branch and UpscalerVelocity), as UE's VelocityFlatten derives the camera motion where no velocity was
        // written. A turning camera so blurs the sky with the hills in front of it: no sharp sky band riding with
        // the camera along the horizon silhouette. A still camera gives ClipToPrevClip = identity, velocity 0.
        LocalSize(8, 8, 1);

        #include <Common/ScreenUV.glslh>
        #include <Common/Velocity.glslh>

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
            mat4  ClipToPrevClip; // ViewFrame::ClipToPrevClip (unjittered); Flatten's sky branch
        };

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(RenderSize))))
                return;
            vec2        ndcVelocity = texelFetch(VelocityTexture, pixel, 0).xy;
            const float depth       = texelFetch(SceneDepthTexture, pixel, 0).r;
            if (ndcVelocity == vec2(0.0) && depth <= 0.0)
            {
                const vec2 uv = (vec2(pixel) + 0.5) / RenderSize;
                ndcVelocity   = DesertCameraVelocityAtInfinity(ScreenUVToNdc(uv), ClipToPrevClip);
            }
            vec2        blur       = ndcVelocity * vec2(0.5, -0.5) * OutputSize * VelocityScale;
            const float blurLength = length(blur);
            if (blurLength > MaxPixels && blurLength > 0.0)
                blur *= MaxPixels / blurLength;
            imageStore(FlattenedVelocityTexture, pixel, vec4(blur, depth, 0.0));
        }
    }
}
