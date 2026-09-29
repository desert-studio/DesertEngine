// DesertAsset {"Kind":"Shader","Guid":"4450da2582658b2b6f4214c5d2031c08","Versions":{"SHDR":1},"Dependencies":[]}
Shader "UI2D"
{
    // Screen-space 2D batcher shader (UI, sprites, text). One dynamic vertex+index buffer is filled by the
    // Render2D batcher each frame; every quad carries its own tint colour and UV. Solid shapes sample a 1x1
    // white texture so `texture * colour` collapses to the flat colour. Blend / depth are set by the pipeline.
    Vertex
    {
        In(0) vec2 a_Position;   // pixel coordinates (top-left origin)
        In(1) vec2 a_TexCoord;
        In(2) vec4 a_Color;

        Out(0) vec2 v_TexCoord;
        Out(1) vec4 v_Color;

        // Orthographic pixel->clip projection, pushed per batch by the Render2D backend.
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

        Uniform(0) sampler2D u_Texture;

        Out(0) vec4 o_Color;

        void main()
        {
            o_Color = texture(u_Texture, v_TexCoord) * v_Color;
        }
    }
}
