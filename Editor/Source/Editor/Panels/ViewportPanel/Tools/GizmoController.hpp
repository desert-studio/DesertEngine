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
                           const glm::vec2& viewportPos, const glm::vec2& viewportSize );
        // A tool's own frame (UE's combined translate + rotate gizmo on a tool's transform proxy, e.g. Warp's
        // deformer): edits @p frame in place, in its local space. No undo - the frame is tool state. True on a
        // frame the drag changed it.
        // @p handles: kFrameTranslateRotate (Warp) or kFrameTranslateRotateScale (Lattice's point selection).
        static constexpr int kFrameTranslateRotate      = 1;
        static constexpr int kFrameTranslateRotateScale = 2;
        bool                 RenderFrame( const char* site, glm::mat4& frame,
                                          const std::shared_ptr<::Desert::Core::Camera>& camera, const glm::vec2& viewportPos,
                                          const glm::vec2& viewportSize, int handles );
        // UE's UIntervalGizmo on a tool frame (Warp's Upper / Lower bounds): the frame's Z axis between @p lower
        // and @p upper (cm from the frame origin) with a handle at each end, dragged along the axis; upper stays
        // >= 0 and lower <= 0, as UE clamps them. A hovered or held handle stands picking down like the gizmo.
        // True on a frame a drag changed a bound.
        // A handle is hovered or held: the caller disables ImGuizmo for the frame gizmo this frame.
        bool IntervalOwnsInput() const
        {
            return m_IntervalHover || m_IntervalDrag != 0;
        }
        // The cursor is over one of our scale handles, or one is held (see ScaleHandles): a click is the gizmo's.
        bool ScaleHandleBusy() const
        {
            return m_ScaleAxis >= 0 || m_ScaleHover >= 0;
        }
        bool RenderInterval( const glm::mat4& frame, float& lower, float& upper,
                             const std::shared_ptr<::Desert::Core::Camera>& camera, const glm::vec2& viewportPos,
                             const glm::vec2& viewportSize );
        // Bone gizmo (Skeleton Edit mode) — edits the selected bone's LocalBindTransform.
        void RenderBone( ::Desert::Core::Scene& scene, const std::shared_ptr<::Desert::Core::Camera>& camera,
                         const glm::vec2& viewportPos, const glm::vec2& viewportSize );

    private:
        // The per-axis scale handles (the balls), hit-tested and dragged on SCREEN here instead of by ImGuizmo:
        // its GetScaleType intersects the mouse ray with the plane whose NORMAL is the axis, so with the camera
        // near that plane the ray meets it behind the eye and the ball cannot be grabbed. ImGuizmo is upstream
        // (not patched); this owns the scale drag, the caller disables ImGuizmo while Owns is set.
        // @p operation: the ImGuizmo::OPERATION bits handed to Manipulate. @p snap: scale ratio step or null.
        struct ScaleHandleResult
        {
            bool Owns    = false; // a scale handle is held: ImGuizmo must stand down this frame
            bool Changed = false; // @p matrix was rescaled
        };
        ScaleHandleResult ScaleHandles( const char* site, glm::mat4& matrix, const glm::mat4& view,
                                        const glm::mat4& proj, const glm::vec2& viewportPos,
                                        const glm::vec2& viewportSize, int operation, const float* snap );
        // Draws the hovered / held handle over ImGuizmo's (call after Manipulate).
        void DrawScaleHandle( const char* site ) const;

        bool m_Hovered = false;
        // ScaleHandles state: the held axis (-1 none) and the site holding it, the hovered axis this frame.
        int         m_ScaleAxis  = -1;
        int         m_ScaleHover = -1;
        const char* m_ScaleSite  = nullptr;
        glm::mat4   m_ScaleStart{ 1.0f };   // the matrix when the drag began
        glm::vec2   m_ScaleMouse{ 0.0f };   // the cursor when the drag began
        glm::vec2   m_ScaleOrigin{ 0.0f };  // gizmo origin on screen
        glm::vec2   m_ScaleDir{ 0.0f };     // unit screen direction origin -> ball
        float       m_ScalePixels = 1.0f;   // screen distance origin -> ball at scale 1
        float       m_ScaleFactor = 1.0f;   // the held drag's current ratio (for drawing)
        // RenderInterval's held handle: 0 none, 1 lower, 2 upper.
        int m_IntervalDrag = 0;
        // The cursor was over a handle last frame: the frame gizmo stands down so the handle gets the click.
        bool m_IntervalHover = false;

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
