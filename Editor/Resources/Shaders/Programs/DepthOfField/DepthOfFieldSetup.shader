// DesertAsset {"Kind":"Shader","Guid":"21ef5e42c3d04e79ab49a3547589cf2a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldSetup"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 1 of 6, one invocation per HALF-res pixel: the 2x2 average of the
        // resolved colour (one bilinear tap at the shared corner) and the signed CoC of the scene depth at its
        // centre, in HALF-res pixels.
        LocalSize(8, 8, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(4) sampler LinearClampSampler;
        Uniform(5) sampler PointClampSampler;
        Uniform(0) texture2D SceneColorTexture; // linear HDR, OutputExtent
        #define SceneColorTexture sampler2D(SceneColorTexture, LinearClampSampler)
        Uniform(1) texture2D SceneDepthTexture; // reversed-Z device depth, RenderExtent
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)
        layout(binding = 2, rgba16f) restrict writeonly uniform image2D HalfResolutionColorCocTexture;

        // The C++ side is Graphic::DofParams (View/DepthOfField.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 3) readonly buffer DepthOfFieldBuffer
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
            if (any(greaterThanEqual(pixel, ivec2(HalfSize))))
                return;
            const vec2  uv      = (vec2(pixel) + 0.5) / HalfSize;
            const vec3  color   = textureLod(SceneColorTexture, uv, 0.0).rgb;
            const float depth   = textureLod(SceneDepthTexture, uv, 0.0).r;
            const float cocHalf = 0.5 * CocRadiusOutput(SceneDistanceCm(depth));
            imageStore(HalfResolutionColorCocTexture, pixel, vec4(color, cocHalf));
        }
    }
}
