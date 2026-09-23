#include "ViewportCameraPreset.hpp"

// The header deliberately does NOT pull the camera in; this translation unit is where the full camera
// is needed and where it is paid for.
#include <Engine/Core/Camera.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <cmath>

namespace Desert::Editor
{
    namespace
    {
        glm::vec3 ForwardOf( const ViewportCameraPresetRow& row )
        {
            return glm::vec3( row.ForwardX, row.ForwardY, row.ForwardZ );
        }
    } // namespace

    void ApplyViewportCameraPreset( ::Desert::Core::EditorCamera& camera, ViewportCameraPreset preset )
    {
        const ViewportCameraPresetRow& row = ViewportCameraPresetRowOf( preset );

        // PROJECTION FIRST, THEN THE ANGLE. SetProjectionType rebuilds the matrix at the cached viewport
        // size and SnapToDirection rebuilds the view; doing it in this order means one frame carries both
        // halves, so a preset never shows as "ortho, still pointing the old way" for a frame.
        camera.SetProjectionType( row.Orthographic ? ::Desert::Core::ProjectionType::Orthographic
                                                   : ::Desert::Core::ProjectionType::Perspective );
        // WHAT IS ASKED FOR AND WHAT IS HELD ARE NOT THE SAME THING FOR TOP AND BOTTOM. SnapToDirection
        // writes pitch = ±90°, and the camera's own OnUpdate clamps it back to ±89° on the very next
        // frame (Camera.cpp, kMaxPitch) — so those two views settle one degree off the axis they name.
        // PresetOfCamera's tolerance is sized from that clamp, and the header says why it is not fixed
        // by widening the clamp.
        camera.SnapToDirection( ForwardOf( row ) );
    }

    std::optional<ViewportCameraPreset> PresetOfCamera( const ::Desert::Core::EditorCamera& camera,
                                                        float toleranceDegrees )
    {
        // ONE IMPLEMENTATION, IN THE HEADER, asked here with what the camera happens to be doing. The
        // comparison used to live in this function, where no suite could reach it without a window and a
        // device — and the tolerance it carried was wrong for two of the seven presets for exactly as
        // long as that was true.
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
