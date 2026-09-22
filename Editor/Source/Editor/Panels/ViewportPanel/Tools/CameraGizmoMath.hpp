#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace Desert::Editor::Tools
{
    // ── THE CAMERA GIZMO'S SHAPE, AS A PURE FUNCTION ────────────────────────────────────────────────
    //
    // WHAT WAS WRONG, MEASURED ON SCREEN BEFORE THIS EXISTED. `RenderCameras` drew the frustum by
    // inverting a projection built with `gizmoFar = min(cam.Far, cam.Near + 2.5f)`. That literal was
    // written when a world unit was a METRE; a unit is a CENTIMETRE, so with the shipped defaults
    // (Near = 10 cm, Far = 50 km) the drawn volume was 10 cm to 12.5 cm — two and a half CENTIMETRES
    // deep behind a rectangle eight centimetres tall. Two things followed and the owner saw both:
    //
    //   * it does not read as a frustum. Depth 2.5 against width 18 is a flat CARD, and its near and
    //     far rectangles are within 25 % of each other, so the pyramid that says "this camera looks
    //     THAT way" is not in the picture at all;
    //   * it is detached. The near rectangle floats 10 cm in front of the camera and NOTHING was drawn
    //     between it and the camera's own position, so the shape and the icon are two unrelated marks.
    //     At a working distance of 6 m the whole thing collapses into a ten-pixel scribble beside the
    //     icon, which is what "the lines are broken" looks like.
    //
    // THE FIX IS NOT A BIGGER NUMBER. A frustum sized in world units is legible at exactly one viewing
    // distance; picking 2.5 or 250 only moves which one. The gizmo is an ANNOTATION, so it is sized the
    // way the icons beside it already are — to a constant share of the screen — and the function below
    // is where that decision lives, alone and testable.
    //
    // WHAT WE TOOK FROM UE AND WHAT WE DID NOT. Taken: the apex is the camera, the shape says which way
    // it looks and how wide, and an up marker says which way is up so roll is visible. Not taken: UE's
    // world-sized frustum drawn to a fixed distance (it is the thing that stops being legible when you
    // move), and its near rectangle. The near plane defaults to 10 cm; at any distance where this gizmo
    // is legible the near rectangle is smaller than the icon glyph drawn on top of it, so it is a line
    // that can never be seen. It is not drawn, and this paragraph is why rather than an omission.

    struct CameraFrustumGizmo
    {
        // The camera's own world position. The four edges START here — that is the half the old shape
        // was missing, and it is what ties the wireframe to the icon.
        glm::vec3 Apex{ 0.0f };

        // Bottom-left, bottom-right, top-right, top-left of the rectangle the edges reach, in world
        // space, wound so that consecutive entries share an edge.
        glm::vec3 FarCorners[4]{};

        // The roll marker: a triangle sitting on top of the far rectangle, apex along the camera's up
        // axis. Two cameras that differ only in roll are indistinguishable without it.
        glm::vec3 UpMarker[3]{};

        // How far down the camera's forward axis FarCorners sit, in world units.
        float Depth = 0.0f;
    };

    // The far rectangle's half-height as a fraction of the distance from the viewer to the camera.
    // 0.05 is ~2.9 degrees of half-angle: about 200 px tall in a 1570 px viewport at a 45-degree editor
    // FOV, which is large enough to read a direction from and small enough not to cover the subject.
    inline constexpr float kGizmoHalfExtentPerDistance = 0.05f;

    // The floor on Depth, in world units (= 1 cm). It exists for the degenerate case only: a viewer
    // standing ON the camera would otherwise get a zero-size shape whose edges are one point, and a
    // wireframe that vanishes when you fly into it reads as a bug rather than as a viewpoint.
    inline constexpr float kGizmoMinDepth = 1.0f;

    /**
     * @brief How far down the camera's forward axis the drawn rectangle sits.
     *
     * Chosen so the rectangle's HALF-HEIGHT is `kGizmoHalfExtentPerDistance * viewerDistance`
     * regardless of the distance and regardless of the camera's own field of view — the constant
     * apparent size the icons beside it already have. The field of view therefore shows as the
     * pyramid's SHARPNESS (a wide camera gets a short flaring one, a long lens a deep narrow one)
     * rather than as its size, which is the more informative of the two mappings and the bounded one.
     *
     * @param viewerDistance distance from the EDITOR camera to the camera entity, world units.
     * @param farPlane       the camera's own Far. The gizmo may not claim to see past it, so this is
     *                       the upper clamp — a camera with a 30 cm Far draws a 30 cm pyramid and the
     *                       constant-apparent-size property is deliberately given up there.
     * @param tanHalfFovY    tan of half the camera's vertical field of view.
     */
    [[nodiscard]] inline float GizmoFrustumDepth( float viewerDistance, float farPlane, float tanHalfFovY )
    {
        // A camera authored with a degenerate FOV would divide by zero here and put the rectangle at
        // infinity; the floor is the smallest tangent the FOV slider can reach (10 deg) divided by a
        // safety decade, so it never binds on an authored value and always binds on a broken one.
        const float tanHalf = std::max( tanHalfFovY, 1.0e-3f );
        const float wanted  = kGizmoHalfExtentPerDistance * std::max( viewerDistance, 0.0f ) / tanHalf;
        return std::clamp( wanted, kGizmoMinDepth, std::max( farPlane, kGizmoMinDepth ) );
    }

    /**
     * @brief The camera gizmo's wireframe, in world space.
     *
     * @param world       the camera entity's WORLD transform (parents composed). Its translation is the
     *                    apex; columns 0/1/2 are its right/up/backward axes, which is where roll comes
     *                    from — taking the axes from the matrix rather than rebuilding them from a
     *                    forward vector and a global up is what makes the up marker mean anything.
     * @param aspect      width / height of the rectangle. The camera component stores no aspect of its
     *                    own, so the caller passes the viewport's; the gizmo then shows the frame the
     *                    editor would actually render through this camera.
     * @param tanHalfFovY tan of half the camera's vertical field of view.
     * @param farPlane    the camera's own Far, world units.
     * @param viewerPos   the EDITOR camera's world position.
     */
    [[nodiscard]] inline CameraFrustumGizmo BuildCameraFrustumGizmo( const glm::mat4& world, float aspect,
                                                                     float tanHalfFovY, float farPlane,
                                                                     const glm::vec3& viewerPos )
    {
        CameraFrustumGizmo out;
        out.Apex = glm::vec3( world[3] );

        // Normalised so an entity scale (the shipped scenes author cameras at scale 100) cannot change
        // the gizmo's size — the shape reports the camera's OPTICS, and scale is not one of them.
        const glm::vec3 right   = glm::normalize( glm::vec3( world[0] ) );
        const glm::vec3 up      = glm::normalize( glm::vec3( world[1] ) );
        const glm::vec3 forward = -glm::normalize( glm::vec3( world[2] ) );

        out.Depth = GizmoFrustumDepth( glm::length( viewerPos - out.Apex ), farPlane, tanHalfFovY );

        const float     halfHeight = out.Depth * std::max( tanHalfFovY, 1.0e-3f );
        const float     halfWidth  = halfHeight * aspect;
        const glm::vec3 centre     = out.Apex + forward * out.Depth;

        out.FarCorners[0] = centre - right * halfWidth - up * halfHeight;
        out.FarCorners[1] = centre + right * halfWidth - up * halfHeight;
        out.FarCorners[2] = centre + right * halfWidth + up * halfHeight;
        out.FarCorners[3] = centre - right * halfWidth + up * halfHeight;

        // The marker sits on the TOP edge and points further up, so it cannot be confused with a corner.
        const float markerHalf = halfWidth * 0.25f;
        out.UpMarker[0]        = centre + up * halfHeight - right * markerHalf;
        out.UpMarker[1]        = centre + up * halfHeight + right * markerHalf;
        out.UpMarker[2]        = centre + up * ( halfHeight + markerHalf * 1.5f );

        return out;
    }
} // namespace Desert::Editor::Tools
