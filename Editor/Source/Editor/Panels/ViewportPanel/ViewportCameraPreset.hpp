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

    // Point @p camera at @p preset: snap the orbit onto the named direction and switch the projection.
    // The framing distance and focal point are untouched — a preset changes the ANGLE, never where the
    // user was looking, which is what makes flipping between Top and Front useful rather than
    // disorienting.
    void ApplyViewportCameraPreset( ::Desert::Core::EditorCamera& camera, ViewportCameraPreset preset );

    // WHICH PRESET THIS CAMERA IS ON RIGHT NOW, or nullopt if it is on none (an orthographic camera the
    // user has since orbited off-axis). A perspective camera is always Perspective: that is what UE's
    // perspective viewport means, and it is the one preset that constrains no direction.
    //
    // HOW FAR OFF-AXIS STILL COUNTS, and the number is NOT a taste — it is DERIVED from the camera,
    // which cannot hold Top or Bottom at all. `EditorCamera::OnUpdate` clamps pitch to ±89° on every
    // frame (Engine/Core/CameraPitchLimit.hpp) because glm::lookAt degenerates when the forward
    // direction is parallel to the up vector, so a camera that has just been put on Top settles ONE FULL
    // DEGREE off straight-down and stays there.
    //
    // A hand-written 0.5° stood here, and it meant the two views a four-up grid exists for were the only
    // two that could never name themselves: both said "Ortho" from the moment the grid opened, which is
    // precisely the failure the header of this file claims to be avoiding. The margin is the clamp's own
    // miss plus half a degree of slack, spelled as an expression over the clamp so widening one moves the
    // other — a second literal agreeing with the first is what produced the defect.
    //
    // THE RESIDUAL TILT IS REAL AND IS NOT FIXED HERE. A plan view one degree off is still not a plan;
    // making the camera hold ±90° means giving UpdateCameraView a second up vector at the poles, which
    // changes orbiting in every viewport in the editor and is not this panel's call to make.
    inline constexpr float kPresetToleranceDegrees = ::Desert::Core::kCameraPitchClampMissDegrees + 0.5f;

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
