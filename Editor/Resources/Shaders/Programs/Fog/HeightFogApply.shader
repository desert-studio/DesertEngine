// DesertAsset {"Kind":"Shader","Guid":"9f6026a483d3b89154b2785ba5c094df","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HeightFogApply"
{
    Fragment
    {
        // The height fog's APPLY: a fullscreen triangle in RenderPhase::Transparency, drawn with a LOAD
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
        // NO TONEMAP, NO GAMMA, NO EXPOSURE here: linear HDR onto linear HDR. The view's PreExposure is already in
        // the fog image's inscattering (HeightFog.shader writes it multiplied, FogPush::Exposure).

        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Color;

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(1) sampler u_PointClampSampler;
        Uniform(0) texture2D u_FogApply;
        #define u_FogApply sampler2D(u_FogApply, u_PointClampSampler)

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
        #include <Common/FullscreenTriangle.glslh>

        Out(0) vec2 v_TexCoord;

        void main()
        {
            v_TexCoord  = FullscreenTriangleUV();
            gl_Position = vec4(FullscreenTriangleNdc(), 0.0, 1.0);
        }
    }
}
