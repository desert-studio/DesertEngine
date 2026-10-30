// DesertAsset {"Kind":"Shader","Guid":"98a7911d387d07d3b6bf5a431308dac0","Versions":{"SHDR":1},"Dependencies":[]}
Shader "CloudComposite"
{
    Fragment
    {
        // The cloud APPLY: a fullscreen triangle in RenderPhase::Transparency at RenderPassOrder::FarField —
        // ABOVE the height fog (AtmosphericFog, -100 below this rung) and BELOW everything the phase
        // composites by default, so particles and translucency land over the clouds rather than under
        // them. FarField exists in RenderGraphSort.hpp for exactly this: "content at sky distance".
        //
        // The pipeline supplies the blend: source factor One, destination factor SrcAlpha, i.e.
        //
        //     scene = cloud.rgb + scene * cloud.a
        //
        // the premultiplied over-operator with .a carrying TRANSMITTANCE, not opacity. The march
        // (CloudRaymarch.shader) produced exactly that pair, so nothing is reconstructed here.
        //
        // OCCLUSION WAS ALREADY RESOLVED. The march cut every ray at the farthest surface of its block and
        // CloudUpsample removed, per pixel, the cloud lying behind that pixel's own surface, so a cloud behind a
        // mountain or a leaf contributes nothing here. This pass therefore runs with the depth test off and
        // could not read the depth attachment anyway — it is bound by the render pass, and sampling a
        // bound attachment is a feedback loop.
        //
        // WHAT THIS PASS READS: the FULL-RESOLUTION cloud image CloudUpsample.shader wrote this frame, one
        // texel per pixel, fetched 1:1. The march traces at a quarter of the framebuffer, CloudTemporalResolve
        // reconstructs half resolution from four such frames, and the upsample to full resolution happens in
        // that compute pass because it needs each pixel's OWN scene depth — which this pass cannot read: the
        // depth attachment is bound by the render pass it draws into, and sampling a bound attachment is a
        // feedback loop.
        //
        // THE UPSAMPLE USED TO LIVE HERE (FOLIAGE-CLOUD-FLICKER). Without the pixel's depth it compared the
        // half-res neighbours against the BILINEAR BLEND of their own distances, which at a grass blade or a
        // leaf against the sky is a mixture of the two and says nothing about which one this pixel is: blades
        // took the sky's cloud on some frames and not on others, the constant flicker of foliage against
        // clouds. CloudUpsample.shader says what replaced it.
        //
        // NO TONEMAP, NO GAMMA, NO EXPOSURE: linear HDR onto linear HDR -- except the view's PreExposure, the scale
        // the scene colour is stored at (ViewFrame::PreExposure): the radiance is multiplied by it, the transmittance
        // is not. The march, the cloud history and the upsampled image stay absolute.

        In(0) vec2 v_TexCoord;

        Out(0) vec4 o_Color;

        // Shared sampler state (UE's shared samplers), bound once by the pass; the same-name macro after the
        // texture keeps the sampled form. The image is texelFetched, so the filter never applies.
        Uniform(2) sampler LinearRepeatSampler;
        Uniform(0) texture2D CloudScatterTexture;
        #define CloudScatterTexture sampler2D(CloudScatterTexture, LinearRepeatSampler)

        Uniform(3) CloudCompositeUB
        {
            vec4 Exposure; // x = ViewFrame::PreExposure (MaterialCloudComposite::SetExposure), yzw unused
        };

        void main()
        {
            // The image has the framebuffer's size, so the fragment's own pixel is its texel. Through
            // v_TexCoord rather than gl_FragCoord: it is the uv every cloud pass indexes its images with
            // (CloudUpsample un-projects texel (i + 0.5) / size), so the two cannot disagree about which way up
            // the image is.
            ivec2 size  = textureSize(CloudScatterTexture, 0);
            ivec2 pixel = clamp(ivec2(floor(v_TexCoord * vec2(size))), ivec2(0, 0), size - ivec2(1, 1));
            vec4  cloud = texelFetch(CloudScatterTexture, pixel, 0);

            // The upsample already clamped both channels; transmittance is clamped again because it multiplies
            // the scene, and a value above 1 brightens what is BEHIND the cloud.
            o_Color = vec4(max(cloud.rgb, vec3(0.0)) * Exposure.x, clamp(cloud.a, 0.0, 1.0));
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
