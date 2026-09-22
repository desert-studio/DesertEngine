#pragma once

#include <Engine/Desert.hpp>
#include <Editor/Core/GizmoState.hpp>
#include <Editor/Core/Commands/SceneCommands.hpp>

#include <Common/Core/UUID.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Desert::Editor::Tools
{
    // ImGuizmo-based viewport gizmos, extracted from ViewportPanel (god-object split). The current operation
    // lives in the editor-global Core::GizmoState (shared with the main toolbar); this owns only the
    // "gizmo is being hovered/dragged" flag (so picking/painting can stand down). Context (scene, selection,
    // the scene-image rect) is passed in per frame; selection comes from SelectionManager.
    class GizmoController
    {
    public:
        // Alias the shared state's enum so existing GizmoController::Operation::X call sites keep compiling.
        using Operation = Core::GizmoState::Operation;

        void      SetOperation( Operation op ) { Core::GizmoState::Set( op ); }
        Operation GetOperation() const { return Core::GizmoState::Get(); }
        bool      IsActive() const { return Core::GizmoState::Get() != Operation::None; }
        bool      IsHovered() const { return m_Hovered; }
        // Call once per frame before rendering the gizmo. It also drops the editor-wide "a pose gizmo is
        // being held" bit, because that bit's one writer is `RenderBone` — and `RenderBone` is not called
        // at all outside Pose mode, so a frame in which the user leaves that mode mid-drag would otherwise
        // leave it stuck on and the Sequencer waiting for a release that never comes.
        void ResetHovered();

        // Object transform gizmo on the current selection. viewportPos/Size = the rendered scene-image rect.
        // @p camera is THIS VIEWPORT's camera, handed in rather than taken from the scene: a scene has a
        // list of views now and `Scene::GetMainCamera()` answers for view 0, so a second viewport would
        // have placed its gizmo through another one's projection and dragged the wrong direction.
        void RenderObject( ::Desert::Core::Scene& scene, const std::shared_ptr<::Desert::Core::Camera>& camera,
                           const glm::vec2& viewportPos,
                           const glm::vec2& viewportSize );
        // Bone gizmo (Skeleton Edit mode) — edits the selected bone's LocalBindTransform.
        void RenderBone( ::Desert::Core::Scene& scene, const std::shared_ptr<::Desert::Core::Camera>& camera,
                         const glm::vec2& viewportPos,
                         const glm::vec2& viewportSize );

    private:
        bool m_Hovered = false;

        // One undo entry per gizmo drag: pre-drag TRS of every selected top-level root captured when the
        // drag starts, committed as one (possibly composite) command on release.
        bool                                     m_DragActive = false;
        Common::UUID                             m_DragEntity = Common::UUID::Null(); // primary at drag start
        std::vector<Commands::TransformSnapshot> m_DragSnapshots;

        // Bone-gizmo undo: the selected bone's LocalBindTransform captured at drag start, committed as one
        // re-resolvable command on release (keyed by mesh handle + bone index, so it survives skeleton reloads).
        bool                m_BoneDragActive = false;
        int                 m_BoneDragIndex  = -1;
        Assets::AssetHandle m_BoneDragMesh;
        glm::mat4           m_BoneDragOld{ 1.0f };
    };
} // namespace Desert::Editor::Tools
