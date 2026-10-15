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
        layout(binding = 0, rgba16f) restrict writeonly uniform image2D u_FogApply;

        // The scene depth attachment, presented to this dispatch by ComputeImageBeginRead and handed
        // back afterwards. Point-sampled with texelFetch, never filtered: a filtered depth across a
        // silhouette averages foreground and background into a distance where nothing is.
        Uniform(2) sampler2D u_SceneDepth;

        // The sky's camera aerial-perspective volume (Graphic::kFogAerialPerspectiveBinding). Sampled
        // TRILINEARLY — across x and y so the 32x32 froxel grid does not show as blocks over a
        // silhouette, and across z because that interpolation is what turns 16 slices into a smooth
        // distance ramp instead of 16 visible shells.
        Uniform(3) sampler3D u_AerialPerspective;

        // The sky's DISTANT SKY LIGHT (Graphic::kFogDistantSkyLightBinding): one texel holding the
        // average radiance of the sky, marched this frame from 64 directions at 6 km
        // (Programs/Sky/SkyDistantLight.shader). It is the FULL-SPHERE mean — UE's arrangement
        // verbatim, and the right one here: a fog pixel is lit from every direction at once and has no
        // separate ground term to double-count against. Bound on the same terms as the volume above:
        // always bound, read only when u_FogAmbient.w says the value is real.
        Uniform(4) sampler2D u_DistantSkyLight;

        // The VOLUMETRIC FOG's integrated froxels (VolumetricFogRenderer, Programs/Fog/VolumetricFogIntegration.shader):
        // per froxel, the lit fog between the camera and that froxel's far edge, premultiplied, transmittance in
        // .a. Always bound (the engine's 3D fallback when the volume did not run); read only when
        // u_FogVolumetric0.w says it is real. Trilinear: the grid is coarse, and the filter is what hides it.
        Uniform(5) sampler3D u_VolumetricFog;

        // View depth (cm) of a slice coordinate of the froxel grid (Common/VolumetricFog.glslh's mapping, with
        // the parameters this pass gets in its own block).
        float VolumetricDepthFromSlice(float slice)
        {
            return (exp2(slice / u_FogVolumetric1.z) - u_FogVolumetric1.y) / u_FogVolumetric1.x;
        }

        PushConstant FogPush
        {
            mat4 u_InverseViewProjection;
            vec4 u_CameraPosition;   // xyz = camera position in world units, w unused
            vec4 u_ApParams;         // x = volume depth (km), y = view distance scale,
                                     // z = 1 when the volume exists, w = 1 when height fog is enabled
        };

        LocalSize(8, 8, 1);

        void main()
        {
            ivec2 size  = imageSize(u_FogApply);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            float deviceDepth = texelFetch(u_SceneDepth, coord, 0).r;

            vec2 uv  = (vec2(coord) + vec2(0.5f, 0.5f)) / vec2(size);
            vec2 ndc = vec2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

            // The world position from the camera's own inverse view-projection at the stored depth —
            // it inherits whatever projection and depth convention the frame was drawn with, and a sky
            // pixel (depth 0.0 under reversed-Z) lands on the far plane, which is exactly the distance
            // UE fogs it at. Only the sentinel below has to know which end of the range is far.
            vec4 worldH   = u_InverseViewProjection * vec4(ndc.x, ndc.y, deviceDepth, 1.0f);
            vec3 worldPos = worldH.xyz / max(worldH.w, 1e-9f);

            vec3 cameraKm = u_CameraPosition.xyz * (1.0f / HEIGHT_FOG_WORLD_UNITS_PER_KM);
            vec3 worldKm  = worldPos * (1.0f / HEIGHT_FOG_WORLD_UNITS_PER_KM);

            HeightFogResult fog;
            fog.Inscattering = vec3(0.0f, 0.0f, 0.0f);
            fog.Transmittance = 1.0f;
            if (u_ApParams.w > 0.5f)
            {
                HeightFogParams params = FogUnpackParams();

                // UE's ExcludeDistance: where the volumetric fog's grid reaches, the closed form starts - the same
                // medium must not be counted twice, once lit per froxel and once analytically.
                if (u_FogVolumetric0.w > 0.5f)
                {
                    float gridFarKm = VolumetricDepthFromSlice(u_FogVolumetric1.w) *
                                      (1.0f / HEIGHT_FOG_WORLD_UNITS_PER_KM);
                    params.StartDistanceKm    = max(params.StartDistanceKm, gridFarKm);
                    params.DirectionalStartKm = max(params.DirectionalStartKm, gridFarKm);
                }

                // THE SKY'S OWN AMBIENT, added to the fog's in-scattering colour before the closed form
                // integrates it — which is what makes it an ambient and not an overlay: the same
                // transmittance that hides distant geometry is what fades this in. In the physical
                // model it is the marched average sky (UE's Distant Sky Light); in the artistic
                // gradient the packer already folded the dome's mean into Inscattering and w is 0 here.
                if (u_FogAmbient.w > 0.5f)
                {
                    vec3 distantSky =
                         texelFetch(u_DistantSkyLight, ivec2(SKY_DISTANT_LIGHT_SPHERE_TEXEL, 0), 0).rgb;
                    params.Inscattering = params.Inscattering + u_FogAmbient.rgb * distantSky;
                }

                fog = HeightFogEvaluate(params, cameraKm, worldKm);
            }

            // The volumetric fog, IN FRONT of the analytic fog (it covers the near part of the ray, the closed form
            // the rest): rgb = Vol.rgb + Fog.rgb * Vol.a, a = Fog.a * Vol.a (UE CombineVolumetricFog). Sky pixels
            // read the grid's last slice, the whole grid in front of them.
            if (u_ApParams.w > 0.5f && u_FogVolumetric0.w > 0.5f)
            {
                // The view axis from the inverse view-projection: two points on the centre ray.
                vec4 nearH   = u_InverseViewProjection * vec4(0.0f, 0.0f, 1.0f, 1.0f);
                vec4 midH    = u_InverseViewProjection * vec4(0.0f, 0.0f, 0.5f, 1.0f);
                vec3 forward = normalize(midH.xyz / midH.w - nearH.xyz / nearH.w);
                float depthCm = max(dot(worldPos - u_CameraPosition.xyz, forward), 0.0f);
                float slice   = log2(max(depthCm * u_FogVolumetric1.x + u_FogVolumetric1.y, 1e-6f)) * u_FogVolumetric1.z;

                // Slice z of the volume holds the integral to its FAR edge (slice coordinate z + 1), so the
                // coordinate s reads texel s - 1; in front of slice 0's far edge it fades from the empty integral.
                vec2 gridUv = uv * vec2(size) / (u_FogVolumetric0.yz * u_FogVolumetric0.x);
                vec3 uvw    = vec3(gridUv, (clamp(slice, 1.0f, u_FogVolumetric1.w) - 0.5f) / u_FogVolumetric1.w);
                vec4 volume = mix(vec4(0.0f, 0.0f, 0.0f, 1.0f), texture(u_VolumetricFog, uvw), clamp(slice, 0.0f, 1.0f));

                fog.Inscattering  = volume.rgb + fog.Inscattering * volume.a;
                fog.Transmittance = fog.Transmittance * clamp(volume.a, 0.0f, 1.0f);
            }

            // Aerial perspective, on OPAQUE PIXELS ONLY. A sky pixel (depth at the far plane) already
            // carries the atmosphere's full radiance — the sky pass drew it from the Sky-View LUT, which
            // integrates the same medium out to the shell — so adding the volume's 96 km on top would be
            // the atmosphere counted twice, as a bright band exactly where the sky is thickest.
            vec4 ap = vec4(0.0f, 0.0f, 0.0f, 1.0f);
            // `> 0` IS THE SKY TEST under reversed-Z — a sky pixel stores 0, the far plane. Left as
            // `< 1.0` it would be true for every pixel including the sky, and the atmosphere would be
            // integrated twice across the whole upper half of the frame.
            if (u_ApParams.z > 0.5f && deviceDepth > 0.0f)
            {
                // Distance ALONG THE RAY, which is what the volume's slices measure — not the linear
                // view-space z, which would put a pixel at the corner of the frame in the wrong slice.
                float distanceKm =
                     length(worldKm - cameraKm) * max(u_ApParams.y, 0.0f);

                float sliceUnit = SkyApSliceUnitFromDistance(distanceKm, u_ApParams.x);

                // Read through the exact inverse of the fill's texel-centre remap on all three axes:
                // unit 0 is the centre of froxel 0 and unit 1 the centre of the last, so the frame's
                // edges and the volume's near plane land exactly on written texels.
                vec3 uvw = vec3(SkyUnitToTexelUv(uv.x, SKY_AP_VOLUME_WIDTH),
                                SkyUnitToTexelUv(uv.y, SKY_AP_VOLUME_HEIGHT),
                                SkyUnitToTexelUv(sliceUnit, SKY_AP_VOLUME_DEPTH));
                ap = texture(u_AerialPerspective, uvw);
            }

            // Fog OVER aerial perspective — see the header. Premultiplied throughout, so this is one
            // over-operator and nothing has to be un-premultiplied to apply it.
            vec3  inscattering = fog.Inscattering + ap.rgb * fog.Transmittance;
            float transmittance = fog.Transmittance * clamp(ap.a, 0.0f, 1.0f);

            imageStore(u_FogApply, coord, vec4(inscattering, transmittance));
        }
    }
}
