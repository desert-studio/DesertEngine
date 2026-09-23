#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Common/Core/UUID.hpp>

#include <Engine/Core/Camera.hpp>

#include <optional>
#include <string>

namespace Desert::Core
{
    class Scene;
}

namespace Desert::Editor
{
    // ── PILOT / EJECT, AS IN UE ─────────────────────────────────────────────────────────────────────
    //
    // A viewport PILOTING a camera entity is locked to it: every frame the viewport shows exactly what
    // that camera sees (Core::CameraEntityViewOf — position, orientation with roll, FOV, Near, Far), so
    // a value changed in the Details panel moves the view the next frame. Flying the viewport
    // (WASD, RMB-look, F, a named angle) moves the CAMERA instead of the viewport, and each stretch of
    // motion is one undo step. Eject puts the editor camera back where it was before the session.
    //
    // It REPLACES the Details button that moved the editor camera to the entity once (forward only: no
    // roll, and none of the lens), which is what the owner reported as "I change the data and the view
    // stays the same".
    //
    // THE ORDER INSIDE ONE FRAME. The scene ticks the editor camera (user input) and renders; then this
    // runs from the viewport's UI pass: if the camera moved since the pose this wrote last frame, the
    // move is the user's and goes INTO the entity; either way the entity's view is then put back onto
    // the camera. So the entity is the single source of truth and the viewport only ever proposes.
    class CameraPilot
    {
    public:
        // Start piloting @p entity from @p camera. Refuses with the reason when the entity is missing or
        // is not a camera. Starting on another entity while one is piloted moves the session over
        // without losing the pose to eject to.
        [[nodiscard]] Common::BoolResultStr Begin( const ::Desert::Core::Scene& scene, const Common::UUID& entity,
                                                   ::Desert::Core::EditorCamera& camera );

        // End the session and restore the pose the editor camera had before it. @p camera may be null
        // (the view was closed); the session ends either way.
        void Eject( ::Desert::Core::EditorCamera* camera );

        // Once per frame while the viewport shows the editor camera.
        void Update( const ::Desert::Core::Scene& scene, ::Desert::Core::EditorCamera& camera );

        [[nodiscard]] bool IsActive() const
        {
            return m_Entity.has_value();
        }
        [[nodiscard]] const std::optional<Common::UUID>& Entity() const
        {
            return m_Entity;
        }
        // The piloted entity's name, for the viewport's "Piloting: <name>" caption.
        [[nodiscard]] const std::string& EntityName() const
        {
            return m_EntityName;
        }

    private:
        void CloseSegment();

        std::optional<Common::UUID>        m_Entity;
        std::string                        m_EntityName;
        ::Desert::Core::EditorCamera::Pose m_Saved;

        // The pose this wrote onto the camera last frame; a difference is the user's flight.
        glm::vec3                 m_LastPosition{ 0.0f };
        ::Desert::Core::ViewBasis m_LastBasis;

        // One undo step per stretch of motion: the entity's transform when the stretch began.
        bool      m_InSegment = false;
        glm::vec3 m_SegmentTranslation{ 0.0f };
        glm::vec3 m_SegmentRotation{ 0.0f };
        glm::vec3 m_SegmentScale{ 1.0f };
    };

    // The seam the Details button, the Outliner's context menu and the command palette all go through:
    // defined by ViewportPanel.cpp, which owns the pilots, so none of those callers needs the panel's
    // header. Pilot acts on the viewport the user is working in (ViewportPanel::ActiveViewport).
    [[nodiscard]] Common::BoolResultStr PilotCameraEntity( const Common::UUID& entity );
    [[nodiscard]] Common::BoolResultStr EjectPilot();
    // Is any viewport piloting @p entity right now (the Details button reads it to offer Eject instead).
    [[nodiscard]] bool IsPiloted( const Common::UUID& entity );
} // namespace Desert::Editor
