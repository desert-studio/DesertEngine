// DesertAsset {"Kind":"Shader","Guid":"c1ae2adbdc914bf58d758cd28ac56668","Versions":{"SHDR":1},"Dependencies":[]}
// The "Deformation Field" view mode (DeferredDebugMode::DeformationField; DEFORM-FIELD): every pixel's surface,
// reconstructed from the scene depth and the unjittered inverse view-projection, looks its world X, Z up in the
// deformation clipmap - level 0 where its window covers the point, level 1 farther out - and shows the field over
// a grey world: blue = sink (full at 8 cm), red = rim (full at 3 cm). The grey is a metre checker, slightly darker
// where level 1 answers and darkest where no level covers, so a print's world place and the level's reach are both
// readable. The sky is black.
Shader "DeformationFieldView"
{
    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>
        Out(0) vec2 v_Ndc;
        void main()
        {
        	v_Ndc       = FullscreenTriangleNdc();
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
    Fragment
    {
        In(0) vec2 v_Ndc;
        Uniform sampler PointClampSampler;
        Uniform(0) texture2D DeformHeight0Texture;
        #define DeformHeight0Texture sampler2D(DeformHeight0Texture, PointClampSampler)
        Uniform(1) texture2D DeformHeight1Texture;
        #define DeformHeight1Texture sampler2D(DeformHeight1Texture, PointClampSampler)
        Uniform(2) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, PointClampSampler)
        Out(0) vec4 oColor;

        PushConstant DeformViewPush
        {
            mat4  InverseViewProjection;
            ivec4 Windows; // xy: level 0's minimum cell, zw: level 1's
            vec4  Levels;  // x, y: texel size of level 0, 1 (cm); z, w: resolution of level 0, 1
        };

        bool InWindow(ivec2 cell, ivec2 window, int n)
        {
            return all(greaterThanEqual(cell, window)) && all(lessThan(cell, window + ivec2(n)));
        }

        void main()
        {
            // The depth is at the render resolution, the target may be the upscaled one: address it by the NDC.
            const ivec2 size  = textureSize(SceneDepthTexture, 0);
            const ivec2 pixel = clamp(ivec2((v_Ndc * 0.5 + 0.5) * vec2(size)), ivec2(0), size - 1);
            const float depth = texelFetch(SceneDepthTexture, pixel, 0).r;
            if (depth <= 0.0) // reversed-Z: 0 is the far plane, the sky
            {
                oColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }
            const vec4 h     = InverseViewProjection * vec4(v_Ndc, depth, 1.0);
            const vec3 world = h.xyz / h.w;

            const ivec2 n     = ivec2(Levels.zw);
            const ivec2 cell0 = ivec2(floor(world.xz / Levels.x));
            const ivec2 cell1 = ivec2(floor(world.xz / Levels.y));
            vec4  field = vec4(0.0);
            float grey  = 0.10;
            if (InWindow(cell0, Windows.xy, n.x))
            {
                field = texelFetch(DeformHeight0Texture, cell0 & ivec2(n.x - 1), 0);
                grey  = 0.30;
            }
            else if (InWindow(cell1, Windows.zw, n.y))
            {
                field = texelFetch(DeformHeight1Texture, cell1 & ivec2(n.y - 1), 0);
                grey  = 0.20;
            }
            const ivec2 metre   = ivec2(floor(world.xz / 100.0));
            const float checker = ((metre.x + metre.y) & 1) == 0 ? 1.0 : 0.85;
            const float sink    = clamp(-field.r / 8.0, 0.0, 1.0);
            const float rim     = clamp(field.g / 3.0, 0.0, 1.0);
            vec3 color = vec3(grey * checker);
            color      = mix(color, vec3(0.05, 0.25, 1.0), sink);
            color      = mix(color, vec3(1.0, 0.12, 0.05), rim);
            oColor     = vec4(color, 1.0);
        }
    }
}
