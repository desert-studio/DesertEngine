// DesertAsset {"Kind":"Shader","Guid":"835dbc3f60dc4be9a63b3bdc2f82fd22","Versions":{"SHDR":1},"Dependencies":[]}
Shader "IndirectView"
{
    // The indirect-only view mode (DeferredDebugMode::GI): the denoised diffuse indirect the deferred lighting
    // composed this frame (SSGI or RSM-GI, the ambient's `indirect` before the albedo), drawn over the post input by
    // "Debug: Indirect" after the temporal resolve (System::BufferViewRenderer says why it is not the composite's).

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

        Uniform(1) sampler2D u_Indirect;

        Out(0) vec4 oColor;

        void main()
        {
        	oColor = vec4(max(textureLod(u_Indirect, v_TexCoord, 0.0).rgb, vec3(0.0)), 1.0);
        }
    }
}
