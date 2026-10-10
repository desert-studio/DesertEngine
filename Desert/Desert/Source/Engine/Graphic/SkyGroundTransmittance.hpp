#pragma once

#include <Engine/Graphic/SkySettings.hpp>

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    /**
     * What survives the trip from the ground to the top of the atmosphere along the sun's direction —
     * UE's FAtmosphereSetup::GetTransmittanceAtGroundLevel, the quantity PrepareSunLightProxy multiplies
     * the directional light's colour by so that a sunset reddens the light falling on geometry by the
     * same law that reddens the sky behind it.
     *
     * @param towardSun direction TOWARD the sun, normalized (the engine's one negation already happened
     *                  in ECS::Rules::AtmosphereSunDirection). Only its zenith component matters: the
     *                  atmosphere is spherically symmetric, so the transmittance depends on the sun's
     *                  elevation alone.
     * @return per-channel transmittance in (0, 1]. Essentially zero for a sun below the horizon — there
     *         is no sunlight at night — and exactly (1,1,1) for a caller that never asks.
     *
     * ON THE CPU, and BEFORE the frame is recorded, because its consumer is the light's colour in the
     * lights uniform block: a GPU-side value would arrive a frame late and would have to be read back to
     * be multiplied into a buffer the CPU packs.
     *
     * ONE IMPLEMENTATION. This is not a C++ port of the transmittance LUT's march — the .cpp compiles
     * Editor/Resources/Shaders/Common/SkyMedium.glslh, the exact text SkyTransmittanceLut.shader
     * compiles as GLSL, including the payload packing that feeds it. The SkyMedium tests assert that
     * this value and the LUT's own texel agree, which is only a meaningful assertion because the two
     * cannot be edited apart.
     */
    glm::vec3 SunTransmittanceAtGround( const SkySettings& sky, const glm::vec3& towardSun );

    /**
     * THE factor the directional (atmosphere) sun's colour is multiplied by before it lights geometry —
     * the one place that decides how much of the authored sun reaches the ground, in BOTH sky models.
     *
     * Two terms in both models — the PLANET'S SHADOW and the ATMOSPHERE'S TRANSMITTANCE — each model
     * evaluating them the way its sky does:
     *   * SkyModel::PhysicalAtmosphere: SkyScattering.glslh's SkyPlanetShadow at the ground (the same
     *     terminator, with the same smooth band, that darkens the physical sky) x the transmittance LUT's
     *     own value (SunTransmittanceAtGround) while the light opts in (UE's
     *     bAffectedByAtmosphereTransmittance).
     *   * SkyModel::ArtisticGradient (no LUTs): the horizon cutting the SOLAR DISK — the visible area
     *     fraction of a disk of SkySettings::SunAngularRadius, so the light fades over the disk's
     *     angular diameter and is exactly zero once the top limb has set — x, while the light opts in,
     *     an analytic transmittance: the zenith column of the SAME medium coefficients raised to the
     *     Kasten-Young relative air mass, per RGB channel, so the low sun dims and reddens as in the
     *     physical model (within ~2 % above 20 degrees, ~7 % at 5 degrees).
     * Without the shadow ArtisticGradient lit a night scene from below at full strength (the sun at
     * -34 degrees in GI_Bistro_Night); without the transmittance it switched off at the horizon at noon
     * brightness and colour.
     *
     * @return per-channel factor in [0, 1]; exactly zero once the sun is past the terminator
     *         band (physical) or the disk has fully set (artistic).
     */
    glm::vec3 SunLightFactorAtGround( const SkySettings& sky, const glm::vec3& towardSun,
                                      bool affectedByAtmosphereTransmittance );
} // namespace Desert::Graphic
