// DesertAsset {"Kind":"Shader","Guid":"6d1f3b0a8c2e4f5d9a7b1c3e5f709246","Versions":{"SHDR":1},"Dependencies":[]}
Shader "PopulateSceneDepth"
{
    // "Scene: PopulateSceneDepth" (TAA1-B step 6): the RENDER-extent scene depth into the OUTPUT-extent overlay
    // depth the overlay phases (debug lines, grid, gizmos, UI) test against after the temporal resolve (UE:
    // PopulateSceneDepth before the editor primitives). A full-screen triangle; depth test ALWAYS, depth write on.
    // Colour 0 is the overlay target's velocity slot, written as no motion: nothing after the resolve reads it and
    // the overlay pipelines never write it (colour write mask 0).

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

    Fragment
    {
        // POINT-sampled: the render pixel under the output pixel's centre, fetched. An upsampled depth is a depth
        // the scene really has there, never a blend of an edge's two surfaces (a surface that does not exist).

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_Depth;

        Out(0) vec4 oVelocity;

        void main()
        {
        	const ivec2 size  = textureSize(u_Depth, 0);
        	const ivec2 texel = clamp(ivec2(v_TexCoord * vec2(size)), ivec2(0), size - ivec2(1));
        	gl_FragDepth      = texelFetch(u_Depth, texel, 0).r;
        	oVelocity         = vec4(0.0);
        }
    }
}
