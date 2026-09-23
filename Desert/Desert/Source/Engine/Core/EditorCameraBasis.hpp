#pragma once

// ── TWO WAYS TO POINT A CAMERA, AND WHY ONE OF THEM CANNOT EXPRESS A PLAN VIEW ─────────────────────
//
// The editor camera ORBITS: it carries a yaw and a pitch, world up is its up vector, and every frame
// `EditorCamera::OnUpdate` clamps the pitch to ±89° (CameraPitchLimit.hpp) because `glm::lookAt`
// collapses when the forward direction is parallel to the up vector. That model is right for a camera
// the user drags around, and it is the one the whole editor flies with.
//
// IT CANNOT HOLD A TOP VIEW. Straight down IS the degenerate case, so a camera asked for Top settles a
// full degree off and stays there — and a plan view one degree off is not a plan: verticals lean, two
// faces of a box are visible where one should be, and a measurement read off the picture is wrong.
// Widening the clamp does not fix it, it moves it: at exactly ±90° the up vector has to become
// something else, and choosing that something inside the orbit changes orbiting in every viewport in
// the editor for the sake of two views that do not orbit at all.
//
// SO AN AXIS VIEW IS NOT AN ORBIT ANGLE, IT IS A BASIS. UE's answer is the same shape: an orthographic
// axis viewport and a perspective orbit viewport are not one camera with one model. Here they stay one
// CLASS — the camera still owns one position, one focal point, one projection — but the ORIENTATION has
// two spellings, and `AxisViewBasisOf` is the one the orbit cannot reach. An axis view names its up
// vector explicitly, per axis, so ±90° is an ordinary value with nothing degenerate about it.
//
// EVERYTHING HERE IS PURE: glm and nothing else, no camera, no window, no Input singleton. That is the
// point. `Camera.cpp` is compiled by no test suite (scripts/CI/UnreachedSources.sh), so while this
// arithmetic lived inside `EditorCamera` the only way to ask it a question was to build a device and an
// event loop — and for as long as that was true the one relation that mattered, "what a preset holds is
// not what the clamp hands back", was asserted by nothing. The camera CALLS these; they are not a
// second copy of it.

#include <Engine/Core/CameraPitchLimit.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <optional>

namespace Desert::Core
{
    // An orientation as the view matrix wants it: where the camera looks, and which way is up on screen.
    // Both unit length and perpendicular for every value this header produces — which is exactly the
    // property `glm::lookAt` needs and the orbit model cannot promise at the poles.
    struct ViewBasis
    {
        glm::vec3 Forward{ 0.0f, 0.0f, -1.0f };
        glm::vec3 Up{ 0.0f, 1.0f, 0.0f };
    };

    // ── THE ORBIT MODEL ───────────────────────────────────────────────────────────────────────────
    //
    // `EditorCamera`'s yaw/pitch, spelled here so the camera and the suites read one definition. The
    // quaternion is built from (-pitch, -yaw, 0) because that is the convention the fly camera's mouse
    // deltas were written against, and changing it would change the direction every drag turns.

    [[nodiscard]] inline glm::quat OrbitOrientation( float yaw, float pitch )
    {
        return glm::quat( glm::vec3( -pitch, -yaw, 0.0f ) );
    }

    [[nodiscard]] inline glm::vec3 OrbitForward( float yaw, float pitch )
    {
        // `q * v` and not `glm::rotate( q, v )`: the two are the same expression (glm/gtx/quaternion.inl
        // defines the second as the first), but the second lives behind GLM_ENABLE_EXPERIMENTAL and a
        // header must not switch that on for every translation unit that includes it.
        return OrbitOrientation( yaw, pitch ) * glm::vec3( 0.0f, 0.0f, -1.0f );
    }

    [[nodiscard]] inline glm::vec3 OrbitUp( float yaw, float pitch )
    {
        return OrbitOrientation( yaw, pitch ) * glm::vec3( 0.0f, 1.0f, 0.0f );
    }

    [[nodiscard]] inline glm::vec3 OrbitRight( float yaw, float pitch )
    {
        return OrbitOrientation( yaw, pitch ) * glm::vec3( 1.0f, 0.0f, 0.0f );
    }

    // THE CLAMP, AND IT IS THE ONLY COPY OF IT. `EditorCamera::OnUpdate` calls this; nothing else in the
    // engine writes the pitch. It is deliberately NOT widened — see the head of this file.
    [[nodiscard]] inline float ClampOrbitPitch( float pitch )
    {
        return glm::clamp( pitch, -kMaxCameraPitch, kMaxCameraPitch );
    }

    // Invert the forward model f = ( sin(yaw)cos(pitch), -sin(pitch), -cos(yaw)cos(pitch) ). Unclamped:
    // the caller decides whether the answer has to survive the orbit, and `SnapToDirection` does exactly
    // that one line later. @p forward must be normalized.
    inline void OrbitAnglesFor( const glm::vec3& forward, float& yaw, float& pitch )
    {
        pitch = glm::asin( glm::clamp( -forward.y, -1.0f, 1.0f ) );
        yaw   = std::atan2( forward.x, -forward.z );
    }

    // WHAT THE ORBIT MODEL ACTUALLY HOLDS WHEN IT IS ASKED FOR @p forward: the angles, THE CLAMP, and
    // back to a direction. Exists so the difference between the two spellings is a value a test can
    // compare rather than a sentence in a comment — for the four horizontal axes it returns @p forward
    // unchanged, and for straight up/down it returns the one-degree miss that started all of this.
    [[nodiscard]] inline glm::vec3 OrbitForwardFor( const glm::vec3& forward )
    {
        float yaw   = 0.0f;
        float pitch = 0.0f;
        OrbitAnglesFor( glm::normalize( forward ), yaw, pitch );
        return OrbitForward( yaw, ClampOrbitPitch( pitch ) );
    }

    // ── THE AXIS VIEWS ────────────────────────────────────────────────────────────────────────────

    // How close to an axis a direction must be to BE that axis. This is not a tolerance on what the user
    // is looking at (the viewport's caption has its own, and a much tighter one now) — it is the guard on
    // a caller handing in a hand-typed or serialized -1.0000001, and it is one ULP-ish rather than a
    // fraction of a degree on purpose: an axis view is a thing you ENTER, never a thing you drift into.
    inline constexpr float kAxisViewEpsilon = 1e-4f;

    // THE SIX AXIS VIEWS, EACH WITH ITS OWN UP VECTOR, or nullopt when @p forward is not a world axis.
    //
    // World is Y-up, so the four horizontal views take world up and nothing interesting happens. Top and
    // Bottom are the whole reason this function exists: world up is PARALLEL to the view there and cannot
    // be the up vector, so the screen's up becomes -Z for Top and +Z for Bottom. That choice is not
    // arbitrary either — it puts +X to the right of the screen in BOTH of them (right = Forward x Up),
    // so flipping between the plan and the bottom view does not mirror the world under the cursor.
    [[nodiscard]] inline std::optional<ViewBasis> AxisViewBasisOf( const glm::vec3& forward )
    {
        if ( glm::length( forward ) < kAxisViewEpsilon )
            return std::nullopt; // an uninitialised or torn-down direction: refused, never guessed
        const glm::vec3 f = glm::normalize( forward );

        struct AxisRow
        {
            glm::vec3 Forward;
            glm::vec3 Up;
        };
        const AxisRow rows[] = {
             { { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } }, // Top:    looking down, -Z up the screen
             { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } },   // Bottom: looking up,   +Z up the screen
             { { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f, 0.0f } },  // Front
             { { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f } },   // Back
             { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },   // Left
             { { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },  // Right
        };

        for ( const AxisRow& row : rows )
        {
            // THE ROW'S OWN VECTOR IS RETURNED, NOT THE CALLER'S. A direction that arrived as
            // (0, -0.99999994, 0) must not become the basis: the whole promise of this path is that the
            // view is EXACT, and "near enough to recognise" is a different statement from "is".
            if ( glm::all( glm::epsilonEqual( f, row.Forward, kAxisViewEpsilon ) ) )
                return ViewBasis{ row.Forward, row.Up };
        }
        return std::nullopt;
    }
} // namespace Desert::Core
