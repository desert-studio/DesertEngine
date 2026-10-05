// DesertAsset {"Kind":"Shader","Guid":"153c47916ea0c4d04785b281a7888c6a","Versions":{"SHDR":1},"Dependencies":[]}
// Plain fullscreen copy of an (already tonemapped) image into the current framebuffer — used by the
// standalone runtime to present the scene's final image to the swapchain WITHOUT ImGui. No tonemap here
// (that already ran); just sample and write.
Shader "SwapchainBlit"
{
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
        In(0) vec2 v_TexCoord;

        Uniform(0) sampler2D u_Texture;

        Out(0) vec4 o_Color;

        void main()
        {
            o_Color = texture(u_Texture, v_TexCoord);
        }
    }
}
