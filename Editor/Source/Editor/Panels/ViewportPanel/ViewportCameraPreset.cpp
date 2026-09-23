#include "ViewportCameraPreset.hpp"

// The header deliberately does NOT pull the camera in; this translation unit is where the full camera
// is needed and where it is paid for.
#include <Engine/Core/Camera.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace Desert::Editor
{
    void ApplyViewportCameraPreset( ::Desert::Core::EditorCamera& camera, ViewportCameraPreset preset )
    {
        const ViewportCameraAim aim = ViewportCameraAimOf( preset );

        // PROJECTION FIRST, THEN THE ANGLE. SetProjectionType rebuilds the matrix at the cached viewport
        // size and the snap rebuilds the view; doing it in this order means one frame carries both
        // halves, so a preset never shows as "ortho, still pointing the old way" for a frame.
        camera.SetProjectionType( aim.Orthographic ? ::Desert::Core::ProjectionType::Orthographic
                                                   : ::Desert::Core::ProjectionType::Perspective );

        // ── THE WHOLE POINT OF THIS FUNCTION IS WHICH OF THESE TWO LINES RUNS ─────────────────────
        //
        // An axis view is handed to the camera as a BASIS and is held exactly: forward and up both
        // named, so straight down is an ordinary value and the ±89° orbit clamp is never consulted. It
        // used to take the second line — `SnapToDirection( 0, -1, 0 )` — which writes pitch = ±90° into
        // the orbit, where OnUpdate clamped it back one frame later and left the Top and Bottom panes a
        // full degree off the axis they name.
        //
        // The dispatch is exhaustive over `aim.Axis` and the table decides which rows have one, so a
        // preset cannot be routed through the orbit by editing this file alone — Tests/Editor/
        // ViewportCameraPreset asserts that every orthographic row yields a basis.
        if ( aim.Axis )
            camera.SnapToAxisView( *aim.Axis );
        else
            camera.SnapToDirection( aim.OrbitForward );
    }

    std::optional<ViewportCameraPreset> PresetOfCamera( const ::Desert::Core::EditorCamera& camera,
                                                        float toleranceDegrees )
    {
        // ONE IMPLEMENTATION, IN THE HEADER, asked here with what the camera happens to be doing. The
        // comparison used to live in this function, where no suite could reach it without a window and a
        // device — and the tolerance it carried was wrong for two of the seven presets for exactly as
        // long as that was true.
        //
        // `GetDirection()` AND NOT `AxisView()`, deliberately. The question is what the camera is
        // POINTING AT, and asking the axis-view state instead would make the caption a report of how the
        // angle was reached rather than of where it is — two ways to be on Top, one of which the picture
        // cannot distinguish. The direction is exact while a basis is held (Camera.cpp, CurrentBasis), so
        // the exactness is carried by the camera and the tolerance is free to refuse the clamp's answer.
        return PresetOfDirection( camera.GetDirection(),
                                  camera.GetProjectionType() == ::Desert::Core::ProjectionType::Orthographic,
                                  toleranceDegrees );
    }

    const char* ViewportCameraPresetLabel( const ::Desert::Core::EditorCamera& camera )
    {
        if ( const auto preset = PresetOfCamera( camera ) )
            return ViewportCameraPresetRowOf( *preset ).Name;
        return "Ortho"; // orthographic and off every axis — true, and says the projection is still parallel
    }
} // namespace Desert::Editor
