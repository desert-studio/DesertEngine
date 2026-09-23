#pragma once

// ── HOW FAR THE EDITOR CAMERA MAY LOOK UP OR DOWN, AND WHY IT IS NOT 90° ───────────────────────────
//
// `glm::lookAt` builds its basis by crossing the forward direction with the up vector, so when the two
// are parallel the cross is zero and the matrix collapses every vertex onto one clip position. The
// editor camera's up vector is world up, so looking straight down is exactly that degenerate case, and
// `EditorCamera::OnUpdate` clamps pitch to this limit on EVERY frame to keep out of it.
//
// IT LIVES IN ITS OWN HEADER BECAUSE A SECOND PLACE NEEDS IT AND MUST NOT GUESS IT. The viewport's
// named camera angles (Editor/Panels/ViewportPanel/ViewportCameraPreset.hpp) ask for Top and Bottom —
// pitch ±90° — and the clamp hands back ±89°, so a camera sitting on the Top preset is one degree off
// the direction it names. Anything deciding "is this camera still on Top" has to admit that degree, and
// while the number was a literal inside OnUpdate, the only way to admit it was to copy it: two literals
// that agree today, which is exactly the shape that stops agreeing on the day somebody widens one.
//
// This header pulls in nothing, deliberately: Camera.hpp reaches Application.hpp and most of the
// engine, and the things that need only this constant (a preset table, a test) must not pay for that.

#include <glm/trigonometric.hpp>

namespace Desert::Core
{
    // Maximum |pitch| the editor camera will hold, in radians. One degree short of straight down.
    inline constexpr float kMaxCameraPitch = glm::radians( 89.0f );

    // The same limit as the ANGLE BY WHICH a request for straight up/down misses, in degrees. Callers
    // that must recognise a clamped camera size their tolerance from this rather than from a literal.
    inline constexpr float kCameraPitchClampMissDegrees = 90.0f - 89.0f;
} // namespace Desert::Core
