// DesertAsset {"Kind":"Shader","Guid":"427c3aa249fa0efeada3b23f641960fb","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SSRResolve"
{
    // Shared denoiser for the 1-sample-per-pixel jittered estimates: used by SSR (trace) and by the
    // RSM-GI gather. Spatial 5x5 alpha-weighted tent + AABB-clamped temporal accumulation.

    Vertex
    {
        #include <Common/QuadPositions.glslh>
        #include <Common/QuadTextureCoords.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
        	v_TexCoord  = QUAD_TEXTURE_COORDINATES[gl_VertexIndex];
        	gl_Position = vec4(QUAD_POSITIONS[gl_VertexIndex], 0.0, 1.0);
        }
    }

    Fragment
    {
        // Fullscreen: the GI gather writes every pixel, so its resolve has nothing to skip. SSR draws the same
        // body over its reflecting tiles only (SSRResolveTiled.shader).
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
        	oColor = SSRDenoise(u_Trace, u_History, u_GBufferWorldPos, v_TexCoord, u_PrevViewProj, u_Params,
        	                    2, 1.0);
        }
    }
}
