#pragma once

/**
 * THE MANIPULATOR IS A FUNCTION OF THE FRAME, NOT A THING IN THE SCENE.
 *
 * T5.2, second half. Report 01 §833 looks at UE's control shapes — a transient `AActor` with a
 * `UPrimitiveComponent` and a `UMaterialInstanceDynamic` PER CONTROL, existing only so the engine's
 * hit-proxy stack works unchanged, and carrying a "shape actor got detached, re-attach it" repair at
 * `ControlRigEditMode.cpp:6038-6055` — and says an immediate-mode manipulator layer is cheaper. 06 §3.16
 * adds the reason that is ours rather than Epic's: there is no 3D debug-geometry pass in this engine to
 * put such an actor's geometry into, and re-verified 2026-09-16 there still is not.
 *
 * So this file adds NO PASS, NO ENTITY AND NO COMPONENT. It answers three questions per frame —
 * where are the shapes on screen, is the pointer on one, and what does dragging one write — and keeps
 * nothing between frames except an in-progress drag, which lives in the caller.
 *
 * ── WHERE IT SITS IN THE FRAME ───────────────────────────────────────────────────────────────────────
 *
 * Nowhere in the render graph. `BuildFrame` turns controls into SCREEN SEGMENTS; the viewport overlay
 * that already draws bone gizmos and light billboards draws them, after the scene image, in the same
 * ImGui draw list. That is the whole of "where the pass stands": there is no pass, which is exactly the
 * saving report 01 §833 is pointing at. The consequence is stated rather than hidden — control shapes do
 * not depth-test against scene geometry and are always drawn on top, which is what a manipulator wants
 * and what UE's hit proxies effectively give too.
 *
 * ── THE PIXEL SPACE, AND WHY THE PROJECTION LIVES HERE ───────────────────────────────────────────────
 *
 * `ProjectToViewport` is the ONE world -> viewport-pixel function in the tree. It used to be a second,
 * private copy in `LightGizmoRenderer.cpp`; a manipulator that agreed with that copy only by inspection
 * would be a convention held in two places, and the class of defect that produces is the one the
 * contract calls "one source of truth per value". Y GROWS DOWNWARD, which is ImGui's convention and
 * therefore the viewport's.
 *
 * ── IT DOES NOT KNOW THE DEPTH CONVENTION, AND THAT IS DELIBERATE ────────────────────────────────────
 *
 * Nothing here asks whether NDC z runs near->far or far->near, and nothing needs the camera position.
 * The drag intersects a LINE through the pointer with a plane (an intersection is the same point for
 * either orientation of the line), and the arcball basis is built from PIXEL OFFSETS unprojected at one
 * fixed depth, whose differences give screen-right and screen-down in world regardless. A layer that
 * took the convention as an input would have a knob that is wrong on exactly one camera type and right
 * everywhere it was tested.
 */

#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlShape.hpp>

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief The camera and the rectangle, as everything in this file needs them.
     *
     * `ViewportOrigin` is the rectangle's top-left IN THE SAME PIXEL SPACE the pointer is given in. For
     * the editor that is absolute-screen, because that is what ImGui hands back for the mouse; for a test
     * it is whatever the test chose. Keeping it a parameter rather than assuming zero is what stopped the
     * bone overlay's own picking from being off by the panel's position.
     */
    struct ManipulatorView
    {
        glm::mat4 ViewProjection = glm::mat4( 1.0F );
        glm::vec2 ViewportOrigin = glm::vec2( 0.0F );
        glm::vec2 ViewportSize   = glm::vec2( 1.0F );
    };

    /**
     * @brief Where a world point landed, and whether it landed at all.
     *
     * `InFront` IS NOT DECORATION. A point behind the eye has clip w <= 0, and dividing by it anyway
     * mirrors the point to a plausible on-screen position — that is the defect that produced the ghost
     * light bulb when the camera turned 180 degrees and the radius circles streaking across the whole
     * viewport. A caller that ignores this flag draws a control that is not there.
     */
    struct ProjectedPoint
    {
        glm::vec2 Pixel   = glm::vec2( 0.0F );
        /// NDC z as the projection produced it. Reported because a caller may have a use for it; nothing
        /// in this file compares two of them, because the sense of the axis is the projection's business.
        float Depth = 0.0F;
        bool      InFront = false;
    };

    /// World -> viewport pixels. The one copy; see the file note.
    [[nodiscard]] ProjectedPoint ProjectToViewport( const ManipulatorView& view, const glm::vec3& world );

    /// One drawn segment, in pixels. A world segment crossing the near plane contributes the part in
    /// front and nothing else, which is why a run becomes segments here rather than staying a point list.
    struct ManipulatorSegment
    {
        glm::vec2 A = glm::vec2( 0.0F );
        glm::vec2 B = glm::vec2( 0.0F );
    };

    /**
     * @brief One control's shape, placed, projected and measured.
     *
     * Carries BOTH the world points and the screen segments on purpose: the screen side is what is drawn
     * and hit-tested, and the world side is what an assertion about placement can be written against
     * without a camera in it. `World * LibraryTransform * point` is checkable arithmetic; "it looked
     * right in the viewport" is not.
     */
    struct ControlShapeDraw
    {
        uint32_t  Control = ControlHierarchy::INVALID;
        glm::mat4 World   = glm::mat4( 1.0F ); ///< the control's global transform, as this frame read it

        std::vector<glm::vec3>  WorldPoints; ///< the shape's points placed in the world
        std::vector<glm::uvec2> Segments;    ///< index pairs into WorldPoints, closing runs included
        std::vector<ManipulatorSegment> Screen; ///< the visible part of each segment, in pixels

        ProjectedPoint Origin;                ///< the control's own origin, projected
        float          ScreenRadius = 0.0F;   ///< furthest drawn pixel from Origin — the arcball's radius
    };

    /**
     * @brief Everything the overlay needs for one frame.
     *
     * `UnknownShapes` IS PART OF THE ANSWER, not a log line this file swallows. A control naming a shape
     * the library does not have must not look identical to a control the rigger chose not to draw: the
     * animator's only symptom would be a control they cannot grab, with nothing on screen to explain it.
     */
    struct ManipulatorFrame
    {
        std::vector<ControlShapeDraw> Shapes;
        std::vector<std::string>      UnknownShapes;
    };

    /**
     * @brief Place, project and measure every control that names a shape.
     *
     * Takes the hierarchy by NON-CONST REFERENCE because reading a control's global is what resolves it
     * (T5.1's laziness is the point, not an accident), and the frame is an output parameter so a steady
     * state allocates nothing after the first frame.
     */
    void BuildFrame( ControlHierarchy& hierarchy, const ControlShapeLibrary& library, const ManipulatorView& view,
                     ManipulatorFrame& out );

    /// What the pointer is over. `Control` is `ControlHierarchy::INVALID` when it is over nothing, and
    /// that answer is the one worth testing: a hit test that always says yes passes every "I grabbed it".
    struct ManipulatorHit
    {
        uint32_t Control    = ControlHierarchy::INVALID;
        float    DistancePx = 0.0F;
    };

    /**
     * @brief The nearest control shape within @p radiusPx of @p pointer.
     *
     * DISTANCE TO THE WIRE, not to a bounding box and not to the origin: a control drawn as a big circle
     * is grabbed by its rim, and a box-based test would make every large control swallow the small ones
     * inside it. An exact tie goes to the lower control index — deterministic, and deliberately NOT a
     * depth comparison: this layer does not know which way NDC z runs (see the file note) and a tie-break
     * that is right on one camera type and silently inverted on another is worse than an arbitrary one.
     */
    [[nodiscard]] ManipulatorHit HitTest( const ManipulatorFrame& frame, const glm::vec2& pointer,
                                          float radiusPx );

    /// What a drag writes. Scale is absent on purpose — see `ControlDrag`'s note.
    enum class ManipulatorMode : uint8_t
    {
        Translate,
        Rotate,
    };

    /**
     * @brief One in-progress drag. IMMEDIATE MODE MEANS THIS LIVES IN THE CALLER.
     *
     * ── THE ONE THING IT REMEMBERS IS LOCAL ──────────────────────────────────────────────────────────
     *
     * The transform captured at the grab is the control's `Pose` — the LOCAL, authored side (A1 made it
     * the only authored side, which is why T5.1 stores no authored global). Every update recomputes the
     * control's parent space FRESH from the hierarchy and converts this frame's pointer delta into it.
     *
     * That is the answer to the hole T5.1 closed from the other direction. `SetGlobalTransform` back-
     * solves through the parent space and therefore READS it, and a parent nobody had read that pass
     * still held last frame's cache; T5.1 fixed it by resolving the control first. A drag is the same
     * read from the writing side, and the same mistake here would be to remember the control's GLOBAL at
     * the grab and keep re-asserting it: the control would then pin itself to where its parent used to
     * be, and the symptom — a dragged hand that stops following the arm — would look like a rig defect
     * rather than a manipulator one. Nothing global is stored here, and the suite's assertion is that a
     * parent moving mid-drag with the pointer still leaves `Pose` BIT-IDENTICAL while the global follows.
     *
     * ── WHAT IT WRITES ───────────────────────────────────────────────────────────────────────────────
     *
     * `SetPose`, never `SetGlobalTransform`. The parent space is recovered as `global * inverse(pose)`,
     * which is exactly `parent * offset` and needs no access to T5.1's private resolver — and the global
     * it is recovered from is the one the hierarchy just resolved, so the read is never of a stale cache.
     *
     * ── NO SCALE, AND THAT IS A DECISION ─────────────────────────────────────────────────────────────
     *
     * A scale drag is not hard; a scale CONTROL is a question T5.3 has to answer first (what does a
     * scale channel key to, and does a scaled control scale its children's spaces). Shipping the drag
     * before the answer would be a knob whose meaning changes under it. Left out, named here.
     */
    class ControlDrag
    {
    public:
        /**
         * @brief Grab @p control at @p pointer.
         *
         * @param arcballRadiusPx the trackball radius for `Rotate`, in pixels. Pass the grabbed shape's
         *        `ScreenRadius`: the shape the animator sees IS the trackball, so a rim-to-rim pointer
         *        sweep is half a turn. A constant here would feel wrong at every zoom but one.
         *
         * Refuses an unknown control, a degenerate parent space or pose, and a pointer the view cannot
         * turn into a line.
         */
        [[nodiscard]] Common::BoolResultStr Begin( ControlHierarchy& hierarchy, uint32_t control,
                                                   ManipulatorMode mode, const ManipulatorView& view,
                                                   const glm::vec2& pointer, float arcballRadiusPx );

        /// Move the pointer. Writes the control's local pose; see the class note on what is recomputed.
        [[nodiscard]] Common::BoolResultStr Update( ControlHierarchy& hierarchy, const ManipulatorView& view,
                                                    const glm::vec2& pointer );

        void End();

        [[nodiscard]] bool Active() const
        {
            return m_Active;
        }

        [[nodiscard]] uint32_t Control() const
        {
            return m_Control;
        }

        [[nodiscard]] ManipulatorMode Mode() const
        {
            return m_Mode;
        }

        /// The local pose the grab captured. Exposed so a caller can push ONE undo entry per completed
        /// drag from the value that was actually there, rather than from an address held across frames —
        /// which is the defect `LightGizmoRenderer::DragValueHandle` carries the note about.
        [[nodiscard]] const BoneTransform& PoseAtGrab() const
        {
            return m_PoseAtGrab;
        }

    private:
        bool            m_Active  = false;
        uint32_t        m_Control = ControlHierarchy::INVALID;
        ManipulatorMode m_Mode    = ManipulatorMode::Translate;

        /// THE ONLY TRANSFORM THIS CLASS KEEPS, and it is the local one. See the class note.
        BoneTransform m_PoseAtGrab;

        /// The drag plane, fixed at the grab. A SCREEN CONSTRUCT, not a transform: it is where the
        /// pointer's line is measured, never something written back into the rig. Fixed rather than
        /// re-derived per frame because re-deriving it through the control being dragged is a feedback
        /// loop — the plane would chase the thing whose position it is being used to compute.
        glm::vec3 m_PlanePoint  = glm::vec3( 0.0F );
        glm::vec3 m_PlaneNormal = glm::vec3( 0.0F, 0.0F, 1.0F );
        glm::vec3 m_HitAtGrab   = glm::vec3( 0.0F );

        glm::vec2 m_ArcballCenter = glm::vec2( 0.0F );
        float     m_ArcballRadius = 1.0F;
        glm::vec3 m_ArcAtGrab     = glm::vec3( 0.0F, 0.0F, 1.0F );
    };
} // namespace Desert::Animation
