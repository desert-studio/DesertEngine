#pragma once

#include <Engine/Assets/ThumbnailInfo.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

// THE ORBIT MATHS OF EDIT THUMBNAIL, pure: what a drag, a wheel or a palette step makes of an orbit. Header-only
// so the ThumbnailEdit suite asserts it without the undo history, the import record or a device; the edit
// itself (one write into the asset's home, one undo entry) is ThumbnailEdit::EditOrbit.
namespace Desert::Editor::ThumbnailEdit
{
    /// Degrees per dragged pixel, and the zoom one wheel notch moves (UE's thumbnail orbit feel).
    inline constexpr float kDegreesPerPixel = 0.5f;
    inline constexpr float kZoomPerNotch    = 0.1f;
    /// The pitch a drag stops at, and the zoom it keeps above (the record refuses -1 and below).
    inline constexpr float kMaxPitch = 89.0f;
    inline constexpr float kMinZoom  = -0.9f;

    /// The orbit a drag of (@p dx, @p dy) pixels and @p wheel notches makes of @p from: yaw follows x, pitch y
    /// (clamped to +-kMaxPitch), yaw wrapped to (-180, 180], wheel forward zooms in (kept above kMinZoom).
    [[nodiscard]] inline Assets::ThumbnailOrbit Orbited( const Assets::ThumbnailOrbit& from, float dx, float dy,
                                                         float wheel )
    {
        Assets::ThumbnailOrbit next = from;
        float                  yaw  = std::fmod( from.Yaw + dx * kDegreesPerPixel, 360.0f );
        if ( yaw > 180.0f )
            yaw -= 360.0f;
        else if ( yaw <= -180.0f )
            yaw += 360.0f;
        next.Yaw   = yaw;
        next.Pitch = std::clamp( from.Pitch + dy * kDegreesPerPixel, -kMaxPitch, kMaxPitch );
        // Zoom is a fraction of the fitted distance: forward (positive) notches bring the camera in.
        next.Zoom = std::max( from.Zoom - wheel * kZoomPerNotch, kMinZoom );
        return next;
    }

    /// THE PALETTE'S STEPS. The palette addresses a command by its exact name (ResolveCommand), so an orbit is
    /// reached there in fixed steps rather than typed numbers; the tile's drag is the free form of the same edit.
    enum class OrbitStep
    {
        YawPlus,
        YawMinus,
        PitchPlus,
        PitchMinus,
        ZoomOut,
        ZoomIn,
        Reset,
    };

    inline constexpr OrbitStep kOrbitSteps[] = { OrbitStep::YawPlus,    OrbitStep::YawMinus, OrbitStep::PitchPlus,
                                                 OrbitStep::PitchMinus, OrbitStep::ZoomOut,  OrbitStep::ZoomIn,
                                                 OrbitStep::Reset };

    /// The step's name as the palette shows it after "Edit Thumbnail: <file> ".
    [[nodiscard]] inline std::string_view OrbitStepName( OrbitStep step )
    {
        switch ( step )
        {
            case OrbitStep::YawPlus:
                return "yaw +45";
            case OrbitStep::YawMinus:
                return "yaw -45";
            case OrbitStep::PitchPlus:
                return "pitch +15";
            case OrbitStep::PitchMinus:
                return "pitch -15";
            case OrbitStep::ZoomOut:
                return "zoom +0.25";
            case OrbitStep::ZoomIn:
                return "zoom -0.25";
            case OrbitStep::Reset:
                return "reset";
        }
        return "unknown step";
    }

    /// @p from moved by @p step: yaw +-45 (wrapped as a drag wraps it), pitch +-15 (clamped as a drag clamps it),
    /// zoom +-0.25 (kept above kMinZoom), Reset = the default orbit. Degrees and notches go through Orbited, so
    /// a step obeys exactly the wrap and clamps a drag obeys.
    [[nodiscard]] inline Assets::ThumbnailOrbit Stepped( const Assets::ThumbnailOrbit& from, OrbitStep step )
    {
        constexpr float kPixelsPerDegree = 1.0f / kDegreesPerPixel;
        constexpr float kNotchesPerZoom  = 1.0f / kZoomPerNotch;
        switch ( step )
        {
            case OrbitStep::YawPlus:
                return Orbited( from, 45.0f * kPixelsPerDegree, 0.0f, 0.0f );
            case OrbitStep::YawMinus:
                return Orbited( from, -45.0f * kPixelsPerDegree, 0.0f, 0.0f );
            case OrbitStep::PitchPlus:
                return Orbited( from, 0.0f, 15.0f * kPixelsPerDegree, 0.0f );
            case OrbitStep::PitchMinus:
                return Orbited( from, 0.0f, -15.0f * kPixelsPerDegree, 0.0f );
            case OrbitStep::ZoomOut:
                return Orbited( from, 0.0f, 0.0f, -0.25f * kNotchesPerZoom );
            case OrbitStep::ZoomIn:
                return Orbited( from, 0.0f, 0.0f, 0.25f * kNotchesPerZoom );
            case OrbitStep::Reset:
                return Assets::ThumbnailOrbit{};
        }
        return from;
    }
} // namespace Desert::Editor::ThumbnailEdit
