Shader "UIText"
{
    // Screen-space distance-field text for the 2D batcher. Same vertex layout as UI2D (pos/uv/colour,
    // ortho push constant); the fragment reads a MULTI-CHANNEL glyph atlas (FontService) and
    // reconstructs coverage through Common/SdfText.glslh — crisp corners at any font size, tinted by
    // the vertex colour.
    //
    // It also draws ICONS (IconService), whose atlas carries one distance field replicated across RGB.
    // The median of three equal channels is that field, so icons reconstruct exactly as they did when
    // this shader sampled .r — no branch, no second shader, and no icon regression to pay for text.
    Vertex
    {
        In(0) vec2 a_Position;
        In(1) vec2 a_TexCoord;
        In(2) vec4 a_Color;

        Out(0) vec2 v_TexCoord;
        Out(1) vec4 v_Color;

        PushConstant constants
        {
            mat4 Projection;
        } m_PushConstants;

        void main()
        {
            v_TexCoord  = a_TexCoord;
            v_Color     = a_Color;
            gl_Position = m_PushConstants.Projection * vec4(a_Position, 0.0, 1.0);
        }
    }

    Fragment
    {
        In(0) vec2 v_TexCoord;
        In(1) vec4 v_Color;

        Uniform(0) sampler2D u_SDFAtlas;

        Out(0) vec4 o_Color;

        #include <Common/SdfText.glslh>

        void main()
        {
            vec3  msd         = texture(u_SDFAtlas, v_TexCoord).rgb;
            float pxRange     = SdfTextScreenPxRange(fwidth(v_TexCoord), vec2(textureSize(u_SDFAtlas, 0)));
            float alpha       = SdfTextAlpha(msd, pxRange);

            if (alpha <= 0.0)
                discard;

            o_Color = vec4(v_Color.rgb, v_Color.a * alpha);
        }
    }
}
