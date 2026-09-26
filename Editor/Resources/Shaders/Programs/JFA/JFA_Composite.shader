// DesertAsset {"Kind":"Shader","Guid":"26886a6ee6613e93e8cae917eee73023","Versions":{"SHDR":1},"Dependencies":[]}
Shader "JFA_Composite"
{
    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
            gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }

    Fragment
    {
        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Color;

        Uniform(0, 0) sampler2D u_OutlineResult;

        void main()
        {
            o_Color = texture(u_OutlineResult, v_TexCoord);
        }
    }
}
