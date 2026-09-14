#pragma once

#include <glm/glm.hpp>

namespace Desert::Graphic
{
    // Per-frame, evaluated state of the scene's SHARED wind (authored in SceneSettings::Wind*). This is the
    // runtime form the renderers consume via SceneRenderer::GetWind(). NOTHING consumes it today - its
    // one reader was the procedural grass generator, removed by Г25; see the note over GetWind(). Hair + cloth
    // next. Keeping it here (not on the Skybox) is the whole point — one wind moves the world.
    struct WindEnv
    {
        glm::vec2 Direction{ 1.0f, 0.0f }; // normalized heading on the ground (XZ) plane
        float     Strength   = 0.15f;      // base force / foliage sway amplitude
        float     Turbulence = 1.0f;       // gustiness (foliage/hair/cloth response)
        float     Time       = 0.0f;       // monotonically increasing seconds (drives the sway animation)
    };
} // namespace Desert::Graphic
