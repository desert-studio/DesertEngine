#include "ViewportCameraPreset.hpp"

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
        camera.SnapToDirection( ForwardOf( row ) );
    }

    std::optional<ViewportCameraPreset> PresetOfCamera( const ::Desert::Core::EditorCamera& camera,
                                                        float toleranceDegrees )
    {
        if ( camera.GetProjectionType() == ::Desert::Core::ProjectionType::Perspective )
            return ViewportCameraPreset::Perspective;

        const glm::vec3 forward = camera.GetDirection();
        if ( glm::length( forward ) < 1e-5f )
            return std::nullopt;
        const glm::vec3 f = glm::normalize( forward );

        // Compared by the ANGLE between the two directions, not component by component: a per-component
        // epsilon has a different meaning near an axis than away from one, and the caller's tolerance is
        // stated in degrees because that is the unit a person can reason about.
        const float cosLimit = std::cos( glm::radians( toleranceDegrees ) );
        for ( const ViewportCameraPresetRow& row : kViewportCameraPresets )
        {
            if ( !row.Orthographic )
                continue; // Perspective is answered above and constrains no direction
            if ( glm::dot( f, ForwardOf( row ) ) >= cosLimit )
                return row.Preset;
        }
        return std::nullopt;
    }

    const char* ViewportCameraPresetLabel( const ::Desert::Core::EditorCamera& camera )
    {
        if ( const auto preset = PresetOfCamera( camera ) )
            return ViewportCameraPresetRowOf( *preset ).Name;
        return "Ortho"; // orthographic and off every axis — true, and says the projection is still parallel
    }
} // namespace Desert::Editor
