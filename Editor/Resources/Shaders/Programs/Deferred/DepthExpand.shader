// DesertAsset {"Kind":"Shader","Guid":"dfe8213be6f74957997c94060b56accf","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthExpand"
{
    // "Deferred: DepthExpand": the single-sample G-buffer depth written into every sample of the multisampled
    // scene depth (DepthExpandRenderer). A full-screen quad; depth test ALWAYS, depth write on.

    Vertex
    {
        #include <Common/QuadPositions.glslh>

        void main()
        {
        	gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }

    Fragment
    {
        // The pixel's own texel, fetched, not filtered: every sample of the pixel receives the G-buffer's depth
        // exactly. The fragment runs once per pixel and gl_FragDepth is written to every covered sample.

        Uniform(1) sampler2D u_Depth;

        void main()
        {
        	gl_FragDepth = texelFetch(u_Depth, ivec2(gl_FragCoord.xy), 0).r;
        }
    }
}
