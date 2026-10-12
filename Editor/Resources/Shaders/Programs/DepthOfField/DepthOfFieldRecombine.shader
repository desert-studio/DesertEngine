// DesertAsset {"Kind":"Shader","Guid":"abcc3cbbda024c19933609335b2a3e3b","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthOfFieldRecombine"
{
    Compute
    {
        // MR3 (View/DepthOfField.hpp). Pass 6 of 6, one invocation per OUTPUT pixel: the full-res in-focus
        // colour, the half-res background layer blended in by the pixel's own background CoC, and the foreground
        // layer over both by its opacity. A frame with nothing out of focus returns the colour unmodified.
        LocalSize(8, 8, 1);

        Uniform(0) sampler2D u_SceneColor; // linear HDR, OutputExtent
        Uniform(1) sampler2D u_SceneDepth; // reversed-Z device depth, RenderExtent
        Uniform(2) sampler2D u_Foreground; // HalfExtent: rgb, a opacity
        Uniform(3) sampler2D u_Background; // HalfExtent: rgb, a presence
        layout(binding = 4, rgba16f) restrict writeonly uniform image2D u_Output;

        // The C++ side is Graphic::DofParams (View/DepthOfField.hpp), offsets pinned by static_asserts.
        layout(std430, binding = 5) readonly buffer DepthOfFieldBuffer
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
            if (any(greaterThanEqual(pixel, ivec2(u_OutputSize))))
                return;
            const vec4  sharp      = texelFetch(u_SceneColor, pixel, 0);
            const vec2  uv         = (vec2(pixel) + 0.5) / u_OutputSize;
            const float coc        = CocRadiusOutput(SceneDistanceCm(textureLod(u_SceneDepth, uv, 0.0).r));
            const vec4  background = textureLod(u_Background, uv, 0.0);
            const vec4  foreground = textureLod(u_Foreground, uv, 0.0);

            // Within half an output pixel of focus the full-res colour stands; by 2 pixels the half-res layer.
            const float backgroundBlend = smoothstep(0.5, 2.0, coc) * background.a;
            vec3        color           = mix(sharp.rgb, background.rgb, backgroundBlend);
            color                       = mix(color, foreground.rgb, foreground.a);
            imageStore(u_Output, pixel, vec4(color, sharp.a));
        }
    }
}
