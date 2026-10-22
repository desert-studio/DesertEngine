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

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(4) sampler PointClampSampler;
        Uniform(1) texture2D SceneColorTexture; // linear HDR, RenderExtent, jittered: the opaque scene
        #define SceneColorTexture sampler2D(SceneColorTexture, PointClampSampler)
        Uniform(2) texture2D ExposureTexture;   // 1x1 previous adapted luminance
        layout(binding = 3, r32f) restrict writeonly uniform image2D FlickeringLumaTexture;

        void main()
        {
            const ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
            if (any(greaterThanEqual(pixel, ivec2(RenderSize))))
                return;
            const vec3  linearColor = max(texelFetch(SceneColorTexture, pixel, 0).rgb, vec3(0.0)) * TsrExposure(ExposureTexture, PointClampSampler);
            const vec3  smColor     = GCSToSMCS(LinearToGCS(linearColor));
            const float smLuma      = dot(smColor, vec3(1.0 / kMoireLumaToChannel));
            imageStore(FlickeringLumaTexture, pixel, vec4(sqrt(smLuma)));
        }
    }
}
