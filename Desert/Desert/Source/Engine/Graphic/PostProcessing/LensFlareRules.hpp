#pragma once

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // How strongly the lens flare is added back in — the CPU half of the effect, in a header the tests
    // can compile without a renderer. The screen PLACEMENT of every feature is the GPU's half and lives
    // in Engine/Content/Shaders/Common/LensFlare.glslh, which the same test suite compiles as C++.
    //
    // This is one multiply, and it is a named function on purpose. "The flare paints when the sun is
    // behind the camera" is the way this effect goes wrong, and the only thing standing between the
    // engine and that symptom is that the sun's screen fade (SunScreen::Fade, 0 behind the camera — see
    // LightShaftRules.hpp) reaches the tonemap's intensity. Writing it inline at the one call site makes
    // it a line nobody tests; writing it here makes it a fact the suite pins.

    // @p sunFade is SunScreen::Fade, @p intensity the authored Core::SceneSettings::LensFlareIntensity.
    // Both are clamped at zero: a negative authored intensity must dim the flare to nothing, never
    // subtract light from the frame.
    inline float LensFlareStrength( float sunFade, float intensity )
    {
        return glm::max( sunFade, 0.0f ) * glm::max( intensity, 0.0f );
    }
} // namespace Desert::Graphic
