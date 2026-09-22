#pragma once

#include <Editor/Core/Selection/AuthoringContext.hpp>

#include <Engine/Desert.hpp>
#include <Engine/Animation/Rig/ControlManipulator.hpp>
#include <ImGui/imgui.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    class LightGizmoRenderer
    {
    public:
        explicit LightGizmoRenderer( const std::shared_ptr<Desert::Core::Scene>& scene );
        ~LightGizmoRenderer() = default;

        /**
         * @brief One frame of every viewport overlay.
         *
         * @param owner  WHO IS DRAWING, in the authoring election's vocabulary. Needed because the control
         *               overlay WRITES — a click selects a control, and a rebuilt rig drops the selection —
         *               and every write to the authoring context names its owner or is refused. The
         *               renderer has no owner of its own: it is the viewport's overlay and says so.
         * @param mine   that owner's durable copy of the context, written through in the same step as the
         *               publication (see AuthoringContextHost::Write).
         */
        // @p camera: THIS VIEWPORT's angle. The icons are projected with it, so taking the scene's would
        // have drawn every light's billboard where view 0 sees it.
        void Render( const std::shared_ptr<::Desert::Core::Camera>& camera, float width, float height, float xpos,
                     float ypos, const Core::AuthoringOwner& owner, Core::AuthoringContext& mine );

        // Nearest skeleton bone head within radiusPx of the (absolute-screen) mouse position, taken from the
        // last skeleton-overlay frame; -1 if none. Populated by RenderSkeleton; drives viewport bone picking.
        int PickBone( const ImVec2& absMouse, float radiusPx = 12.0f ) const;

        // True while the mouse is over a light billboard OR a drag handle this frame — both consume the
        // click, so the scene ray-pick must not run under them (it would hit whatever is behind).
        bool IsLightIconHovered() const
        {
            return m_LightIconHovered;
        }

        // True while a radius / range / cone handle is being dragged. The viewport keeps its camera and
        // gizmo off during that (a handle drag is a value edit, not a selection or a move).
        bool IsDraggingHandle() const
        {
            return m_ActiveHandle != HandleKind::None;
        }

    private:
        void RenderPointLights( const std::shared_ptr<Desert::Core::Camera>& camera, float width, float height,
                                float xpos, float ypos );
        // Sun billboard + direction arrow (direction = -normalize(Translation), the shading convention).
        void RenderDirectionLights( const std::shared_ptr<Desert::Core::Camera>& camera, float width,
                                    float height );
        void RenderSpotLights( const std::shared_ptr<Desert::Core::Camera>& camera, float width, float height,
                               float xpos, float ypos );
        // Camera entities (CameraComponent): billboard icon + wireframe view frustum.
        void RenderCameras( const std::shared_ptr<Desert::Core::Camera>& camera, float width, float height,
                            float xpos, float ypos );
        // Skeleton Edit mode: draw the selected skinned mesh's bind-pose skeleton (bone heads + parent->child
        // links) as an overlay, with the bone-tree-selected bone highlighted. Read-only (Phase 1).
        void RenderSkeleton( const std::shared_ptr<Desert::Core::Camera>& camera, float width, float height,
                             float xpos, float ypos );
        // In-editor rig placement (RigBuilder): overlay the bones being placed on a static mesh before
        // "Convert to Skinned". Shares the UE-style visuals with RenderSkeleton via DrawBoneGizmos.
        void RenderRigBuilder( const std::shared_ptr<Desert::Core::Camera>& camera, float width, float height );
        /**
         * @brief Control Rig mode: draw the selected entity's control shapes, pick one, and drag it.
         *
         * THE CALLER `ControlManipulator` NEVER HAD (T5.2). That file answers three questions per frame —
         * where are the shapes, is the pointer on one, what does a drag write — adds no pass, no entity and
         * no component, and until this function existed nothing asked it any of them.
         *
         * It reads the LIVE hierarchy out of the entity's Animator, which is the stage AnimationECSSystem
         * built from the entity's `.derig`. So a drag writes into the object the Rig pipeline stage is about
         * to evaluate, and the moved control reaches the skinning matrices on the same frame — there is no
         * copy in between that could go stale, which is why nothing here caches a hierarchy.
         */
        void RenderControlRig( const std::shared_ptr<Desert::Core::Camera>& camera, float width, float height,
                               float xpos, float ypos, const Core::AuthoringOwner& owner,
                               Core::AuthoringContext& mine );
        // Shared UE-style bone drawing: octahedral parent->child links + sphere joints, from already-projected
        // absolute-screen head positions (nullopt = behind camera). parents[i] < 0 marks a root. When
        // recordForPick is set, fills m_BoneScreenPositions for PickBone.
        void DrawBoneGizmos( ImDrawList* drawList, const std::vector<std::optional<ImVec2>>& screen,
                             const std::vector<int>& parents, const std::vector<std::string>& names,
                             int selectedBone, bool showAllNames, bool recordForPick );
        // Billboard icons for entities with no rendered geometry (spawn points, audio emitters, triggers,
        // empties) so they are visible in the viewport. Hover shows a tooltip; the normal LMB pick selects them.
        void RenderSpawnIcons( const std::shared_ptr<Desert::Core::Camera>& camera, float width, float height );
        // Text entities (TextComponent): a big, click-selectable "Aa" billboard so a label is easy to find
        // and grab in the viewport even when its glyphs are small/edge-on.
        void RenderTextIcons( const std::shared_ptr<Desert::Core::Camera>& camera, float width, float height );
        // Draws a world-space line segment, clipping the endpoint that crosses the editor camera's near
        // plane (so a segment dipping behind the camera never wraps across the whole viewport).
        void DrawWorldLine( ImDrawList* drawList, const glm::vec3& a, const glm::vec3& b, const glm::mat4& mvp,
                            float width, float height, float windowX, float windowY, ImU32 color,
                            float thickness = 1.5f );
        void DrawAxisAlignedCircle( ImDrawList* drawList, const glm::vec3& center, float radius, int segments,
                                    const glm::vec3& axis1, const glm::vec3& axis2, const glm::mat4& mvp,
                                    float width, float height, float xpos, float ypos, ImU32 color );

        void DrawLightRadiusSphere( const std::shared_ptr<Desert::Core::Camera>& camera, const glm::vec3& worldPos,
                                    float radius, float width, float height, float windowX, float windowY,
                                    float iconCenterX, float iconCenterY );

        void DrawSpotCone( const std::shared_ptr<Desert::Core::Camera>& camera, const glm::vec3& apex,
                           const glm::vec3& dir, float outerAngleDeg, float range, float width, float height,
                           float windowX, float windowY );

        // --- draggable value handles (selected light only) ------------------------------------------
        // What a grab is currently editing. One at a time: a drag owns the mouse until it is released.
        enum class HandleKind
        {
            None,
            PointRadius,
            SpotRange,
            SpotOuterAngle,
        };

        // A round grab dot at @p handle. Dragging scales @p value by the RATIO of the pointer's distance
        // from @p center to that distance when the grab started, which keeps the feel identical at any
        // zoom or camera angle without needing a 3D ray intersection. Returns true when it wrote a value;
        // pushes one undo entry per completed drag.
        /**
         * @brief One proportional drag handle.
         *
         * @param value       what the DRAG scales. May be a derived quantity (the spot cone drags
         *                    tan(angle), because the rim's distance from the axis IS range*tan(angle)),
         *                    and may therefore be a caller's local.
         * @param undoTarget  the AUTHORED field the undo entry must address. For a derived @p value this
         *                    is a DIFFERENT object; for a direct one it is `&value`.
         *
         * THE TWO ARE SEPARATE PARAMETERS BECAUSE CONFLATING THEM WAS A DEFECT. This used to store
         * `&value` in a member on mouse-down and use it on the release frame — and the spot-cone caller
         * passes a stack local, so the release read `*m_ActiveHandleTarget` in a dead frame and then
         * handed that dead address to CommandHistory, whose ByteCommand::Undo memcpy's into it. A Ctrl+Z
         * after a cone drag wrote four bytes into somebody else's stack; it "worked" only because the
         * frame's layout repeats. Nothing addressed across frames now: the pointer used at release is the
         * one this frame passed in, and the undo entry addresses a component field, which is what
         * CommandHistory::DropVolatile is able to protect. A8-2.
         */
        bool DragValueHandle( HandleKind kind, const Common::UUID& owner, const ImVec2& center,
                              const ImVec2& handle, float& value, float minValue, float maxValue,
                              const char* tooltip, float* undoTarget );

        // Is this the entity the Details panel is showing? Handles only appear on the selected light —
        // otherwise a scene full of lights would be a minefield of grab dots.
        bool IsSelected( const ECS::Entity& entity ) const;

    private:
        std::shared_ptr<Desert::Core::Scene> m_Scene;

        // (boneIndex, absolute-screen head position) captured each frame RenderSkeleton draws — the source
        // for PickBone. Cleared when Skeleton Edit mode is inactive so stale positions never pick.
        std::vector<std::pair<int, ImVec2>> m_BoneScreenPositions;

        bool m_LightIconHovered = false; // see IsLightIconHovered()

        // The in-progress control drag. IMMEDIATE MODE MEANS THIS LIVES IN THE CALLER (ControlManipulator's
        // own words), and this overlay is the caller. It keeps the control's LOCAL pose at the grab and
        // nothing global — see ControlDrag's note on why remembering the global is a defect that reads as a
        // rig failure rather than a manipulator one.
        Animation::ControlDrag m_ControlDrag;

        // Perform one queued nudge (Editor/Core/ControlNudgeRequest.hpp) on the selected control. It is a
        // method of this class and not a free function because the two things a drag needs — the grabbed
        // shape's projected origin and the in-progress drag itself — are members of it.
        void PerformQueuedControlNudge( Animation::ControlHierarchy&      hierarchy,
                                        const Animation::ManipulatorView& view );

        // The pose the grab captured. READ by RecordControlDrag when the drag completes -- which is what
        // makes "one undo entry per completed drag" a line of code rather than, as it was until A33, a
        // sentence in this comment with nothing behind it. Held BY VALUE, never as an address;
        // DragValueHandle's note is about exactly that.
        //
        // The UUID that used to sit beside it is GONE rather than kept: nothing ever read it either, and
        // the entry is volatile, so DropVolatile takes it on the very selection change the UUID existed
        // to notice.
        Animation::BoneTransform m_ControlPoseAtGrab;

        // Reused between frames so a steady state allocates nothing after the first — the frame builder
        // takes it as an output parameter for that reason.
        Animation::ManipulatorFrame m_ControlFrame;
        // Built once, on first use: the six shipped shapes. A Result rather than a library because
        // BuiltIn() can refuse, and a discarded refusal is the "empty successful answer" the contract
        // forbids — a control nobody can grab, with nothing said.
        std::optional<Animation::ControlShapeLibrary> m_ControlShapes;

        // Active handle drag. The start value + start pixel distance define the proportional drag; the
        // captured bytes become one undo entry when the mouse is released.
        HandleKind   m_ActiveHandle = HandleKind::None;
        Common::UUID m_ActiveHandleOwner;
        // BOTH START VALUES ARE HELD BY VALUE, and no address is held at all — see DragValueHandle's note.
        // `m_DragStartValue` is the proxy the drag scales; `m_DragStartAuthored` is what the undo entry
        // must restore, and the two differ whenever the handle edits a derived quantity.
        float m_DragStartValue    = 0.0f;
        float m_DragStartAuthored = 0.0f;
        float m_DragStartDistance = 0.0f;
    };
} // namespace Desert::Editor