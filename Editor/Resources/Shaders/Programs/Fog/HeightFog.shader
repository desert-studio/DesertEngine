// DesertAsset {"Kind":"Shader","Guid":"177731dbf442190ecea57129ede8d0e3","Versions":{"SHDR":1},"Dependencies":[]}
Shader "HeightFog"
{
    Compute
    {
        // The ATMOSPHERE-AND-FOG evaluation pass: the sky's aerial perspective on opaque geometry, with
        // the exponential height fog composed over it. One closed-form integral and one volume fetch per
        // pixel, no march — the march that produced the aerial perspective happened once for the whole
        // frame, into a 32x32x16 froxel volume (Programs/Sky/SkyAerialPerspectiveLut.shader).
        //
        // Writes RGBA16F: .rgb = in-scattered radiance, PREMULTIPLIED, linear HDR; .a = the transmittance
        // of everything between the camera and that pixel's geometry. The apply pass
        // (HeightFogApply.shader) then needs no more than `scene = rgb + scene * a`.
        //
        // COMPOSITION ORDER — fog OVER aerial perspective, which is UE's and is not arbitrary: the fog is
        // the optically thicker medium and sits between the camera and everything the atmosphere did,
        //
        //     rgb = Fog.rgb + AP.rgb * Fog.a
        //     a   = Fog.a   * AP.a
        //
        // (Docs/Sky/UE_SKYATMOSPHERE_RESEARCH.md section 1.6). Composing the other way round would let a
        // dense fog be tinted by 90 km of air in front of it.
        //
        // BOTH HALVES ARE OPTIONAL, and each absent half is the exact arithmetic identity of that
        // formula — not an approximation of it. A scene on SkyModel::ArtisticGradient has no volume, so
        // AP is (0,0,0,1) and the result is `Fog.rgb + 0*Fog.a`, `Fog.a * 1`: the same bits this pass
        // wrote before the atmosphere existed. A scene with a physical sky and no fog component is the
        // mirror image. That is why there is one shader here and no permutation.
        //
        // LINEAR HDR ONLY — no tonemap, no gamma, no exposure; the engine's tonemap owns the curve and
        // runs later in the frame.
        //
        // WHY COMPUTE AND NOT A FRAGMENT PASS: the fog needs the scene depth, and the fullscreen apply
        // draws into a framebuffer that has the depth attachment BOUND — sampling a bound attachment is
        // a feedback loop. The depth read therefore happens here, outside any render pass, with
        // ComputeImageBeginRead handling the layout round-trip. One path for Forward and Deferred: both
        // write this depth attachment, only Deferred has a G-buffer (teamlead decision Q5,
        // Docs/Sky/UE_SKYATMOSPHERE_RESEARCH.md section 5).
        //
        // SKY PIXELS ARE FOGGED. Depth clears to 0.0 where nothing was drawn (the engine is REVERSED-Z:
        // 0 is the far plane, see Core/Projection.hpp), and the reconstruction
        // below turns that into the far-plane point — fifty kilometres of fog toward the horizon, thinning
        // toward the zenith as the closed form's height term says, attenuated by StartDistance and
        // MaxOpacity. That is UE's behaviour (fog applies to skybox pixels through their far-plane
        // depth), and it is what produces the horizon veil. AERIAL PERSPECTIVE IS NOT applied to them —
        // the sky pass already drew the same medium integrated to the top of the atmosphere, and adding
        // the volume on top would be that integral counted twice.
        //
        // WHAT IS WHERE. The maths is Common/HeightFog.glslh, compiled as C++ by the HeightFog unit
        // tests from this same text; the parameter block is Common/FogParams.glslh, mirrored offset for
        // offset by Graphic::FogGpuPayload. What is left here is reconstruction and a store.

        #include <Common/HeightFog.glslh>
        #include <Common/FogParams.glslh>

        // The aerial-perspective volume's SLICE MAPPING only. SkyScattering.glslh's integrator block is
        // guarded on the two LUT-callback macros, which this pass deliberately does not define: it reads
        // the volume, it never fills it, so the only thing it needs from the sky's maths is the exact
        // inverse of the mapping the fill wrote through.
        #include <Common/SkyMedium.glslh>
        #include <Common/SkyScattering.glslh>

        // rgba16f: radiance is pre-tonemap HDR and transmittance is in [0,1]; half precision carries
        // three decimal digits, an order more than an over-operator needs.
        layout(binding = 0, rgba16f) restrict writeonly uniform image2D FogApplyTexture;

        // The scene depth attachment, presented to this dispatch by ComputeImageBeginRead and handed
        // back afterwards. Point-sampled with texelFetch, never filtered: a filtered depth across a
        // silhouette averages foreground and background into a distance where nothing is.
        // Shared sampler states (UE's shared samplers): every input below is a separate texture read through one
        // of these, bound once by the pass; a same-name macro after each texture keeps the sampled form.
        Uniform sampler GlobalFilterSampler;
        Uniform sampler LinearRepeatSampler;
        Uniform sampler LinearClampSampler;
        Uniform(2) texture2D SceneDepthTexture;
        #define SceneDepthTexture sampler2D(SceneDepthTexture, GlobalFilterSampler)

        // The sky's camera aerial-perspective volume (Graphic::kFogAerialPerspectiveBinding). Sampled
        // TRILINEARLY — across x and y so the 32x32 froxel grid does not show as blocks over a
        // silhouette, and across z because that interpolation is what turns 16 slices into a smooth
        // distance ramp instead of 16 visible shells.
        Uniform(3) texture3D AerialPerspectiveTexture;

        // The sky's DISTANT SKY LIGHT (Graphic::kFogDistantSkyLightBinding): one texel holding the
        // average radiance of the sky, marched this frame from 64 directions at 6 km
        // (Programs/Sky/SkyDistantLight.shader). It is the FULL-SPHERE mean — UE's arrangement
        // verbatim, and the right one here: a fog pixel is lit from every direction at once and has no
        // separate ground term to double-count against. Bound on the same terms as the volume above:
        // always bound, read only when FogAmbient.w says the value is real.
        Uniform(4) texture2D DistantSkyLightTexture;

        // The VOLUMETRIC FOG's integrated froxels (VolumetricFogRenderer, Programs/Fog/VolumetricFogIntegration.shader):
        // per froxel, the lit fog between the camera and that froxel's far edge, premultiplied, transmittance in
        // .a. Always bound (the engine's 3D fallback when the volume did not run); read only when
        // FogVolumetric0.w says it is real. Trilinear: the grid is coarse, and the filter is what hides it.
        Uniform(5) texture3D VolumetricFogTexture;

        PushConstant FogPush
        {
            mat4 InverseViewProjection;
        };

        // The evaluation itself (height fog, volumetric fog, aerial perspective, and their order) is
        // Common/ViewFog.glslh, the text every translucent surface evaluates at its own depth too. The AP
        // extent, the gates and the camera are in the FogParams block (FogView, FogGates, FogCamera),
        // not the push constant, because those surfaces read them from the same buffer.
        #include <Common/ViewFog.glslh>

        LocalSize(8, 8, 1);

        void main()
        {
            ivec2 size  = imageSize(FogApplyTexture);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            float deviceDepth = texelFetch(SceneDepthTexture, coord, 0).r;

            vec2 uv  = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(size);
            vec2 ndc = vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

            // The world position from the camera's own inverse view-projection at the stored depth —
            // it inherits whatever projection and depth convention the frame was drawn with, and a sky
            // pixel (depth 0.0 under reversed-Z) lands on the far plane, which is exactly the distance
            // UE fogs it at. Only the sentinel below has to know which end of the range is far.
            vec4 worldH   = InverseViewProjection * vec4(ndc.x, ndc.y, deviceDepth, 1.0f);
            vec3 worldPos = worldH.xyz / max(worldH.w, 1e-9f);

            // The OPAQUE depth: this image is the composite for the opaque scene only. Translucent surfaces
            // are drawn after it and fog themselves at their own depth. ViewFogEvaluate pre-exposes the
            // inscattering (FogGates.z = ViewFrame::PreExposure); the transmittance is a ratio.
            imageStore(FogApplyTexture, coord,
                       ViewFogEvaluate(uv, worldPos, deviceDepth > 0.0f, AerialPerspectiveTexture,
                                       LinearRepeatSampler, DistantSkyLightTexture, GlobalFilterSampler,
                                       VolumetricFogTexture, LinearClampSampler));
        }
    }
}
