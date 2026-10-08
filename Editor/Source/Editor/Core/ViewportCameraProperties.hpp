#pragma once

#include <Editor/Core/EditableProperty.hpp>

#include <Common/Core/Math/AABB.hpp>
#include <Common/Core/ResultStr.hpp>

#include <Engine/Core/Projection.hpp>

#include <glm/glm.hpp>

#include <cmath>

#include <string>
#include <vector>

namespace Desert::Editor
{
    /**
     * @brief THE VIEWPORT CAMERA AS A SET OF EDITABLE VALUES — the channel's second subject.
     *
     * ── WHY THE CAMERA IS A `set` AND NOT A PALETTE COMMAND ──────────────────────────────────────────
     *
     * Г14 established that the channel's vocabulary IS the command palette's list: a capability reachable
     * only by hand does not exist for the channel. That rule is about ACTIONS. The control protocol has
     * always had a SECOND category beside them, and it exists for exactly this kind of thing — its own
     * header says so: "`properties`/`set` reach the focused document's own values. That is everything a
     * person does by DRAGGING something, and it does not fit in a dictionary of actions: a slider has no
     * name, and giving it one would mean a palette entry per value."
     *
     * A camera pose is dragged, not chosen. Six continuous numbers cannot be a palette label without
     * becoming one entry per position, which is a serialised argument list wearing a dictionary's clothes.
     * So the camera joins the category that already carries values — and the change is a second SUBJECT
     * for `properties`/`set`, not a second mechanism: the same request, the same census type, the same
     * refusals, the same JSON writer.
     *
     * ── WHAT THIS REPLACES, AND WHY IT IS NOT A SMALL THING ──────────────────────────────────────────
     *
     * Placing the editor camera was wired to `--camera` / `--look`, and those are read ONLY inside
     * `shot.Active()`. So a developer who needed the camera somewhere specific — with no intention of
     * taking a `--shot` at all, because the control channel takes the pictures now — had to launch the
     * editor with a fictitious `--shot --shot-frames 1000000` to unlock the placement. The mandatory step
     * of a proof was being performed by the flag family the channel was built to replace. A capture flag
     * must not be the switch for something that is not capture (A6-1 point 5).
     *
     * ── AND IT LANDS WHERE A PERSON'S HANDS LAND ─────────────────────────────────────────────────────
     *
     * The write goes through EditorCamera::SnapToDirection + Focus, which are the editor's own view-axis
     * gizmo (click Front / Top / Right) and its F-focus. One placement function serves the shot flag and
     * the channel alike, so there is one route into the camera and two ways to reach it — the rule the
     * protocol states for `set` and the reason this is not a private back door into the view.
     *
     * Nothing here touches a camera, a scene or ImGui: names in, a census or a refusal out. That is what
     * makes the addressing and the arithmetic assertable, which they have to be — EditorLayer.cpp is
     * compiled by no suite at all (scripts/CI/UnreachedSources.sh).
     */

    /// How far in front of the camera the focal point is placed. `Focus` moves the focal point to a world
    /// point and backs the camera off along the CURRENT view direction by this distance, so aiming one
    /// framing distance ahead lands the camera exactly on the position asked for.
    ///
    /// A NAMED CONSTANT because the same number has to be used by both halves of that sentence: the
    /// distance the aim point is computed with and the distance `Focus` backs off by. Two spellings of it
    /// would put the camera somewhere else and nothing would say so — it was a bare `500.0f` twice on one
    /// line in the capture path.
    inline constexpr float kViewportCameraFramingDistance = 500.0f;

    /// The two properties, named once. `set` addresses by these strings, so they are the wire.
    inline constexpr const char* kViewportCameraPosition  = "Camera.Position";
    inline constexpr const char* kViewportCameraDirection = "Camera.Direction";

    // THE SUBJECT'S WIRE NAME IS NOT SPELLED HERE, deliberately. It lives once, in the protocol's own
    // kSubjects table (Control/ControlProtocol.hpp), which is what the parser matches against and what a
    // refusal lists. A copy here would be a second spelling of one string — and the refusals below would
    // have gone on quoting it after somebody renamed the real one, telling a client to send a subject the
    // parser no longer accepts. This header does not need to know how it was addressed; the client that
    // reached it does.

    /// Where to aim so the camera ends up AT @p position looking along @p forward.
    ///
    /// This is the whole arithmetic of the placement and it is separated for one reason: "the camera lands
    /// exactly where it was asked" is a RELATION between the aim point and the framing distance, and a
    /// relation is what this project's defects have always been. @p forward need not be normalized.
    [[nodiscard]] inline glm::vec3 ViewportCameraFocalPoint( const glm::vec3& position, const glm::vec3& forward,
                                                             float distance = kViewportCameraFramingDistance )
    {
        return position + glm::normalize( forward ) * distance;
    }

    /// The camera's pose as the census `properties` answers with.
    ///
    /// BOTH ROWS ARE SETTABLE and both carry their current value, because this census is what a client
    /// reads the picture against: a report that says "the camera was at (0, 200, 0) looking along
    /// (0, 0.9, -1)" and a capture beside it are two halves of one claim, and the number has to come from
    /// the camera rather than from what the client believes it asked for.
    ///
    /// NO Min/Max. A world position has no clamp in an engine whose unit is the centimetre, and inventing
    /// one here would refuse a legitimate placement on the authority of a number nobody chose.
    [[nodiscard]] inline std::vector<EditableProperty> DescribeViewportCamera( const glm::vec3& position,
                                                                               const glm::vec3& forward )
    {
        std::vector<EditableProperty> census;

        EditableProperty positionRow;
        positionRow.Name       = kViewportCameraPosition;
        positionRow.Label      = "Position";
        positionRow.Group      = "Camera";
        positionRow.Type       = "float3";
        positionRow.Components = 3;
        positionRow.Value      = { position.x, position.y, position.z, 0.0f };
        census.push_back( positionRow );

        EditableProperty directionRow;
        directionRow.Name       = kViewportCameraDirection;
        directionRow.Label      = "Direction";
        directionRow.Group      = "Camera";
        directionRow.Type       = "float3";
        directionRow.Components = 3;
        directionRow.Value      = { forward.x, forward.y, forward.z, 0.0f };
        census.push_back( directionRow );

        return census;
    }

    /// Which of the two a write names, once it is known to be valid.
    enum class ViewportCameraWrite
    {
        Position,
        Direction,
    };

    /**
     * @brief Is @p property one of the camera's, written with the right number of components?
     *
     * EVERY REFUSAL NAMES WHAT WOULD HAVE FIXED IT, because the alternative for each is a silent wrong
     * answer this project has already paid for:
     *
     *   - an unknown name would otherwise write nothing and report success;
     *   - the wrong component count would otherwise be padded, and a client that meant a different
     *     property would see its two numbers become a position;
     *   - a ZERO direction cannot be normalized, and normalizing it yields NaN — which reaches the camera,
     *     then the view matrix, then every pass, and shows up as a black frame nothing explains.
     */
    [[nodiscard]] inline Common::ResultStr<ViewportCameraWrite>
    ValidateViewportCameraWrite( const std::string& property, const std::vector<float>& value )
    {
        const bool isPosition  = ( property == kViewportCameraPosition );
        const bool isDirection = ( property == kViewportCameraDirection );

        if ( !isPosition && !isDirection )
        {
            return Common::MakeFormattedError<ViewportCameraWrite>(
                 "'{}' is not a property of the viewport. It offers '{}' and '{}'; ask 'properties' for the "
                 "same subject to see their current values.",
                 property, kViewportCameraPosition, kViewportCameraDirection );
        }

        if ( value.size() != 3 )
        {
            return Common::MakeFormattedError<ViewportCameraWrite>(
                 "'{}' takes three numbers and was given {}. A world position and a look direction are both "
                 "three components; padding the missing ones would place the camera somewhere nobody asked "
                 "for and report it as done.",
                 property, value.size() );
        }

        if ( isDirection )
        {
            const glm::vec3 forward( value[0], value[1], value[2] );
            if ( glm::dot( forward, forward ) <= 0.0f )
            {
                return Common::MakeError<ViewportCameraWrite>(
                     "'Camera.Direction' was given (0, 0, 0), which is not a direction. Normalizing it "
                     "produces NaN, and a NaN view matrix renders a black frame that looks like a broken "
                     "renderer rather than a bad request." );
            }
        }

        return Common::MakeSuccess( isPosition ? ViewportCameraWrite::Position : ViewportCameraWrite::Direction );
    }
    /// The smallest radius a framed box is given. A box of one point (a single-vertex mesh, or every mesh
    /// collapsed onto one spot) has no extent, and a zero radius puts the camera ON the point it frames.
    /// One millimetre, not UE's 15 (MinimumFocusRadius): a 1 cm scene is still a scene to frame, and a
    /// 15 cm floor would shrink it to a fifteenth of the picture.
    inline constexpr float kMinimumFramedRadius = 0.1f;

    /// A pose that holds a whole box in view, with the depth range it needs.
    struct FramedView
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Forward{ 0.0f, 0.0f, -1.0f }; // normalized
        float     Near = ::Desert::Core::kDefaultNearPlane;
        float     Far  = ::Desert::Core::kDefaultFarPlane;
    };

    /**
     * @brief FRAME @p box LOOKING ALONG @p forward — the editor's F-focus on a box (UE FocusViewportOnBox).
     *
     * Port of UE EditorViewportClient.cpp:955-1000: the box's bounding sphere (centre, |extent|), the
     * camera backed off along the view direction from the centre. Two departures, both because UE's
     * version is not what its own comment claims ("fitting the sphere into the viewport completely"):
     *
     *   - DISTANCE = R / sin(half-angle), not R / tan. A sphere is inside a cone of half-angle a exactly
     *     when its centre is at least R / sin(a) away; R / tan leaves the near side of the sphere outside
     *     the frustum (a cube seen along a diagonal pokes out of the picture by most of its corner).
     *   - THE NARROWER HALF-ANGLE, computed from @p fovXDegrees (the editor's FOV is HORIZONTAL,
     *     Core::kEditorViewportFovXDegrees) and @p aspect. UE multiplies R by the aspect for wide views,
     *     which is the same choice spelled through the tangent and wrong in the same way.
     *
     * THE DEPTH RANGE FOLLOWS THE BOX, in one direction only. The defaults (10 cm .. 50 km) are kept for
     * every box they already hold; a 1 cm scene pulls the near plane in front of itself, a 100 km one
     * pushes the far plane past itself. Without that, framing would put a small scene behind the near
     * plane and a large one beyond the far plane — "framed" and invisible.
     *
     * Pure: no camera, no scene. @p forward need not be normalized and must not be zero; @p aspect > 0.
     */
    [[nodiscard]] inline FramedView FrameBox( const ::Common::Math::AABB& box, const glm::vec3& forward,
                                              float fovXDegrees, float aspect )
    {
        const glm::vec3 centre = 0.5f * ( box.Min + box.Max );
        const float     radius = glm::max( glm::length( 0.5f * ( box.Max - box.Min ) ), kMinimumFramedRadius );

        const float halfX = 0.5f * glm::radians( fovXDegrees );
        const float halfY =
             0.5f * ::Desert::Core::VerticalFovKeepingHorizontal( glm::radians( fovXDegrees ), aspect );
        const float halfNarrow = glm::min( halfX, halfY );
        const float distance   = radius / std::sin( halfNarrow );

        FramedView view;
        view.Forward  = glm::normalize( forward );
        view.Position = centre - view.Forward * distance;
        view.Near     = glm::min( ::Desert::Core::kDefaultNearPlane, 0.5f * ( distance - radius ) );
        view.Far      = glm::max( ::Desert::Core::kDefaultFarPlane, 1.01f * ( distance + radius ) );
        return view;
    }
} // namespace Desert::Editor
