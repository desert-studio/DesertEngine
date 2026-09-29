// DesertAsset {"Kind":"Shader","Guid":"6ad853a043fe5bd0b7648f4d24d54bec","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SMAAEdges"
{
    Fragment
    {
        // SMAA pass 1: luma edge detection. Reads the tonemapped LDR color; writes 2-channel edges into .rg.
        #include <Common/SMAA.glslh>

        In(0)  vec2 v_TexCoord;
        Uniform(2) sampler2D u_ColorTex;
        Out(0) vec4 oColor;

        void main()
        {
            vec2 edges = SMAALumaEdgeDetection( u_ColorTex, v_TexCoord );
            oColor = vec4( edges, 0.0, 0.0 );
        }
    }

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord  = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
            gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }
}
