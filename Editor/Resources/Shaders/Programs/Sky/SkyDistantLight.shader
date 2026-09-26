// DesertAsset {"Kind":"Shader","Guid":"75fb91a1185f5eb76af600b723aa6746","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkyDistantLight"
{
    Compute
    {
        // The DISTANT SKY LIGHT of the physical atmosphere (UE's Distant Sky Light LUT): the average
        // radiance of the sky seen from near the ground, refilled every frame because it depends on
        // the sun.
        //
        // ONE TEXEL, ONE MARCH. Texel (0,0) is the FULL-SPHERE mean — UE's own quantity, and what the
        // height fog reads: a fog pixel is lit from every direction at once and has no ground term of
        // its own. The mean is Common/SkyScattering.glslh's SkyDistantLight, which the tests drive.
        //
        // ONE WORKGROUP, 64 THREADS, ONE DIRECTION EACH. The direction set and the per-direction
        // radiance are Common/SkyScattering.glslh's SkyDistantLightDirection / SkyDistantLightRadiance —
        // the same text the SkyScattering tests compile as C++ — so the only thing this file adds is the
        // groupshared reduction and the store. That split is deliberate: a sum written twice (once here,
        // once in a C++ reference) is two answers to one question, and the two would drift on the first
        // tuning pass.
        //
        // WHY A REDUCTION AND NOT 64 SEQUENTIAL MARCHES IN ONE THREAD: each direction is a 32-step
        // atmosphere march with two LUT fetches per step. In one thread that is ~2000 dependent texture
        // reads on the critical path of every frame; spread over a workgroup it is one march deep.
        //
        // WHY A MEAN AND NOT A CUBEMAP: this value's consumers want the MEAN sky, not its directional
        // variation — the height fog's ambient in-scattering is isotropic by construction (the
        // closed-form line integral has no direction in it), and directional ambient is what the IBL
        // bake's irradiance cube already is.

        #include <Common/Atmosphere.glslh>
        #include <Common/SkyMedium.glslh>

        // rgba32f for one texel: the cost of the exact format is a rounding error here, and the value is
        // read by a pass that adds it to a fog colour, where a half's three decimal digits at very low
        // night radiances would quantise visibly.
        layout(binding = 0, rgba32f) restrict writeonly uniform image2D u_DistantSkyLight;

        // The SAME sky parameter buffer every other sky consumer reads (Graphic::kSkyPayloadBinding).
        ReadBuffer(1) SkyBuffer
        {
            vec4 u_SkyPacked[SKY_PACKED_VEC4_COUNT];
        };

        // The cached pair, valid whenever this pass runs — SkyboxRenderer dispatches them first.
        // Bindings = Graphic::kSkyTransmittanceLutBinding / kSkyMultiScatterLutBinding.
        Uniform(2) sampler2D u_TransmittanceLut;
        Uniform(3) sampler2D u_MultiScatterLut;

        // The same read-side mappings every other consumer of the cached pair uses: raw uv for the
        // transmittance LUT, texel-centre remap for the multi-scatter LUT — each the exact inverse of
        // its write side.
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

        shared vec3 s_Radiance[64];

        LocalSize(64, 1, 1);
        void main()
        {
            uint index = gl_LocalInvocationIndex;

            SkyPacked s;
            for (int i = 0; i < SKY_PACKED_VEC4_COUNT; ++i)
                s.v[i] = u_SkyPacked[i];

            SkyAtmParams atm = SkyMakeAtmParams(UnpackMediumRayleigh(s), UnpackMediumMie(s),
                                                UnpackMediumMieAbsorption(s), UnpackMediumOzone(s),
                                                UnpackMediumGround(s), UnpackMediumTentPlanet(s));

            // The march runs in the frame whose +Y is the zenith at the evaluation point, exactly like
            // the Sky-View LUT's: only the sun's zenith angle survives from the world, and the sky is
            // symmetric about the sun's azimuth, so the azimuth of the frame is free.
            float sunZenithCos = clamp(UnpackSunDirection(s).y, -1.0f, 1.0f);
            float sunZenithSin = sqrt(max(1.0f - sunZenithCos * sunZenithCos, 0.0f));
            vec3  sunDir       = vec3(sunZenithSin, sunZenithCos, 0.0f);

            // Outer-space sun illuminance — the same pair every other physical pass reads.
            vec3 sunIlluminance = UnpackSkyConfig(s).sunColor * UnpackSunIntensity(s);

            vec3 direction = SkyDistantLightDirection(int(index));

            vec3 radiance = SkyDistantLightRadiance(atm, direction, sunDir, sunIlluminance,
                                                    SKY_DISTANT_LIGHT_ALTITUDE_KM,
                                                    SKY_DISTANT_LIGHT_SAMPLES);

            // The art-direction tint that belongs INSIDE every scattering integration (UE's
            // SkyAndAerialPerspectiveLuminanceFactor): the ambient a fog receives has to be the ambient
            // of the sky that is on screen, tint and all.
            vec3 tinted = radiance * UnpackSkyAndAerialPerspectiveLuminanceFactor(s);

            s_Radiance[index] = tinted;

            barrier();

            // Tree reduction, uniform control flow at every barrier (the loop bound is a constant and
            // the barrier sits outside the branch) — the one rule a groupshared sum must not break.
            for (uint stride = 32u; stride > 0u; stride = stride >> 1u)
            {
                if (index < stride)
                    s_Radiance[index] = s_Radiance[index] + s_Radiance[index + stride];
                barrier();
            }

            if (index == 0u)
            {
                vec3 mean = s_Radiance[0] / float(SKY_DISTANT_LIGHT_DIRECTIONS);

                imageStore(u_DistantSkyLight, ivec2(SKY_DISTANT_LIGHT_SPHERE_TEXEL, 0), vec4(mean, 1.0f));
            }
        }
    }
}
