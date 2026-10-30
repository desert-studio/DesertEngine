#pragma once

#include <Engine/ECS/VolumetricCloudComponent.hpp>

#include <Common/Settings/MachineSettings.hpp>

#include <algorithm>
#include <cstdint>

namespace Desert::Graphic
{
    /**
     * WHAT A CLOUD QUALITY TIER IS ALLOWED TO CHANGE, as three numbers and not as six.
     *
     * THE SHORT VERSION, because the long one below is a list of refusals: this subsystem has exactly
     * three costs a cheaper setting can buy back without breaking something that is asserted elsewhere —
     * the shadow ray a lit sample traces toward the sun, the map the layer casts onto the world, and the
     * transmittance at which the view march gives up. Everything else that looks like a quality knob is
     * pinned, and the pinning is a relation with a test behind it rather than an opinion.
     *
     * WHAT IS PINNED, AND BY WHAT. Measured on the shipped library; the numbers are in
     * Docs/Clouds/CALIBRATION.md section QT.
     *
     *   * MAX STEPS cannot fall. The march's SEARCH step is CLOUD_COARSE_STEP_MULTIPLIER x
     *     CLOUD_DISTANCE_TO_MAX_STEPS_KM / MaxSteps, so the finest chord it can be relied on to FIND is
     *     32 km / MaxSteps — 125 m at the component's 256. The two tightest shipped types (Altocumulus
     *     and Cirrus) place a median chord of 137 m, which is 1.10x that and no more. MaxSteps 234 is
     *     where the margin is gone; at 192 four of the nine shipped types are past Nyquist and at 128
     *     five of them are, the worst at 0.55x. A tier that lowers this number does not make the sky
     *     cheaper and softer, it makes four kinds of cloud dither in and out with the ray's jitter, and
     *     Desert/Tests/Engine/CloudType asserts exactly that and now asserts it FOR EVERY TIER.
     *
     *   * THE SHADOW MAP'S TEXEL cannot grow, because it is the same relation seen from the other side:
     *     Desert/Tests/Engine/CloudShadow requires the texel to sit inside the chord the march can find.
     *     At 512 texels over a 30 km radius the texel is 117 m against that 125 m. Dropping the
     *     resolution to 256 alone takes the texel to 234 m — 1.9x past the chord — and the only way to
     *     make 234 m legal is to raise the chord by lowering MaxSteps, which the paragraph above forbids.
     *     So the map's cost is bought back by shrinking WHAT IT COVERS instead of how finely it covers
     *     it: resolution, extent and snap are scaled by ONE number together, which leaves the texel
     *     exactly where it was and moves only the radius around the camera that receives cloud shadow at
     *     all. That is a limit the map already has and states (CLOUD_SHADOWMAP_EXTENT_KM); the tier moves
     *     it, it does not invent it.
     *
     *   * THE SHADOW MAP'S SAMPLE COUNT cannot fall either, and for the third face of the same relation:
     *     the count has to resolve the shipped congestus envelope, 3.6 km / 32 = 112 m against the same
     *     125 m chord. Sixteen — the count the shadow-map task priced at 2.75 ms — is 225 m, 1.8x past
     *     it, and its own measurement said so beside the price.
     *
     * WHAT IS NOT PINNED, AND IS THEREFORE THE WHOLE TIER:
     *
     *   * THE SHADOW RAY'S SAMPLE COUNT. Linear, 0.233 ms of frame time per sample on this machine, no
     *     relation attached to it at all, and the quality it buys is known to the per cent
     *     (VolumetricCloudData::LightMarchSamples carries the convergence table). It is the first thing a
     *     tier should reach for and it is the only per-sample knob that is free to move.
     *
     *   * THE SHADOW MAP'S FOOTPRINT, as described above.
     *
     *   * WHERE THE MARCH GIVES UP. Stop Transmittance 0.005 -> 0.05 is 2.83 ms — sixteen per cent — for
     *     a frame that differs from the default by at most 10 of 255 anywhere, which is the best cost per
     *     visible unit in the whole subsystem. The samples it drops are behind material that has already
     *     hidden them. Its ONE caveat is stated rather than smoothed over: unlike the two above, this
     *     saving is scene-dependent — a sky of thin cloud never reaches either threshold, so it costs
     *     nothing and saves nothing there.
     *
     * WHAT WAS MEASURED AND LEFT OUT. Multiple-scattering octaves: the octave loop re-uses ONE shadow
     * march (Programs/Clouds/CloudRaymarch.shader, the comment above the loop), so three octaves cost
     * 0.32 ms more than one — under two per cent, and inside the run-to-run spread of that row — while
     * turning the clouds grey. A knob that changes the picture and not the cost is the wrong half of a
     * tier. The shadow ray's DISTANCE: shortening it moves no cost at all, because the count is what is
     * marched and the distance only decides how far apart the samples are — it is a way to make the
     * clouds flat for free rather than fast.
     */
    struct CloudQualityScale
    {
        /**
         * The ceiling this tier puts on VolumetricCloudData::LightMarchSamples.
         *
         * A CEILING AND NOT A VALUE, which is the whole of how the tier avoids being a second source of
         * truth for a field the component already owns: the artist authors what the sky needs, the tier
         * says what the machine can afford, and the GPU gets min(). An artist who authors 16 gets 16 on
         * every tier, because they have already asked for the cheap answer and a tier that raised it
         * would be overriding an authored intention rather than protecting a frame budget. The identity
         * value is ECS::kCloudLightMarchMaxSamples, which caps nothing the component does not already.
         */
        int32_t LightMarchSampleCeiling = ECS::kCloudLightMarchMaxSamples;

        /**
         * The ceiling this tier puts on VolumetricCloudData::LightMarchLocalSamples — the SHORT shadow ray
         * that runs where the cloud shadow map supplies the far optical depth (Programs/Clouds/
         * CloudRaymarch.shader, CloudSelfShadowOpticalDepth). Composed by min() on exactly the terms of
         * LightMarchSampleCeiling above, and for the same reason; the identity is
         * ECS::kCloudLightMarchLocalMaxSamples.
         */
        int32_t LightMarchLocalSampleCeiling = ECS::kCloudLightMarchLocalMaxSamples;

        /**
         * The linear scale applied to the shadow map's resolution, extent and snap TOGETHER.
         *
         * ONE NUMBER FOR THREE, and that is the point rather than a shorthand. The texel is
         * 2 x extent / resolution, so scaling both by the same factor leaves it — and therefore the
         * relation against the march's resolvable chord, which is the thing that must not move — exactly
         * where the shipped tier put it. The snap travels with them because the coverage guaranteed
         * around the camera is extent - snap/2, and a snap that did not scale would eat the smaller
         * extent from the inside. Three numbers scaled independently is three ways to break one relation;
         * this way there is no way to break it, and Desert/Tests/Engine/CloudShadow asserts that over
         * every tier rather than over the default.
         */
        float ShadowMapScale = 1.0f;

        /**
         * The FLOOR this tier puts under VolumetricCloudData::StopTransmittance — how much light may
         * still be getting through when the march gives up.
         *
         * A floor rather than a ceiling for the same reason LightMarchSampleCeiling is a ceiling: both
         * are the direction that makes the frame CHEAPER, so the tier composes with the artist's number
         * by max() here and min() there, and neither can make a scene more expensive than it was
         * authored to be.
         *
         * THE IDENTITY IS ZERO and not the component's current default, deliberately: a floor of zero
         * floors nothing whatever the component's default becomes, where a copy of that default is a
         * mirror that drifts the day somebody retunes it.
         */
        float StopTransmittanceFloor = 0.0f;
    };

    /**
     * The tier's numbers. PURE, so Desert/Tests/Engine/CloudShadow drives it directly and asserts the
     * relations tier by tier instead of on the one set of constants that happens to be compiled in.
     *
     * An unknown enumerator answers High rather than a guess: High is the calibrated reference, so a
     * scene whose tier failed to deserialize renders correctly and slowly, which is the failure a person
     * notices and reports rather than one they ship.
     */
    inline CloudQualityScale CloudQualityFor( Common::Settings::CloudQuality tier )
    {
        CloudQualityScale scale;
        switch ( tier )
        {
            case Common::Settings::CloudQuality::Low:
                // Sixteen is where the shadow ray's price list stops being worth paying rather than an
                // arbitrary halving: it is 3.73 ms below the default here, and the frame it produces is
                // 34% hot in the sunward highlights against the converged answer — a difference a person
                // sees, which is the test a tier has to pass in the other direction.
                scale.LightMarchSampleCeiling = 16;
                // Four: the first segment of the 1 km short ray is 62.5 m, half a shadow-map texel —
                // still inside the map's resolution, so the near field and the far term still meet.
                scale.LightMarchLocalSampleCeiling = 4;
                scale.ShadowMapScale          = 0.5f;
                // Ten times the default, and the number is where the measurement stops being free: it
                // saves 2.83 ms while moving no pixel of the shipped demo by more than 10 of 255.
                scale.StopTransmittanceFloor = 0.05f;
                break;

            case Common::Settings::CloudQuality::Medium:
                scale.LightMarchSampleCeiling = ECS::kCloudLightMarchMaxSamples;
                // Six: 27.8 m first segment, between Low's half texel and the authored default's 15.6 m.
                scale.LightMarchLocalSampleCeiling = 6;
                scale.ShadowMapScale          = 0.5f;
                scale.StopTransmittanceFloor  = 0.0f;
                break;

            case Common::Settings::CloudQuality::High:
            default:
                scale.LightMarchSampleCeiling      = ECS::kCloudLightMarchMaxSamples;
                scale.LightMarchLocalSampleCeiling = ECS::kCloudLightMarchLocalMaxSamples;
                scale.ShadowMapScale               = 1.0f;
                scale.StopTransmittanceFloor  = 0.0f;
                break;
        }
        return scale;
    }
} // namespace Desert::Graphic
