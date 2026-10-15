// DesertAsset {"Kind":"Shader","Guid":"dfe8213be6f74957997c94060b56accf","Versions":{"SHDR":1},"Dependencies":[]}
Shader "DepthExpand"
{
    // "Deferred: DepthExpand": the single-sample G-buffer depth written into every sample of the multisampled
    // scene depth (DepthExpandRenderer). A full-screen triangle; depth test ALWAYS, depth write on.

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        void main()
        {
        	gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }

    Fragment
    {
        // The pixel's own texel, fetched, not filtered: every sample of the pixel receives the G-buffer's depth
        // exactly. The fragment runs once per pixel and gl_FragDepth is written to every covered sample.

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(0) sampler u_PointClampSampler;
        Uniform(1) texture2D u_Depth;
        #define u_Depth sampler2D(u_Depth, u_PointClampSampler)

        void main()
        {
        	gl_FragDepth = texelFetch(u_Depth, ivec2(gl_FragCoord.xy), 0).r;
        }
    }
}
