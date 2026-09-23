#pragma once

// ── WHAT A FOUR-UP GRID ACTUALLY BUYS, AND HOW WE SPELL IT ─────────────────────────────────────────
//
// UE's answer to "I need to see this object from several sides at once" is a four-viewport layout
// widget with a per-viewport type menu (Perspective / Top / Front / Left / ...). Two halves:
//
//   * the ARRANGEMENT — four views on screen at the same time;
//   * the ANGLES — each of them locked to a named direction, with a parallel projection for the
//     three axis views so measurement reads true.
//
// WE TAKE THE SECOND HALF LITERALLY AND REFUSE THE FIRST. The arrangement is already ours: a scene
// holds N views (Engine/Core/SceneViewList.hpp) and each is an ordinary dockable window, so a 2x2
// grid is a DOCKING LAYOUT and not a widget. Building UE's fixed splitter here would be a second,
// weaker layout system beside the one the whole editor already uses — the four panes could not be
// tabbed, floated, resized against their neighbours, or saved as a named layout, all of which our
// docking gives for free. `EditorLayer::BuildViewportGrid` therefore opens the three extra views and
// asks DockBuilder for the quarters, which is the same outcome with nothing new to maintain.
//
// This file is the second half: the named angles, in ONE table that the toolbar menu, the command
// palette and the grid builder all read.
//
// THE PRESET IS NOT REMEMBERED, IT IS DERIVED. A stored "this viewport is the Top view" becomes a lie
// the first time the user orbits, and a label that lies about the camera is worse than no label —
// this is the shape the contract calls "a comment is not the code". `PresetOfCamera` asks the camera
// what it is doing right now and answers nullopt when it is not on any preset, so the toolbar can say
// "Ortho" or "Perspective" truthfully instead of "Top" falsely.

// NO <Engine/Core/Camera.hpp> HERE, ON PURPOSE. That header reaches Application.hpp and most of the
// engine with it, and the rule this file states — "which named angle is a direction on" — needs none of
// that. Forward declarations keep the rule reachable from a suite that links no graphics at all, which
// is how `Tests/Editor/ViewportCameraPreset` can assert it. The camera-facing entry points below are
// still declared here and still defined in the .cpp, where the full camera is available.
#include <Engine/Core/CameraPitchLimit.hpp>
#include <Engine/Core/EditorCameraBasis.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <optional>

namespace Desert::Core
{
    class EditorCamera;
    enum class ProjectionType : int;
} // namespace Desert::Core

namespace Desert::Editor
{
    enum class ViewportCameraPreset : size_t
    {
        Perspective = 0,
        Top,
        Bottom,
        Front,
        Back,
        Left,
        Right,
    };

    inline constexpr size_t kViewportCameraPresetCount =
         static_cast<size_t>( ViewportCameraPreset::Right ) + 1u;

    struct ViewportCameraPresetRow
    {
        ViewportCameraPreset Preset;
        const char*          Name;
        // The direction the camera LOOKS ALONG, in world space. Three floats and not a glm::vec3 so the
        // table is a constant expression on every toolchain we build with.
        float ForwardX, ForwardY, ForwardZ;
        // The six axis views are PARALLEL projections, which is the half of UE's pattern that carries the
        // meaning: an orthographic top view is a plan, a perspective one is just a high camera.
        bool Orthographic;
    };

    // Y is up and the default camera looks along -Z (Core::EditorCamera's m_Orientation), so "Front" is
    // the default direction and the rest follow from it. Dense and in enum order — asserted below.
    inline constexpr std::array<ViewportCameraPresetRow, kViewportCameraPresetCount> kViewportCameraPresets{ {
         { ViewportCameraPreset::Perspective, "Perspective", 0.0f, 0.0f, -1.0f, false },
         { ViewportCameraPreset::Top, "Top", 0.0f, -1.0f, 0.0f, true },
         { ViewportCameraPreset::Bottom, "Bottom", 0.0f, 1.0f, 0.0f, true },
         { ViewportCameraPreset::Front, "Front", 0.0f, 0.0f, -1.0f, true },
         { ViewportCameraPreset::Back, "Back", 0.0f, 0.0f, 1.0f, true },
         { ViewportCameraPreset::Left, "Left", 1.0f, 0.0f, 0.0f, true },
         { ViewportCameraPreset::Right, "Right", -1.0f, 0.0f, 0.0f, true },
    } };

    consteval bool ViewportCameraPresetTableIsDense()
    {
        for ( size_t i = 0; i < kViewportCameraPresets.size(); ++i )
            if ( static_cast<size_t>( kViewportCameraPresets[i].Preset ) != i )
                return false;
        return true;
    }
    static_assert( ViewportCameraPresetTableIsDense(), "kViewportCameraPresets must be in enum order" );

    [[nodiscard]] inline const ViewportCameraPresetRow& ViewportCameraPresetRowOf( ViewportCameraPreset p )
    {
        return kViewportCameraPresets[static_cast<size_t>( p )];
    }

    // ── WHAT A PRESET ASKS THE CAMERA FOR, AS A VALUE ─────────────────────────────────────────────
    //
    // The aim is separated from the aiming so that the DECISION is reachable: ViewportCameraPreset.cpp
    // needs the whole camera and therefore the whole engine, while this is three fields a suite can
    // compare. The relation it exists to pin is `EveryAxisPresetIsHeldExactly` — every orthographic row
    // yields an `Axis` basis, so no axis view can reach the orbit's ±89° clamp by accident.
    struct ViewportCameraAim
    {
        bool Orthographic = false;
        // Set for EVERY orthographic row and for no other. The exact basis, forward and up both named —
        // the orbit's yaw/pitch pair cannot express straight down without degenerating, which is the
        // defect this whole seam exists to close.
        std::optional<::Desert::Core::ViewBasis> Axis;
        // Where an ORBITING camera is pointed instead. Read only when `Axis` is empty, which today means
        // the Perspective row alone.
        glm::vec3 OrbitForward{ 0.0f, 0.0f, -1.0f };
    };

    [[nodiscard]] inline ViewportCameraAim ViewportCameraAimOf( ViewportCameraPreset preset )
    {
        const ViewportCameraPresetRow& row     = ViewportCameraPresetRowOf( preset );
        const glm::vec3                forward = glm::vec3( row.ForwardX, row.ForwardY, row.ForwardZ );

        ViewportCameraAim aim;
        aim.Orthographic = row.Orthographic;
        aim.OrbitForward = forward;
        // AN AXIS VIEW FOR THE PARALLEL ROWS AND ONLY THOSE. A perspective viewport orbits by definition
        // — it is the one preset that constrains no direction — so pinning its basis would take the
        // user's freedom to drag, which is the opposite of what it means.
        if ( row.Orthographic )
            aim.Axis = ::Desert::Core::AxisViewBasisOf( forward );
        return aim;
    }

    // Point @p camera at @p preset: put it on the named basis (or, for Perspective, on the named orbit
    // direction) and switch the projection. The framing distance and focal point are untouched — a preset
    // changes the ANGLE, never where the user was looking, which is what makes flipping between Top and
    // Front useful rather than disorienting.
    void ApplyViewportCameraPreset( ::Desert::Core::EditorCamera& camera, ViewportCameraPreset preset );

    // WHICH PRESET THIS CAMERA IS ON RIGHT NOW, or nullopt if it is on none (an orthographic camera the
    // user has since orbited off-axis). A perspective camera is always Perspective: that is what UE's
    // perspective viewport means, and it is the one preset that constrains no direction.
    //
    // HOW FAR OFF-AXIS STILL COUNTS, AND THE NUMBER'S JOB IS NOW THE OPPOSITE OF WHAT IT WAS.
    //
    // It used to have to ADMIT an error. `EditorCamera::OnUpdate` clamps pitch to ±89° on every frame
    // (Engine/Core/CameraPitchLimit.hpp) because glm::lookAt degenerates when the forward direction is
    // parallel to the up vector, so a camera put on Top settled one full degree off straight down — and a
    // tolerance that did not reach that far made the two views a four-up grid exists for the only two that
    // could never name themselves. The margin was therefore sized as the clamp's miss plus slack.
    //
    // THE TILT IS GONE AT THE SOURCE NOW. An axis preset is held as a BASIS and never passes through the
    // orbit clamp at all (`ViewportCameraAimOf` above, `EditorCamera::SnapToAxisView`), so Top means
    // exactly (0,-1,0). What is left for this number to do is REFUSE the old answer: sized at HALF the
    // clamp's own miss, so a camera that reached an axis through the orbit path cannot be named after it
    // and the caption says "Ortho" — which is true of a plan view that is one degree off, and is the
    // signal that the preset went the wrong way. Still an expression over the clamp and not a literal:
    // two numbers that agree today are what produced the original defect.
    inline constexpr float kPresetToleranceDegrees = ::Desert::Core::kCameraPitchClampMissDegrees * 0.5f;

    // THE RULE ITSELF, over a direction rather than a camera, so it can be asked a question without a
    // window, a device or an Input singleton. @p orthographic is the camera's projection: a perspective
    // camera is always Perspective, which is what UE's perspective viewport means and the one preset
    // that constrains no direction.
    [[nodiscard]] inline std::optional<ViewportCameraPreset>
    PresetOfDirection( const glm::vec3& forward, bool orthographic,
                       float toleranceDegrees = kPresetToleranceDegrees )
    {
        if ( !orthographic )
            return ViewportCameraPreset::Perspective;
        if ( glm::length( forward ) < 1e-5f )
            return std::nullopt;
        const glm::vec3 f = glm::normalize( forward );

        // Compared by the ANGLE between the two directions, not component by component: a per-component
        // epsilon has a different meaning near an axis than away from one, and the caller's tolerance is
        // stated in degrees because that is the unit a person can reason about.
        const float cosLimit = glm::cos( glm::radians( toleranceDegrees ) );
        for ( const ViewportCameraPresetRow& row : kViewportCameraPresets )
        {
            if ( !row.Orthographic )
                continue; // Perspective is answered above and constrains no direction
            if ( glm::dot( f, glm::vec3( row.ForwardX, row.ForwardY, row.ForwardZ ) ) >= cosLimit )
                return row.Preset;
        }
        return std::nullopt;
    }

    // The same question asked of a live camera.
    [[nodiscard]] std::optional<ViewportCameraPreset>
    PresetOfCamera( const ::Desert::Core::EditorCamera& camera, float toleranceDegrees = kPresetToleranceDegrees );

    // What the toolbar prints when PresetOfCamera finds nothing: the projection, which is still true.
    [[nodiscard]] const char* ViewportCameraPresetLabel( const ::Desert::Core::EditorCamera& camera );
} // namespace Desert::Editor
