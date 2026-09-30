// DesertAsset {"Kind":"Shader","Guid":"fbf88c9b60544afabbfa8523d0cd553c","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SceneDepthResolve"
{
    // "Scene: DepthResolve" (SceneDepthResolveRenderer): the multisampled scene depth (deferred AND forward
    // geometry) into the single-sample SceneDepthResolved that the compute passes reading scene depth (height
    // fog, volumetric clouds) sample at MSAA > 1. A full-screen quad; depth test ALWAYS, depth write on.

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
        // Sample 0 of the pixel, fetched: the resolved depth is a real depth of the scene at that pixel, never an
        // average of an edge's two surfaces (a depth between them would be a surface that does not exist).

        Uniform(1) sampler2DMS u_Depth;

        void main()
        {
        	gl_FragDepth = texelFetch(u_Depth, ivec2(gl_FragCoord.xy), 0).r;
        }
    }
}
