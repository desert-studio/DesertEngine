// DesertAsset {"Kind":"Shader","Guid":"d92bf93f92b232211b4976212b867170","Versions":{"SHDR":1},"Dependencies":[]}
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

    // The glyph / icon atlas is this material's own texture (Render2D caches one executor per atlas).
    Properties
    {
        Texture2D   SdfAtlasTexture ("SDF Atlas")
    }

    Vertex
    {
        // Generated from the C++ vertex format (Graphic/ShaderProtocols/VertexInputs.hpp).
        #include <Common/UIVertexInputs.glslh>

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

        // A bound texture property: a separate texture read through its own sampler state `<name>Sampler`,
        // which the material writes from the property (VulkanMaterialBackend::ApplyTexture2D).
        Uniform(0) texture2D SdfAtlasTexture;
        Uniform(1) sampler SdfAtlasTextureSampler;
        #define SdfAtlasTexture sampler2D(SdfAtlasTexture, SdfAtlasTextureSampler)

        Out(0) vec4 o_Color;

        #include <Common/SdfText.glslh>

        void main()
        {
            vec3  msd         = texture(SdfAtlasTexture, v_TexCoord).rgb;
            float pxRange     = SdfTextScreenPxRange(fwidth(v_TexCoord), vec2(textureSize(SdfAtlasTexture, 0)));
            float alpha       = SdfTextAlpha(msd, pxRange);

            if (alpha <= 0.0)
                discard;

            o_Color = vec4(v_Color.rgb, v_Color.a * alpha);
        }
    }
}
