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
     * Engine/Content/Shaders/Common/SkyMedium.glslh, the exact text SkyTransmittanceLut.shader
     * compiles as GLSL, including the payload packing that feeds it. The SkyMedium tests assert that
     * this value and the LUT's own texel agree, which is only a meaningful assertion because the two
     * cannot be edited apart.
     */
    glm::vec3 SunTransmittanceAtGround( const SkySettings& sky, const glm::vec3& towardSun );
} // namespace Desert::Graphic
