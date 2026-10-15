// DesertAsset {"Kind":"Shader","Guid":"7ab518b286f27364cf60d70d21ef5aa5","Versions":{"SHDR":1},"Dependencies":[]}
Shader "JFA_Final"
{
    Fragment
    {
        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Color;

        // Shared sampler states (UE's shared samplers): each input below is a separate texture read through one.

        Uniform(0, 3) sampler u_PointClampSampler;

        Uniform(0, 4) sampler u_LinearClampSampler;

        Uniform(0, 0) texture2D u_JFATexture;

        #define u_JFATexture sampler2D(u_JFATexture, u_PointClampSampler)
        Uniform(0, 1) texture2D u_SceneTexture;
        #define u_SceneTexture sampler2D(u_SceneTexture, u_LinearClampSampler)

        Uniform(2) JFAFinalUB
        {
            vec4  u_OutlineColor;
            float u_OutlineWidth;
            float u_Smoothness;
        };

        void main()
        {
            vec3 sceneColor = texture(u_SceneTexture, v_TexCoord).rgb;

            // Outline disabled (width <= 0): pass the scene through unchanged.
            if (u_OutlineWidth <= 0.0)
            {
                o_Color = vec4(sceneColor, 1.0);
                return;
            }

            ivec2 texSize = textureSize(u_JFATexture, 0);
            vec2 pixelCoord = v_TexCoord * vec2(texSize);

            vec4 nearestSeed = texture(u_JFATexture, v_TexCoord);

            if (nearestSeed.x < 0.0)
            {
                o_Color = vec4(sceneColor, 1.0);
                return;
            }

            float dist = distance(nearestSeed.xy, pixelCoord);

            // Inside the object — keep scene color
            if (dist < 0.5)
            {
                o_Color = vec4(sceneColor, 1.0);
                return;
            }

            // Outline band: smoothstep for soft edges
            float alpha = 1.0 - smoothstep(u_OutlineWidth - u_Smoothness, u_OutlineWidth, dist);

            vec3 finalColor = mix(sceneColor, u_OutlineColor.rgb, alpha);
            o_Color = vec4(finalColor, 1.0);
        }
    }

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord = FullscreenTriangleUV();
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
}
