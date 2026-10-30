// DesertAsset {"Kind":"Shader","Guid":"7987993943858babcf5c00130fe0f8c9","Versions":{"SHDR":1},"Dependencies":[]}
Shader "JFA_Init"
{
    Fragment
    {
        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Seed;

        // Shared sampler states (UE's shared samplers): each input below is a separate texture read through one.

        Uniform(0, 1) sampler PointClampSampler;

        Uniform(0, 0) texture2D StencilTexture;

        #define StencilTexture sampler2D(StencilTexture, PointClampSampler)

        void main()
        {
            float mask = texture(StencilTexture, v_TexCoord).r;

            // The silhouette mask is rendered white (1.0) for selected meshes on top of the
            // framebuffer clear color (~0.1). A 0.5 threshold cleanly separates seed from background.
            if (mask > 0.5)
            {
                ivec2 texSize = textureSize(StencilTexture, 0);
                vec2 pixelCoord = v_TexCoord * vec2(texSize);
                o_Seed = vec4(pixelCoord, 0.0, 1.0);
            }
            else
            {
                o_Seed = vec4(-1.0, -1.0, 0.0, 0.0);
            }
        }
    }

    Vertex
    {
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord = FullscreenTriangleUV();
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
}
