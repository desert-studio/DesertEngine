// DesertAsset {"Kind":"Shader","Guid":"b8a2fad4e5fc46376996f0ff5b40ffd1","Versions":{"SHDR":1},"Dependencies":[]}
Shader "SkyMultiScatterLut"
{
    Compute
    {
        // The multiple-scattering LUT of the physical atmosphere (Hillaire 2020, section 5.3): for every
        // (sun zenith angle, view height) pair, Psi_ms — the luminance a point receives from light that
        // has scattered through the air MORE than once, per unit sun illuminance. 32x32 RGBA16F,
        // re-dispatched with the transmittance LUT it reads, only when the atmosphere block changes.
        //
        // The paper's construction, faithfully:
        //   * 64 directions on a uniform sphere (the 8x8 grid of SkyUniformSphereDirection);
        //   * each direction marches 20 steps with the UNIFORM phase, accumulating
        //       L      — second-order in-scattered luminance (sun transmittance from the LUT, planet
        //                shadow analytic, plus the ground-albedo bounce where the ray lands), and
        //       f_ms   — the scattering transfer of the same path (no phase, no sun: how much of an
        //                isotropic radiance field the path would re-scatter);
        //   * orders three and up form a geometric series because each further bounce applies the same
        //     transfer, so the infinite sum is L * 1 / (1 - f_ms) — that closed form is the whole reason
        //     this LUT can be 32x32 and still hold every scattering order.
        //
        // The per-step integral is Frostbite's analytic form (S - S*e^(-sigma*dt)) / sigma, which stays
        // exact however coarse the 20 steps are against the extinction.

        #include <Common/Atmosphere.glslh>
        #include <Common/SkyMedium.glslh>

        layout(binding = 0, rgba16f) restrict writeonly uniform image2D u_MultiScatterLut;

        // The SAME sky parameter buffer every other sky consumer reads (Graphic::kSkyPayloadBinding).
        ReadBuffer(1) SkyBuffer
        {
            vec4 u_SkyPacked[SKY_PACKED_VEC4_COUNT];
        };

        // Written by SkyTransmittanceLut in the same dispatch batch, ALWAYS before this pass runs —
        // SkyboxRenderer orders the two. Binding = Graphic::kSkyTransmittanceLutBinding.
        Uniform(2) sampler2D u_TransmittanceLut;

        const int   kSqrtDirectionCount = 8;  // 8x8 = the paper's 64 uniform sphere directions
        const int   kMarchSampleCount   = 20; // the reference implementation's fixed count
        const float kExtinctionFloor    = 1e-6f;

        vec3 SampleSunTransmittance(SkyAtmParams atm, float radiusKm, float sunZenithCos)
        {
            vec2 uv = SkyTransmittanceLutUvFromParams(atm.BottomRadiusKm, atm.TopRadiusKm, radiusKm,
                                                      sunZenithCos);
            return texture(u_TransmittanceLut, uv).rgb;
        }

        LocalSize(8, 8, 1);
        void main()
        {
            ivec2 size  = imageSize(u_MultiScatterLut);
            ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
            if (coord.x >= size.x || coord.y >= size.y)
                return;

            SkyPacked s;
            for (int i = 0; i < SKY_PACKED_VEC4_COUNT; ++i)
                s.v[i] = u_SkyPacked[i];

            SkyAtmParams atm = SkyMakeAtmParams(UnpackMediumRayleigh(s), UnpackMediumMie(s),
                                                UnpackMediumMieAbsorption(s), UnpackMediumOzone(s),
                                                UnpackMediumGround(s), UnpackMediumTentPlanet(s));

            // Texel-centre remap so the first/last texel hold exactly sunZenithCos = -1/+1 and the
            // surface/top altitudes; the future reader applies SkyUnitToTexelUv, the exact inverse.
            vec2 unit;
            unit.x = SkyTexelUvToUnit((float(coord.x) + 0.5f) / float(size.x), float(size.x));
            unit.y = SkyTexelUvToUnit((float(coord.y) + 0.5f) / float(size.y), float(size.y));

            SkyMultiScatterLutCoord c =
                 SkyMultiScatterParamsFromUnit(atm.BottomRadiusKm, atm.TopRadiusKm, unit.x, unit.y);

            // Planet-centred frame, zenith = +Y. The atmosphere is spherically symmetric, so the sun can
            // live in the XY plane without loss of generality.
            vec3 samplePosition = vec3(0.0f, c.ViewHeightKm, 0.0f);
            float sunSin        = sqrt(max(1.0f - c.SunZenithCos * c.SunZenithCos, 0.0f));
            vec3 sunDirection   = vec3(sunSin, c.SunZenithCos, 0.0f);

            float uniformPhase = SkyPhaseUniform();

            vec3 luminanceSum = vec3(0.0f, 0.0f, 0.0f); // second-order L, summed over the sphere
            vec3 transferSum  = vec3(0.0f, 0.0f, 0.0f); // f_ms, summed over the sphere

            for (int dirIndex = 0; dirIndex < kSqrtDirectionCount * kSqrtDirectionCount; ++dirIndex)
            {
                float u = (float(dirIndex / kSqrtDirectionCount) + 0.5f) / float(kSqrtDirectionCount);
                float v = (float(dirIndex % kSqrtDirectionCount) + 0.5f) / float(kSqrtDirectionCount);
                vec3  rayDir = SkyUniformSphereDirection(u, v);

                float rayMu       = rayDir.y; // dot(rayDir, zenith) at the start point
                bool  hitsGround  = SkyIntersectsGround(c.ViewHeightKm, rayMu, atm.BottomRadiusKm);
                float tMax        = hitsGround
                                         ? SkyDistanceToBottom(c.ViewHeightKm, rayMu, atm.BottomRadiusKm)
                                         : SkyDistanceToTop(c.ViewHeightKm, rayMu, atm.TopRadiusKm);
                float dt = tMax / float(kMarchSampleCount);

                vec3 luminance  = vec3(0.0f, 0.0f, 0.0f);
                vec3 transfer   = vec3(0.0f, 0.0f, 0.0f);
                vec3 throughput = vec3(1.0f, 1.0f, 1.0f);

                for (int step = 0; step < kMarchSampleCount; ++step)
                {
                    float t = (float(step) + 0.5f) * dt;

                    vec3  position = samplePosition + rayDir * t;
                    float radius   = max(length(position), atm.BottomRadiusKm);
                    vec3  zenith   = position / radius;

                    SkyMediumSample medium = SkySampleMedium(atm, radius - atm.BottomRadiusKm);
                    vec3 stepTransmittance = exp(-medium.Extinction * dt);
                    vec3 extinction        = max(medium.Extinction, vec3(kExtinctionFloor));

                    float sunZenithHere = dot(zenith, sunDirection);
                    // The planet blocks the sun analytically — the LUT knows nothing of geometry, only
                    // of the sphere it sits on.
                    float planetShadow =
                         SkyIntersectsGround(radius, sunZenithHere, atm.BottomRadiusKm) ? 0.0f : 1.0f;

                    vec3 sunTransmittance = SampleSunTransmittance(atm, radius, sunZenithHere);

                    // Sun illuminance is 1: the LUT stores a transfer, and the consumer multiplies the
                    // real illuminance back in.
                    vec3 scatteredSource = planetShadow * sunTransmittance * medium.Scattering *
                                           uniformPhase;

                    // Analytic per-step integration of source * transmittance over the segment.
                    luminance += throughput * (scatteredSource - scatteredSource * stepTransmittance) /
                                 extinction;
                    // The same integral for the multi-scattering transfer: the scattering coefficient
                    // itself, no phase and no sun — an isotropic field re-scattered by this path.
                    transfer += throughput * (medium.Scattering - medium.Scattering * stepTransmittance) /
                                extinction;

                    throughput = throughput * stepTransmittance;
                }

                if (hitsGround)
                {
                    // The bounce off the planet: Lambertian ground lit by the transmitted sun.
                    vec3  groundPosition = samplePosition + rayDir * tMax;
                    float groundRadius   = max(length(groundPosition), atm.BottomRadiusKm);
                    vec3  groundZenith   = groundPosition / groundRadius;
                    float sunZenithGround = dot(groundZenith, sunDirection);
                    float NdotL           = clamp(sunZenithGround, 0.0f, 1.0f);

                    vec3 sunTransmittance = SampleSunTransmittance(atm, groundRadius, sunZenithGround);
                    luminance += throughput * sunTransmittance * NdotL * atm.GroundAlbedo / SKY_PI;
                }

                luminanceSum += luminance;
                transferSum += transfer;
            }

            // Sum over the sphere: each of the 64 directions carries 4pi/64 steradians, and the receiving
            // point re-scatters isotropically (1/4pi) — the 4pi cancels, leaving the plain average.
            float directionCount = float(kSqrtDirectionCount * kSqrtDirectionCount);
            vec3  inScattered    = luminanceSum / directionCount;
            vec3  transferAvg    = transferSum / directionCount;

            // The geometric series over all higher orders. transferAvg < 1 physically: a path cannot
            // re-scatter more energy than enters it, so the series converges; the min is a guard against
            // a half-precision texel ever storing infinity, not part of the model.
            vec3 seriesSum = vec3(1.0f, 1.0f, 1.0f) / max(vec3(1.0f, 1.0f, 1.0f) - transferAvg,
                                                          vec3(1e-4f, 1e-4f, 1e-4f));

            vec3 psiMs = inScattered * seriesSum * atm.MultiScatteringFactor;

            imageStore(u_MultiScatterLut, coord, vec4(psiMs, 1.0f));
        }
    }
}
