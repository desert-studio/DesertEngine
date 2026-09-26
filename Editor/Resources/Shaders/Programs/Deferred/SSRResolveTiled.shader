// DesertAsset {"Kind":"Shader","Guid":"a607db1b6c40417bbe22f774e11ba5bd","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSRResolveTiled"
{
    // SSR's resolve: the shared denoiser (Common/SSRDenoise.glslh) drawn only over the tiles next to a pixel the
    // trace can write (Common/SSRTiles.glslh). Everywhere else its output would be exactly 0, which is what the
    // accumulation target is cleared to.

    Vertex
    {
        #include <Common/QuadTextureCoords.glslh>
        #include <Common/SSRTiles.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	gl_Position = SSRTileVertex(1, v_TexCoord);
        }
    }

    Fragment
    {
        #include <Common/SSRDenoise.glslh>

        In(0) vec2 v_TexCoord;

        Uniform(1) sampler2D u_Trace;           // this frame's jittered estimate (rgb, a = weight)
        Uniform(2) sampler2D u_History;         // previous frame's resolved result
        Uniform(3) sampler2D u_GBufferWorldPos; // rgb = world position (for reprojection)

        Out(0) vec4 oColor;

        Uniform(0) SSRResolveUB
        {
        	mat4 u_PrevViewProj; // LAST frame's world -> clip
        	vec4 u_Params;       // xy = texel size, z = history blend (0 = first frame / resize), w unused
        };

        void main()
        {
        	oColor = SSRDenoise(u_Trace, u_History, u_GBufferWorldPos, v_TexCoord, u_PrevViewProj, u_Params);
        }
    }
}
