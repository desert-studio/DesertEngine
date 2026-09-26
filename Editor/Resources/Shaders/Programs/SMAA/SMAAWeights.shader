// DesertAsset {"Kind":"Shader","Guid":"ebd206d43aef76d36f1f6edb027a6818","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SMAAWeights"
{
    Fragment
    {
        // SMAA pass 2: blending-weight calculation. Reads the edges texture + the precomputed AreaTex/SearchTex
        // LUTs; writes per-direction blend weights (rgba).
        #include <Common/SMAA.glslh>

        In(0)  vec2 v_TexCoord;
        Uniform(2) sampler2D u_EdgesTex;
        Uniform(3) sampler2D u_AreaTex;
        Uniform(4) sampler2D u_SearchTex;
        Out(0) vec4 oColor;

        void main()
        {
            vec2 ts      = vec2( textureSize( u_EdgesTex, 0 ) );
            vec4 metrics = vec4( 1.0 / ts, ts );
            oColor = SMAABlendingWeightCalculation( u_EdgesTex, u_AreaTex, u_SearchTex, v_TexCoord, metrics );
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
