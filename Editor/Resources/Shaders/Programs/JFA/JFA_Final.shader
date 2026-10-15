// DesertAsset {"Kind":"Shader","Guid":"7ab518b286f27364cf60d70d21ef5aa5","Versions":{"SHDR":1},"Dependencies":[]}
Shader "JFA_Final"
{
    // "PostFX: JumpFloodFinal" (JumpFloodOutlineRenderer): the selection outline as an editor overlay on the DISPLAY
    // image, after the tonemap and the spatial AA, as UE's SelectionOutline pass (EPass::SelectionOutline, before
    // EPass::EditorPrimitive). The outline colour is display-linear like UE's selection colour; it is encoded by the
    // view's own sRGB output device (FilmEncodeDisplay at the view's Film Gamma, the encoding the tonemap wrote) and
    // blended (One, OneMinusSrcAlpha) over the tonemapped image, so neither the exposure, the bloom nor the tone curve
    // touches it. Where no outline falls the fragment is discarded and the display image stays as the tonemap left it.
    Fragment
    {
        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Color;

        Uniform(0, 0) sampler2D u_JFATexture;

        Uniform(2) JFAFinalUB
        {
            vec4  u_OutlineColor;
            float u_OutlineWidth;
            float u_Smoothness;
            float u_DisplayGamma; // the view's Film Gamma (the tonemap's output device)
        };

        #include <Common/FilmToneMap.glslh>

        void main()
        {
            ivec2 texSize = textureSize(u_JFATexture, 0);
            vec2 pixelCoord = v_TexCoord * vec2(texSize);

            vec4 nearestSeed = texture(u_JFATexture, v_TexCoord);
            // No seed reached this texel: nothing is selected near it.
            if (nearestSeed.x < 0.0)
                discard;

            float dist = distance(nearestSeed.xy, pixelCoord);
            // Inside the selected object: the scene shows through.
            if (dist < 0.5)
                discard;

            // Outline band: smoothstep for soft edges.
            float alpha = 1.0 - smoothstep(u_OutlineWidth - u_Smoothness, u_OutlineWidth, dist);
            if (alpha <= 0.0)
                discard;

            o_Color = vec4(FilmEncodeDisplay(u_OutlineColor.rgb, u_DisplayGamma) * alpha, alpha);
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
