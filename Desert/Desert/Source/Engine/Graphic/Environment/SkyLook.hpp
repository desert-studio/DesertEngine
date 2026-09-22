#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace Desert::Graphic
{
    /**
     * @brief THE AUTHORED LOOK OF AN HDR SKY — the three knobs that turn one `.hdr` file into one
     *        scene's sky, carried as ONE value from the component to the bake.
     *
     * WHY A STRUCT RATHER THAN THREE ARGUMENTS. Every one of these has to reach four places that must
     * agree — the visible background, the diffuse irradiance, the GGX-prefiltered specular, and the
     * fingerprint that decides whether any of them needs rebuilding. Three loose floats is three chances
     * for a new knob to be threaded into three of the four; a struct with a fingerprint on it means
     * adding a field to the struct is the whole change, and a field that is not in Fingerprint() is a
     * field whose edit does nothing — which is a mistake a reader can SEE here.
     *
     * NOT SEPARATELY APPLIED TO THE BACKGROUND. The obvious cheap spelling is to multiply the intensity
     * into the skybox fragment shader and leave the cubes alone, and that is what this engine shipped:
     * `SkyboxComponent::Intensity` reached the fullscreen sky pass and NOTHING else, so a sky authored
     * at 5x lit every surface in the world at 1x. Applying the look once, where the panorama becomes a
     * cube, is what makes that state unreachable — the background samples the radiance cube, so it
     * cannot disagree with the irradiance cube built beside it.
     */
    struct SkyLook
    {
        /// Turn the sky about the world's up axis, in DEGREES. Answers "the sun baked into this
        /// panorama is not where my directional light is" without re-authoring the file.
        float RotationDegrees = 0.0f;

        /// Linear multiplier on the sky's radiance. The one knob that already existed, now reaching the
        /// lighting as well as the backdrop.
        float Intensity = 1.0f;

        /// Linear colour the sky is graded through. White is the file as authored.
        glm::vec3 Tint = glm::vec3( 1.0f );

        /// (cos, sin) of the rotation, evaluated once on the CPU: the shaders need it per texel and a
        /// 1024-texel face is six million transcendentals per bake otherwise.
        [[nodiscard]] glm::vec2 YawCosSin() const
        {
            const float yaw = glm::radians( RotationDegrees );
            return glm::vec2( std::cos( yaw ), std::sin( yaw ) );
        }

        /// What the shader multiplies radiance by. Tint and intensity are ONE number on the GPU and two
        /// knobs in the editor on purpose — the shader has no use for the distinction and a second
        /// uniform is a second thing to forget to bind.
        [[nodiscard]] glm::vec3 Gain() const
        {
            return Tint * Intensity;
        }

        /// Is this the sky the cubes were baked from? Compared as an exact bit pattern rather than with
        /// a tolerance: the question is "did an author change a value", not "are these skies similar",
        /// and a tolerance here would make a small deliberate nudge do nothing at all.
        [[nodiscard]] bool operator==( const SkyLook& ) const = default;
    };
} // namespace Desert::Graphic
