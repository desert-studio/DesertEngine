// DesertAsset {"Kind":"Shader","Guid":"594354b3edff49e19c1f82ba73312809","Versions":{"SHDR":1},"Dependencies":[]}
// HDR-OUTPUT: the editor's last pass on the scRGB output (DisplayOutput::scRGB_Linear, macOS EDR). The interface
// is drawn, as on SDR, in the sRGB-ENCODED space (ImGui's colours and the viewport image are encoded values), into
// a half-float composite where the scene's highlights carry encoded values above 1. This pass decodes the
// extended sRGB transfer (IEC 61966-2-1, continued past 1) to scRGB linear: UI white (1.0) lands on paper white
// (scRGB 1.0 = the display's SDR white), the scene's highlights above it up to the display's headroom.
// UE: the HDR UI composite (r.HDR.UI.CompositeMode) — the UI on paper white, the scene in the output transform.
Shader "DisplayEncodeScRGB"
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

        Uniform(1) sampler LinearClampSampler;
        Uniform(0) texture2D FinalColorTexture;
        #define FinalColorTexture sampler2D(FinalColorTexture, LinearClampSampler)

        Out(0) vec4 o_Color;

        vec3 SrgbToLinearExtended(vec3 encoded)
        {
            const vec3 e = max(encoded, vec3(0.0));
            return mix(e / 12.92, pow((e + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), e));
        }

        void main()
        {
            // texelFetch: the composite is the back buffer's own size, one texel per pixel.
            vec4 encoded = texelFetch(FinalColorTexture, ivec2(gl_FragCoord.xy), 0);
            o_Color      = vec4(SrgbToLinearExtended(encoded.rgb), 1.0);
        }
    }
}
