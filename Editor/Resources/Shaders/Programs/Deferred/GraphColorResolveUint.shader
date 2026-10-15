// DesertAsset {"Kind":"Shader","Guid":"3fdc0b81f82446a5a873f6bb0fb9970a","Versions":{"SHDR":1},"Dependencies":[]}
Shader "GraphColorResolveUint"
{
    // "<Color>: Resolve" (GraphColorResolveRenderer, ViewRasterTargets.hpp AddGraphColorResolves) of an INTEGER
    // graph colour declared GraphColorResolve::SampleZero (the forward depth prepass's contact-shadow shading word,
    // ViewTargetFormats::kGBufferShadingWord) from its multisampled attachment into the single-sample one: the
    // twin of GraphColorResolve.shader with an unsigned sampler and output, since a float output into an integer
    // attachment is undefined. A full-screen triangle; no depth.

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
        // Sample 0 of the pixel, fetched: the bits one surface at that pixel really wrote.

        Uniform(1) usampler2DMS u_Input;

        Out(0) uvec4 oColor;

        void main()
        {
        	oColor = texelFetch(u_Input, ivec2(gl_FragCoord.xy), 0);
        }
    }
}
