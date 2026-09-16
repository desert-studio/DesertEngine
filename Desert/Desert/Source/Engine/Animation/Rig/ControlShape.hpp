#pragma once

/**
 * WHAT A CONTROL LOOKS LIKE, KEPT OUT OF WHAT A CONTROL IS.
 *
 * T5.2, first half. `ControlElement::ShapeName` is a NAME (report 01 §(a)6) and `ControlHierarchy`
 * deliberately does not know what it draws. This is the other side of that seam: a name -> wireframe
 * geometry table, with no dependency on the hierarchy, on ImGui, or on the GPU.
 *
 * ── WIREFRAME AND NOT A MESH, BECAUSE THERE IS NO PASS TO PUT A MESH IN ──────────────────────────────
 *
 * Verified 2026-09-16 and it is still true: `grep -rln "DebugRenderer\|LineRenderer\|DrawLine\|DebugDraw\|
 * DrawWireframe"` over `Engine/Graphic/` returns nothing, and `Engine/Graphic/Systems/Scene/` holds only
 * Clouds, Deferred, Fog, Mesh, Particles, PostProcessing, Skybox, Terrain (06 §3.16). Report 01 §833
 * looks at UE's answer to this — a transient `AActor` + `UPrimitiveComponent` + `UMaterialInstanceDynamic`
 * per control, plus a "shape actor got detached, re-attach it" repair — and says an immediate-mode
 * manipulator layer is cheaper. So a shape here is a LIST OF LINE SEGMENTS, which the existing viewport
 * overlay can already draw, and no render pass is added by this tier.
 *
 * ── SIZE IS NEVER THE CONTROL'S SCALE, AND THE THIRD TERM NOW EXISTS ────────────────────────────────
 *
 * Every built-in is authored at unit size around its own origin. That is a size of ONE CENTIMETRE, since
 * 1 world unit = 1 cm, so on a 260 cm character an unsized control is a mark a pixel wide that an
 * animator can neither see nor grab.
 *
 * MEASURED, AND THE OBVIOUS FIX IS THE WRONG ONE. A control's global is `parent * offset * pose`, so
 * sizing it through the SCALE of its `Offset` multiplies the POSE's translation too: a control sized 8x
 * travelled 8 world units per unit of animated translation, and the drawn shape's centroid moved 182.9
 * units where the animator had asked for 26.4. The offset's scale is not a size knob — it is a change to
 * what a pose unit MEANS, and one that a clip authored before the resize would silently reinterpret.
 *
 * UE avoids the same coupling with a shape transform that is NOT the offset
 * (`LibraryShapeTransform * ControlShapeTransform * ControlGlobalTransform`, report 01 §(a)6). THIS FILE
 * USED TO SAY WE HAD THE FIRST AND THIRD TERMS AND NOT THE MIDDLE ONE, "because there is nowhere to
 * author it until the rig is an asset (T5.4)". That premise died with `.derig`: a rig is a file now, so
 * `ControlElement::ShapeTransform` is a per-control field the file carries and `BuildFrame` composes.
 * The entry transform is still load-bearing and still not a size knob — one circle serves `CircleXY`,
 * `CircleXZ` and `CircleYZ` through it, which is ORIENTATION shared by every control naming that entry,
 * while the size a rigger chooses for one control belongs to that control.
 */

#include <Common/Core/ResultStr.hpp>

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace Desert::Animation
{
    /**
     * @brief One run of points. `Closed` adds the last->first segment.
     *
     * A RUN AND NOT A SEGMENT SOUP because the closing segment is the thing that gets forgotten: a circle
     * authored as 32 points and drawn as 31 segments has a gap that is invisible at a small radius and
     * obvious at a large one. Saying "closed" once is cheaper than remembering to repeat the first point.
     */
    struct ControlShapePolyline
    {
        std::vector<glm::vec3> Points;
        bool                   Closed = true;
    };

    /// The geometry behind one name, in the shape's own space, with the library's authoring transform.
    struct ControlShape
    {
        std::vector<ControlShapePolyline> Polylines;
        glm::mat4                         Transform = glm::mat4( 1.0F );
    };

    /**
     * @brief Name -> shape, and a refusal when the name is not here.
     *
     * `Find` ANSWERS `nullptr` AND THE CALLER MUST SAY SO. A manipulator that quietly drew nothing for an
     * unknown shape name would make a typo in a rig look exactly like a control the rigger chose not to
     * draw, and the animator's only symptom would be a control they cannot grab. The frame builder
     * therefore collects the unresolved names rather than skipping them silently.
     */
    class ControlShapeLibrary
    {
    public:
        /**
         * @brief The shapes the engine ships.
         *
         * Six names over three geometries: a 32-gon in three planes, a unit cube, a unit octahedron, and
         * the three circles together as a sphere. Chosen because they are what a control rig actually
         * uses (UE's own defaults are the same family) and stopped there — a seventh shape is content,
         * and content belongs in a rig asset, not in a header.
         *
         * ANSWERS A RESULT RATHER THAN A LIBRARY, and the reason is that the alternative was six
         * discarded `Add` results. A built-in table whose registration is thrown away is the "empty
         * successful answer" the contract forbids: edit one entry into a degenerate run and the engine
         * ships a library that is quietly one shape short, with the first symptom a control an animator
         * cannot grab. The suite unwraps this, so the table is gated at build time.
         */
        [[nodiscard]] static Common::ResultStr<ControlShapeLibrary> BuiltIn();

        /// Refuses an empty name, a duplicate, a shape with no runs, a run shorter than two points, and a
        /// non-finite point or transform. A degenerate shape is a control that cannot be hit.
        [[nodiscard]] Common::BoolResultStr Add( std::string name, ControlShape shape );

        /// The shape behind @p name, or nullptr. See the class note on why nullptr is not "draw nothing".
        [[nodiscard]] const ControlShape* Find( const std::string& name ) const;

        [[nodiscard]] size_t Size() const
        {
            return m_Entries.size();
        }

        [[nodiscard]] std::vector<std::string> Names() const;

    private:
        struct Entry
        {
            std::string  Name;
            ControlShape Shape;
        };

        std::vector<Entry> m_Entries;
    };
} // namespace Desert::Animation
