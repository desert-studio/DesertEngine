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
     * Two terms, and the order of the conditions is the point:
     *   * the PLANET'S SHADOW, always. A sun below the horizon is behind the ground, and no sky model
     *     can make it shine up through the floor. It is SkyScattering.glslh's SkyPlanetShadow evaluated
     *     at the ground — the same terminator, with the same smooth band, that darkens the physical sky —
     *     so the light fades across the horizon instead of switching, and fades exactly where the sky
     *     says the sun has set. Without it SkyModel::ArtisticGradient lit a night scene from below at
     *     full strength (the sun at -34 degrees in GI_Bistro_Night).
     *   * the ATMOSPHERE'S TRANSMITTANCE (SunTransmittanceAtGround), only in SkyModel::PhysicalAtmosphere
     *     and only while the light opts in (UE's bAffectedByAtmosphereTransmittance). ArtisticGradient
     *     keeps the documented independence of sky radiance and surface illuminance ABOVE the horizon:
     *     there the factor is exactly (1,1,1).
     *
     * @return per-channel factor in [0, 1]; exactly zero once the sun is past the terminator band.
     */
    glm::vec3 SunLightFactorAtGround( const SkySettings& sky, const glm::vec3& towardSun,
                                      bool affectedByAtmosphereTransmittance );
} // namespace Desert::Graphic
