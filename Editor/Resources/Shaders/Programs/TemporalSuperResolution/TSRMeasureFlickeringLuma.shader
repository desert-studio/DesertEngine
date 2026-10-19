// DesertAsset {"Kind":"Shader","Guid":"93af14e3345b48a5ad66b6480d746389","Versions":{"SHDR":1},"Dependencies":[]}
Shader "TSRMeasureFlickeringLuma"
{
    Compute
    {
        // TSR.MeasureFlickeringLuma (TemporalSuperResolution::AddOpaquePasses: before the translucency composites
        // into the scene colour). Ported from UE 5.8 Engine/Shaders/Private/TemporalSuperResolution/
        // TSRMeasureFlickeringLuma.usf MeasureFlickeringLumaCS :21-69, adapted: one pixel per lane on an 8x8 group
        // (UE: TILE_ROWS x TILE_ROWS pixels per lane), the pre-exposure is AutoExposure's previous adapted
        // luminance (TsrExposure, as every TSR pass) and the output is a plain R32F texture instead of slice 0 of
        // UE's flickering-luma array. The luma: the pre-exposed colour in the shading-measurement space, its
        // channels weighted by kMoireSMCSWeights (1/3 each), back to the guide space (UE SMCSToGCS: sqrt).
        // TSR.DetectThinGeometry's intensity lines read it (UE FlickeringLumaTexture).
        LocalSize(8, 8, 1);

        #include <Common/TSRCommon.glslh>

        Uniform(1) sampler2D u_SceneColor; // linear HDR, RenderExtent, jittered: the opaque scene
        Uniform(2) sampler2D u_Exposure;   // 1x1 previous adapted luminance
        layout(binding = 3, r32f) restrict writeonly uniform image2D u_FlickeringLuma;

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(u_RenderSize))))
                return;
            const vec3  linearColor = max(texelFetch(u_SceneColor, pixel, 0).rgb, vec3(0.0)) * TsrExposure(u_Exposure);
            const vec3  smColor     = GCSToSMCS(LinearToGCS(linearColor));
            const float smLuma      = dot(smColor, vec3(1.0 / kMoireLumaToChannel));
            imageStore(u_FlickeringLuma, pixel, vec4(sqrt(smLuma)));
        }
    }
}
