// DesertAsset {"Kind":"Shader","Guid":"21ef5e42c3d04e79ab49a3547589cf2a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldSetup"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 1 of 6, one invocation per HALF-res pixel: the 2x2 average of the
        // resolved colour (one bilinear tap at the shared corner) and the signed CoC of the scene depth at its
        // centre, in HALF-res pixels.
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_SceneColor; // linear HDR, OutputExtent
        Uniform(1) sampler2D u_SceneDepth; // reversed-Z device depth, RenderExtent
        layout(binding = 2, rgba16f) restrict writeonly uniform image2D u_Half;

        // The C++ side is Graphic::DofParams (View/DepthOfField.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 3) readonly buffer DepthOfFieldBuffer
        {
            vec2  u_OutputSize;
            vec2  u_RenderSize;
            vec2  u_HalfSize;
            vec2  u_TileCount;
            vec4  u_DepthToView;
            float u_FocalDistanceMm;
            float u_FocalLengthMm;
            float u_CocScale;
            float u_MaxRadius;
            int   u_Rings;
            int   u_DilateTiles;
            float u_Pad0;
            float u_Pad1;
        };

        // Reversed-Z device depth -> distance along the view axis, cm (CPU twin: Graphic::DofSceneDistanceCm).
        float SceneDistanceCm(float deviceDepth)
        {
            const float w = u_DepthToView.z * deviceDepth + u_DepthToView.w;
            if (abs(w) < 1e-12)
                return 1e30;
            return abs((u_DepthToView.x * deviceDepth + u_DepthToView.y) / w);
        }

        // The signed thin-lens CoC radius in OUTPUT pixels, negative in front of the focus plane
        // (CPU twin: Graphic::DofCocRadiusPixels): A f / 2 * W / SensorWidth * (1 - s / z) / (s - f).
        float CocRadiusOutput(float distanceCm)
        {
            const float z = distanceCm * 10.0;
            const float t = (1.0 - u_FocalDistanceMm / z) / (u_FocalDistanceMm - u_FocalLengthMm);
            return clamp(u_CocScale * t, -u_MaxRadius, u_MaxRadius);
        }

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_HalfSize))))
                return;
            const vec2  uv      = (vec2(pixel) + 0.5) / u_HalfSize;
            const vec3  color   = textureLod(u_SceneColor, uv, 0.0).rgb;
            const float depth   = textureLod(u_SceneDepth, uv, 0.0).r;
            const float cocHalf = 0.5 * CocRadiusOutput(SceneDistanceCm(depth));
            imageStore(u_Half, pixel, vec4(color, cocHalf));
        }
    }
}
