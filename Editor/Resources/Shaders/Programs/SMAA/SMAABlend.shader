// DesertAsset {"Kind":"Shader","Guid":"c9cec12e730bf5c878453552ce30f594","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SMAABlend"
{
    Fragment
    {
        // SMAA pass 3: neighborhood blending. Reads the tonemapped color + the blend-weights texture and
        // produces the final anti-aliased color.
        // The one sampler state of every input below (separate textures); Common/SMAA.glslh samples through it.
        Uniform(6) sampler u_LinearClampSampler;
        #include <Common/SMAA.glslh>

        In(0)  vec2 v_TexCoord;
        Uniform(2) texture2D u_ColorTex;
        Uniform(3) texture2D u_BlendTex;
        Uniform(4) texture2D u_EdgesTex; // DIAGNOSTIC only
        Uniform(5) texture2D u_AreaTex;  // DIAGNOSTIC only
        Out(0) vec4 oColor;

        // DIAGNOSTIC: 1 = grayscale blend-weight magnitude (x4); 0 = normal SMAA output.
        #define SMAA_DEBUG_VIEW 0

        void main()
        {
            vec2 ts      = vec2( textureSize( sampler2D( u_ColorTex, u_LinearClampSampler ), 0 ) );
            vec4 metrics = vec4( 1.0 / ts, ts );

        #if SMAA_DEBUG_VIEW
            vec4  w    = texture( sampler2D( u_BlendTex, u_LinearClampSampler ), v_TexCoord );          // blend weights
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
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord  = FullscreenTriangleUV();
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
}
