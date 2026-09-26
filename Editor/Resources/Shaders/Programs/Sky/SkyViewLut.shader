// DesertAsset {"Kind":"Shader","Guid":"381f95fd0dff4df68e2105d13bf7df7f","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkyViewLut"
{
    Compute
    {
        // The Sky-View LUT of the physical atmosphere (Hillaire 2020, section 5.3): the whole distant
        // sky for THIS camera and THIS sun, 192x104 RGBA16F, refilled every frame — it depends on the
        // camera's altitude and the sun's direction, the two things the cached transmittance /
        // multi-scattering LUTs deliberately do not.
        //
        // Per texel: invert the horizon-warped parameterisation into a (view zenith, light azimuth)
        // ray, then run the shared single-scattering integrator (Common/SkyScattering.glslh) with the
        // two cached LUTs supplying the sun transmittance and Psi_ms per step. rgb = in-scattered
        // luminance to the shell exit (or the ground), a = mean transmittance of the same path — the
        // lane the sky pass uses to sit the lit ground under the haze.
        //
        // The march runs in the LUT's local frame (+Y = the zenith at the camera, sun toward +X),
        // which is what keeps the parameterisation undistorted at altitude; only the view HEIGHT and
        // the sun's zenith angle survive from the world.

        #include <Common/Atmosphere.glslh>
        #include <Common/SkyMedium.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image2D u_SkyViewLut;

        // The SAME sky parameter buffer every other sky consumer reads (Graphic::kSkyPayloadBinding).
        ReadBuffer(1) SkyBuffer
        {
            vec4 u_SkyPacked[SKY_PACKED_VEC4_COUNT];
        };

        // The cached pair, valid whenever this pass runs — SkyboxRenderer dispatches them first.
        // Bindings = Graphic::kSkyTransmittanceLutBinding / kSkyMultiScatterLutBinding.
        Uniform(2) sampler2D u_TransmittanceLut;
        Uniform(3) sampler2D u_MultiScatterLut;

        // The one per-frame, per-view quantity the shared payload deliberately does not carry.
        // Mirrored by Graphic::SkyViewLutPush.
        PushConstant SkyViewPush
        {
            vec4 u_CameraPosWorld; // xyz = camera position in WORLD UNITS (centimetres)
        };

        // The transmittance LUT is addressed straight from its distance parameterisation: no
        // SkyUnitToTexelUv here, because Bruneton's mapping already puts the domain ends on the edge
        // texels. (Its write side DOES address texel centres — see SkyTransmittanceLut.shader — which
        // is what makes this read the exact inverse of that write; the two are different remaps and
        // the comment that used to stand here conflated them.) The multi-scatter LUT is parameterised
        // in unit range instead, so its read applies SkyUnitToTexelUv — again the inverse of its write.
        vec3 SkySampleSunTransmittanceLut(SkyAtmParams atm, float radiusKm, float sunZenithCos)
        {
            vec2 uv = SkyTransmittanceLutUvFromParams(atm.BottomRadiusKm, atm.TopRadiusKm, radiusKm,
                                                      sunZenithCos);
            return texture(u_TransmittanceLut, uv).rgb;
        }

        vec3 SkySampleMultiScatterLut(SkyAtmParams atm, float radiusKm, float sunZenithCos)
        {
            vec2 unit = SkyMultiScatterUnitFromParams(atm.BottomRadiusKm, atm.TopRadiusKm, radiusKm,
                                                      sunZenithCos);
            vec2 uv   = vec2(SkyUnitToTexelUv(unit.x, 32.0f), SkyUnitToTexelUv(unit.y, 32.0f));
            return texture(u_MultiScatterLut, uv).rgb;
        }

        #define SKY_SCATTERING_SUN_TRANSMITTANCE(atm, radiusKm, sunZenithCos) SkySampleSunTransmittanceLut(atm, radiusKm, sunZenithCos)
        #define SKY_SCATTERING_MULTI_SCATTER(atm, radiusKm, sunZenithCos) SkySampleMultiScatterLut(atm, radiusKm, sunZenithCos)

        #include <Common/SkyScattering.glslh>

        // Squared sample distribution (inside the integrator). The paper's Sky-View budget is ~32,
        // which is enough for a daylit sky but NOT for the sun within a degree of the horizon: the
        // far segments are then tens of kilometres long and the twilight terminator is still visibly
        // under-resolved even with the shadow faded (measured against 128- and 256-sample fills, 32
        // overshoots the sky either side of the sun by ~10%, 64 lands within a display level).
        // 192x104x64 marches per frame is a fraction of one full-screen post pass.
        const int kSkyViewSampleCount = 64;

        LocalSize(8, 8, 1);
        void main()
        {
            ivec2 size  = imageSize(u_SkyViewLut);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            SkyPacked s;
            for (int i = 0; i < SKY_PACKED_VEC4_COUNT; ++i)
                s.v[i] = u_SkyPacked[i];

            SkyAtmParams atm = SkyMakeAtmParams(UnpackMediumRayleigh(s), UnpackMediumMie(s),
                                                UnpackMediumMieAbsorption(s), UnpackMediumOzone(s),
                                                UnpackMediumGround(s), UnpackMediumTentPlanet(s));

            // Texel centres land exactly on the unit domain's ends; the sky pass reads back through
            // SkyUnitToTexelUv, the exact inverse.
            vec2 unit;
            unit.x = SkyTexelUvToUnit((float(coord.x) + 0.5f) / float(size.x), float(size.x));
            unit.y = SkyTexelUvToUnit((float(coord.y) + 0.5f) / float(size.y), float(size.y));

            // The camera's height above the planet centre, clamped inside the shell: below the 10 m
            // lift the horizon-warp's ground half degenerates, above the top the parameterisation is
            // not defined (a space view is out of scope for a ground-level engine).
            vec3  cameraKm   = u_CameraPosWorld.xyz / SKY_WORLD_UNITS_PER_KM;
            float viewHeight = clamp(SkyViewHeightKm(cameraKm, atm.BottomRadiusKm),
                                     atm.BottomRadiusKm + SKY_PLANET_RADIUS_OFFSET_KM,
                                     atm.TopRadiusKm - 0.01f);

            SkyViewLutCoord c = SkyViewParamsFromUnit(atm.BottomRadiusKm, viewHeight, unit);

            // Local frame: zenith = +Y, sun in the XY plane. The sun's zenith cosine is measured in
            // the WORLD (against the camera's real zenith) and survives the frame change.
            vec3  zenithWorld  = SkyViewZenith(cameraKm, atm.BottomRadiusKm);
            float sunZenithCos = clamp(dot(zenithWorld, UnpackSunDirection(s)), -1.0f, 1.0f);
            float sunZenithSin = sqrt(max(1.0f - sunZenithCos * sunZenithCos, 0.0f));

            vec3 originKm = vec3(0.0f, viewHeight, 0.0f);
            vec3 sunDir   = vec3(sunZenithSin, sunZenithCos, 0.0f);
            vec3 rayDir   = SkyViewRayDirection(c.ViewZenithCos, c.LightViewCos);

            // Outer-space sun illuminance: the authored sun colour times intensity — the same pair
            // the gradient's disc reads, reinterpreted as illuminance by the physical model.
            vec3 sunIlluminance = UnpackSkyConfig(s).sunColor * UnpackSunIntensity(s);

            SkyScatterResult result = SkyIntegrateScatteredLuminance(atm, originKm, rayDir, sunDir,
                                                                     sunIlluminance, kSkyViewSampleCount);

            // The art-direction tint that belongs INSIDE every scattering integration (UE's
            // SkyAndAerialPerspectiveLuminanceFactor). Equivalent to tinting the source function:
            // the integral is linear in it.
            vec3 luminance = result.Luminance * UnpackSkyAndAerialPerspectiveLuminanceFactor(s);

            float meanTransmittance =
                 (result.Transmittance.r + result.Transmittance.g + result.Transmittance.b) / 3.0f;

            imageStore(u_SkyViewLut, coord, vec4(luminance, meanTransmittance));
        }
    }
}
