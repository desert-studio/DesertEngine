// DesertAsset {"Kind":"Shader","Guid":"a39b7f07331d4b2488100b2ed0ec19d1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "GraphColorResolve"
{
    // "<Color>: Resolve" (GraphColorResolveRenderer, ViewRasterTargets.hpp AddGraphColorResolves): a graph colour
    // declared GraphColorResolve::SampleZero (the view's velocity) from its multisampled attachment into the
    // single-sample one. A full-screen triangle; no depth.

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
        // Sample 0 of the pixel, fetched: the resolved value is one a surface at that pixel really has, never the
        // average of an edge's two (the mean of two motion vectors is a motion nothing on screen makes).

        Uniform(1) sampler2DMS u_Input;

        Out(0) vec4 oColor;

        void main()
        {
        	oColor = texelFetch(u_Input, ivec2(gl_FragCoord.xy), 0);
        }
    }
}
