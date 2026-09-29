// DesertAsset {"Kind":"Shader","Guid":"9f6026a483d3b89154b2785ba5c094df","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HeightFogApply"
{
    Fragment
    {
        // The height fog's APPLY: a fullscreen quad in RenderPhase::Transparency, drawn with a LOAD
        // begin over the finished scene colour, at RenderPassOrder::AtmosphericFog — below everything
        // else the phase composites, so particles land OVER the fogged scene rather than under it.
        //
        // The pipeline supplies the blend: source factor One, destination factor SrcAlpha, i.e.
        //
        //     scene = fog.rgb + scene * fog.a
        //
        // the premultiplied over-operator with .a carrying TRANSMITTANCE. The compute pass
        // (HeightFog.shader) produced exactly that pair, so this shader has nothing to reconstruct —
        // and it CANNOT read the scene depth itself (the render pass has the depth attachment bound;
        // sampling a bound attachment is a feedback loop), which is the whole reason the evaluation
        // lives in compute.
        //
        // The fog image is the target's own size, so the fetch is one texel per pixel — no filtering to
        // smear a silhouette, and nothing for a bilateral guide to fix.
        //
        // NO TONEMAP, NO GAMMA, NO EXPOSURE: linear HDR onto linear HDR.

        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Color;

        Uniform(0) sampler2D u_FogApply;

        void main()
        {
            ivec2 size  = textureSize(u_FogApply, 0);
            ivec2 coord = clamp(ivec2(v_TexCoord * vec2(size)), ivec2(0, 0), size - ivec2(1, 1));
            vec4  fog   = texelFetch(u_FogApply, coord, 0);

            o_Color = vec4(fog.rgb, clamp(fog.a, 0.0, 1.0));
        }
    }

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
}
