// DesertAsset {"Kind":"Shader","Guid":"c9cec12e730bf5c878453552ce30f594","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SMAABlend"
{
    Fragment
    {
        // SMAA pass 3: neighborhood blending. Reads the tonemapped color + the blend-weights texture and
        // produces the final anti-aliased color.
        #include <Common/SMAA.glslh>

        In(0)  vec2 v_TexCoord;
        Uniform(2) sampler2D u_ColorTex;
        Uniform(3) sampler2D u_BlendTex;
        Uniform(4) sampler2D u_EdgesTex; // DIAGNOSTIC only
        Uniform(5) sampler2D u_AreaTex;  // DIAGNOSTIC only
        Out(0) vec4 oColor;

        // DIAGNOSTIC: 1 = grayscale blend-weight magnitude (x4); 0 = normal SMAA output.
        #define SMAA_DEBUG_VIEW 0

        void main()
        {
            vec2 ts      = vec2( textureSize( u_ColorTex, 0 ) );
            vec4 metrics = vec4( 1.0 / ts, ts );

        #if SMAA_DEBUG_VIEW
            vec4  w    = texture( u_BlendTex, v_TexCoord );          // blend weights
            float wmag = ( w.r + w.g + w.b + w.a );
            oColor = vec4( vec3( min( wmag * 4.0, 1.0 ) ), 1.0 );
            return;
        #else
            oColor = SMAANeighborhoodBlending( u_ColorTex, u_BlendTex, v_TexCoord, metrics );
        #endif
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
