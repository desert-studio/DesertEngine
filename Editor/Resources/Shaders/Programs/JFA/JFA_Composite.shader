// DesertAsset {"Kind":"Shader","Guid":"26886a6ee6613e93e8cae917eee73023","Versions":{"SHDR":1},"Dependencies":[]}
Shader "JFA_Composite"
{
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

    Fragment
    {
        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Color;

        Uniform(0, 0) texture2D u_OutlineResult;
        Uniform(1, 0) sampler u_LinearClampSampler;
        #define u_OutlineResult sampler2D(u_OutlineResult, u_LinearClampSampler)

        void main()
        {
            o_Color = texture(u_OutlineResult, v_TexCoord);
        }
    }
}
