#pragma once

// ── WHAT A CAMERA ENTITY SEES, AS ONE PURE FUNCTION ─────────────────────────────────────────────────
//
// Two consumers ask this question and they used to answer it twice, differently:
//
//   * Play mode (Scene::UpdateActiveCameraSource) turned the entity's world matrix into Euler angles
//     through `glm::quat_cast` and handed those to the GameplayCamera. `quat_cast` assumes a pure
//     rotation, and the shipped scenes author cameras at scale 100, so the angles it produced came from
//     a matrix it was never meant to read.
//   * The Details button "Look through this camera" moved the EDITOR camera with SnapToDirection and
//     Focus: forward only — no roll — and nothing of the lens. The editor camera kept its own field of
//     view, its own near/far, and its size-anchored projection (EditorCamera::UpdateProjectionMatrix),
//     so the owner changed the camera's FOV and the view did not move. That was the complaint.
//
// Now both read the entity through the functions below: orientation from the world matrix's own axes
// (normalised, so scale is not an optic), roll included because the up axis is taken from the matrix
// and not rebuilt from a global up, and the projection built from the component's FOV/Near/Far exactly
// as the gameplay camera builds it. The pilot mode in the editor also WRITES a pose back through
// `CameraWorldTransformFor`, which is this function's inverse and is tested as one.
//
// ASPECT. The camera component stores no aspect, so the frame is the viewport's: at the same vertical
// FOV a wider viewport shows more to the sides, exactly as Play does in that viewport. There is nothing
// to letterbox against until a component carries an aspect of its own.

#include <Engine/Core/EditorCameraBasis.hpp>
#include <Engine/Core/Projection.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdint>

namespace Desert::Core
{
    struct CameraEntityView
    {
        glm::vec3 Position{ 0.0f };
        ViewBasis Basis;
        float     FovYDegrees = 45.0f;
        float     Near        = kDefaultNearPlane;
        float     Far         = kDefaultFarPlane;
    };

    /**
     * @brief The view a camera entity renders, from its WORLD transform and its lens.
     *
     * The matrix's columns 0/1/2 are the entity's right/up/backward axes. They are normalised (entity
     * scale is not an optic) and the up axis is re-orthogonalised against forward, so a sheared parent
     * chain still yields a basis `glm::lookAt` can use.
     */
    [[nodiscard]] inline CameraEntityView CameraEntityViewOf( const glm::mat4& world, float fovYDegrees,
                                                              float nearPlane, float farPlane )
    {
        CameraEntityView out;
        out.Position = glm::vec3( world[3] );

        const glm::vec3 forward = -glm::normalize( glm::vec3( world[2] ) );
        const glm::vec3 rawUp   = glm::normalize( glm::vec3( world[1] ) );
        out.Basis.Forward       = forward;
        out.Basis.Up            = glm::normalize( rawUp - forward * glm::dot( rawUp, forward ) );

        out.FovYDegrees = fovYDegrees;
        out.Near        = nearPlane;
        out.Far         = farPlane;
        return out;
    }

    [[nodiscard]] inline glm::mat4 ViewMatrixOf( const CameraEntityView& view )
    {
        return glm::lookAt( view.Position, view.Position + view.Basis.Forward, view.Basis.Up );
    }

    // The plain vertical-FOV projection at the viewport's aspect — the one Camera::UpdateProjectionMatrix
    // builds for a GameplayCamera. Deliberately NOT the editor camera's height-anchored one: that
    // projection widens the effective FOV with the viewport's height, which is right for flying around a
    // level and wrong for showing what a lens sees.
    [[nodiscard]] inline glm::mat4 ProjectionOf( const CameraEntityView& view, uint32_t width, uint32_t height )
    {
        const float aspect = static_cast<float>( width ) / static_cast<float>( std::max( height, 1u ) );
        return MakePerspective( glm::radians( view.FovYDegrees ), aspect, view.Near, view.Far );
    }

    /**
     * @brief The inverse of CameraEntityViewOf's pose half: a world matrix whose camera sits at
     *        @p position looking along @p basis, carrying @p scale on its axes.
     *
     * The pilot mode uses it to write the viewport's pose back into the entity. The scale is the
     * entity's own, passed through untouched: flying a camera must not rescale it.
     */
    [[nodiscard]] inline glm::mat4 CameraWorldTransformFor( const glm::vec3& position, const ViewBasis& basis,
                                                            const glm::vec3& scale )
    {
        const glm::vec3 f = glm::normalize( basis.Forward );
        const glm::vec3 u = glm::normalize( basis.Up - f * glm::dot( basis.Up, f ) );
        const glm::vec3 r = glm::cross( f, u );

        glm::mat4 m( 1.0f );
        m[0] = glm::vec4( r * scale.x, 0.0f );
        m[1] = glm::vec4( u * scale.y, 0.0f );
        m[2] = glm::vec4( -f * scale.z, 0.0f );
        m[3] = glm::vec4( position, 1.0f );
        return m;
    }
} // namespace Desert::Core
