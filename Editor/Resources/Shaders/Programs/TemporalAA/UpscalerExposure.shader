// DesertAsset {"Kind":"Shader","Guid":"986d26a6f61441df87448005a93032af","Versions":{"SHDR":1},"Dependencies":[]}
Shader "UpscalerExposure"
{
    Compute
    {
        // The exposure image a vendor temporal upscaler takes (Graphic::UpscalerExposure): 1x1, the MULTIPLIER
        // the resolve tonemaps its samples by. It is 1 / adapted luminance — the exposure TemporalAA.shader's
        // HdrWeight reads from the same 1x1 AutoExposure image — so a vendor upscaler weighs samples in the space
        // the engine's own TAA does. Without auto exposure the input is the white system texture: exposure 1.
        // NOT divided by ViewFrame::PreExposure: FSR3 (dispatch.preExposure) and DLSS (sl preExposure) receive the
        // pre-exposure themselves and divide it out of the colour BEFORE this exposure (UE hands DLSS the absolute
        // eye-adaptation exposure next to View.PreExposure the same way).

        LocalSize(1, 1, 1);

        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform(2) sampler PointClampSampler;
        Uniform(0) texture2D AdaptedLuminanceTexture;
        #define AdaptedLuminanceTexture sampler2D(AdaptedLuminanceTexture, PointClampSampler)
        layout(binding = 1, r32f) restrict writeonly uniform image2D ExposureTexture;

        void main()
        {
            const float adaptedLum = texelFetch(AdaptedLuminanceTexture, ivec2(0), 0).r;
            imageStore(ExposureTexture, ivec2(0), vec4(1.0 / max(adaptedLum, 1e-4), 0.0, 0.0, 0.0));
        }
    }
}
