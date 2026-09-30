#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Desert::Core
{
    // ─── THE ENGINE'S DEPTH CONVENTION: REVERSED-Z, ZERO-TO-ONE ─────────────────────────────────────
    //
    // Every projection the scene is rendered through is built HERE, so the convention is one decision in
    // one file instead of an assumption spread over a camera, a frustum, a render pass and five shaders.
    //
    // The stored depth is 1 AT THE NEAR PLANE and 0 AT THE FAR PLANE, the depth attachment clears to 0,
    // and the depth test asks whether a fragment is GREATER than what is already there.
    //
    // WHY, and it is not a preference. A perspective projection distributes depth as 1/z: at a far plane
    // of 50 km with a 10 cm near plane, HALF of the [0,1] range is spent on the first 20 cm in front of
    // the lens. Standard-Z then stores that range in a float whose own precision is ALSO densest near
    // zero — the two densities pile up in the same place, and everything past a few hundred metres shares
    // a handful of representable values. That is what makes a 10 km surface shimmer.
    //
    // Reversing it puts the far plane at 0, where float32 has its exponent range, against the 1/z curve
    // that is coarsest there. The two distributions cancel almost exactly, and the relative depth error
    // becomes near-constant across the whole range. A 50 km far plane then costs nothing, which is the
    // whole reason this exists: the aerial-perspective volume already reaches 96 km, and no scene could
    // show more than a kilometre of it.
    //
    // THE FLOAT MATTERS. Reversed-Z on a UNORM24 attachment buys NOTHING — an integer buffer quantizes
    // uniformly in NDC, so flipping it just relabels the same 2^24 levels. The scene depth attachment is
    // DEPTH32F for exactly this reason (SceneRenderer::Init).
    //
    // WHAT DOES NOT FLIP: the shadow cascades. Their projection is ORTHOGRAPHIC, so their depth is linear
    // in light-space distance and there is no 1/z curve for the float exponent to cancel — reversing them
    // would be cost and risk for a precision gain of exactly zero. They stay standard-Z and say so at
    // ShadowCascades.hpp and MeshRenderer::SetupShadowPass, and their render pass carries its own clear.
    //
    // GLM is NOT configured with GLM_FORCE_DEPTH_ZERO_TO_ONE, so `glm::perspective`/`glm::ortho` produce
    // OpenGL's [-1,1] clip range. Vulkan clips z to [0,w] and discards the rest, which is why these
    // helpers name the _ZO variants explicitly rather than relying on a global define that a single
    // translation unit could fail to see.

    // The value a depth attachment clears to under this convention: the FAR plane.
    inline constexpr float kDepthClear = 0.0f;

    // The device depth of a fragment sitting exactly on the near plane.
    inline constexpr float kDepthNear = 1.0f;

    // ─── DEFAULT CLIP PLANES ────────────────────────────────────────────────────────────────────────
    //
    // Shared by every camera in the engine (Core::Camera, ECS::CameraData), in world units — 1 unit is
    // a centimetre. They live here rather than on the camera because the far plane is a consequence of
    // the convention above, not an independent taste.
    //
    // THE FAR PLANE IS 50 km, AND IT IS THE REVERSED-Z DIVIDEND. It was 1 km, not because a kilometre
    // was ever the right view distance but because standard-Z could not afford more: past a few hundred
    // metres every surface collapsed onto the same handful of depth values and z-fighting set in.
    //
    // 50 km is chosen, not maximal, and three things agree on it:
    //   * it is roughly the distance to the geometric horizon from 200 m up — the altitude a flying
    //     editor camera actually reaches — so ground can be drawn to a real horizon and not to a ring;
    //   * the atmosphere's aerial-perspective froxel volume is 96 km deep and its useful range ends
    //     inside this, so opaque geometry and the air in front of it finally span the same distances;
    //   * it stays well clear of the point where even reversed-Z thins out. The relative depth error is
    //     about (far/near) * 2^-23 of a distance; at 50 km over a 10 cm near plane that is sub-millimetre
    //     everywhere, and the near plane — not the far one — is what would have to move to spend it.
    // Anything beyond — the planet shell, the sky — is drawn by passes that never consult
    // this plane at all.
    inline constexpr float kDefaultNearPlane = 10.0f;      // 10 cm
    inline constexpr float kDefaultFarPlane  = 5000000.0f; // 50 km

    // Reversed-Z perspective projection. Near and far are handed to GLM SWAPPED, which is what produces
    // the reversal: the matrix that maps `far` to 0 and `near` to 1 is literally the standard matrix with
    // its two plane distances exchanged, so there is no bespoke algebra here to get wrong.
    [[nodiscard]] inline glm::mat4 MakePerspective( float fovYRadians, float aspect, float nearPlane,
                                                    float farPlane )
    {
        return glm::perspectiveRH_ZO( fovYRadians, aspect, farPlane, nearPlane );
    }

    inline constexpr float kEditorViewportFovXDegrees = 90.0f;

    [[nodiscard]] inline float VerticalFovKeepingHorizontal( float fovXRadians, float aspect )
    {
        return 2.0f * glm::atan( glm::tan( fovXRadians * 0.5f ) / aspect );
    }

    // Reversed-Z orthographic projection, by the same swap. Reversing an ortho projection gains no
    // precision (its depth is linear), but the VIEWPORT camera must agree with the depth test and the
    // clear value that the rest of the frame uses, and those are not per-projection-type.
    [[nodiscard]] inline glm::mat4 MakeOrthographic( float left, float right, float bottom, float top,
                                                     float nearPlane, float farPlane )
    {
        return glm::orthoRH_ZO( left, right, bottom, top, farPlane, nearPlane );
    }
} // namespace Desert::Core
