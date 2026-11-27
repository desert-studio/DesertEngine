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
    // ── WHAT THIS VIEWPORT LOOKS THROUGH: PILOT AND CAMERA CUT, AS IN UE ───────────────────────────────
    //
    // Two lock layers, UE's FLevelEditorViewportClient ActorLockStack:
    //   CINEMATIC  — the Level Sequence's Camera Cut in force (UE SetCinematicActorLock), set by the
    //                Sequencer through SetCinematicCamera below;
    //   USER       — the user's Pilot (Details button, Outliner menu, palette).
    // The ACTIVE entity is the cinematic one when set, else the user's. While an entity is active the
    // viewport shows exactly what that camera sees (Core::CameraEntityViewOf — position, orientation with
    // roll, FOV, Near, Far), and flying the viewport moves the ACTIVE entity (one undo step per stretch).
    //
    // The editor camera's pose is saved when the first layer is set and restored when the last one is
    // cleared — never in between, so cut → cut → pilot → cut returns to where the user was.
    //
    // Every write to the EditorCamera happens in Update: the setters only record the request. When the
    // active entity changes (a cut switch, Begin, Eject) Update places the camera WITHOUT reading the
    // difference as the user's flight, and calls EditorCamera::FollowEntity so the view's camera identity
    // changes: the renderer's history reset (SceneViewState, HistoryResetReason::CameraCut) fires on its own.
    class CameraPilot
    {
    public:
        // The user layer. Refuses with the reason when the entity is missing or not a camera of @p scene.
        // Starting on another entity while one is piloted moves the session over.
        [[nodiscard]] Common::BoolResultStr Begin( const ::Desert::Core::Scene& scene, const Common::UUID& uuid );
        // Clears the user layer. With no cinematic layer the next Update restores the saved pose; @p camera is
        // the view's editor camera when the view is still open (null: the view closed, nothing to restore).
        void Eject( ::Desert::Core::EditorCamera* camera );

        // The cinematic layer (nullopt clears it). An entity that is not a camera is refused by the Sequencer
        // host already (LevelSequenceEntityHost::SetCamera); one that stops being a camera is dropped by Update
        // with a warning, as a user pilot is.
        void                                             SetCinematic( const std::optional<Common::UUID>& uuid );
        [[nodiscard]] const std::optional<Common::UUID>& Cinematic() const
        {
            return m_Cinematic;
        }

        // Once per frame while the viewport shows the editor camera.
        void Update( const ::Desert::Core::Scene& scene, ::Desert::Core::EditorCamera& camera );

        [[nodiscard]] bool IsActive() const
        {
            return m_Cinematic.has_value() || m_User.has_value();
        }
        [[nodiscard]] bool IsCinematic() const
        {
            return m_Cinematic.has_value();
        }
        // The user layer's entity (the Details button asks "is THIS camera piloted").
        [[nodiscard]] const std::optional<Common::UUID>& Entity() const
        {
            return m_User;
        }
        // The active entity's name, for the caption ("Camera Cut: <name>" / "Piloting: <name>").
        [[nodiscard]] const std::string& EntityName() const
        {
            return m_ShownName;
        }

    private:
        [[nodiscard]] std::optional<Common::UUID> ActiveEntity() const
        {
            return m_Cinematic ? m_Cinematic : m_User;
        }
        void CloseSegment();

        std::optional<Common::UUID> m_User;
        std::optional<Common::UUID> m_Cinematic;

        // What Update placed last frame (nullopt = the editor camera's own pose) and its name.
        std::optional<Common::UUID> m_Shown;
        std::string                 m_ShownName;

        // The editor camera's pose before the first layer; valid while m_HasSaved.
        bool                               m_HasSaved = false;
        ::Desert::Core::EditorCamera::Pose m_Saved;

        // The pose this wrote onto the camera last frame; a difference is the user's flight.
        glm::vec3                 m_LastPosition{ 0.0f };
        ::Desert::Core::ViewBasis m_LastBasis;

        // One undo step per stretch of motion: the shown entity's transform when the stretch began.
        bool      m_InSegment = false;
        glm::vec3 m_SegmentTranslation{ 0.0f };
        glm::vec3 m_SegmentRotation{ 0.0f };
        glm::vec3 m_SegmentScale{ 1.0f };
    };

    // The seam the Details button, the Outliner's context menu and the command palette all go through:
    // defined by ViewportPanel.cpp, which owns the pilots. Pilot acts on the active viewport.
    [[nodiscard]] Common::BoolResultStr PilotCameraEntity( const Common::UUID& entity );
    [[nodiscard]] Common::BoolResultStr EjectPilot();
    [[nodiscard]] bool                  IsPiloted( const Common::UUID& entity );

    // THE SEQUENCER'S SEAM (UE: FSequencer::UpdatePreviewLevelViewportClientFromCameraCut over every level
    // viewport client that AllowsCinematicControl). Sets the cinematic layer of every live viewport showing
    // @p scene that allows cinematic control.
    void SetCinematicCamera( const ::Desert::Core::Scene& scene, const Common::UUID& camera );
    // Clears it on those viewports whose cinematic camera is @p camera (another document's cut stays).
    void ReleaseCinematicCamera( const ::Desert::Core::Scene& scene, const Common::UUID& camera );
} // namespace Desert::Editor
