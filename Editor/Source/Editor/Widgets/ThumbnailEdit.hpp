#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Engine/Assets/ThumbnailInfo.hpp>

#include <filesystem>
#include <string_view>

// EDIT THUMBNAIL (UE: the asset's context menu -> "Edit Thumbnail": the tile becomes interactive, a left drag
// orbits, the wheel zooms, and the result is the asset's ThumbnailInfo). Every way an orbit is edited - the
// browser tile, the palette - goes through EditOrbit, which writes the asset's ONE home and records the undo:
// a material's MaterialData::Thumbnail in its .demat, an imported mesh's entry in its import record
// (SetMeshThumbnailOrbit). The picture is re-shot by freshness alone (the .demat's bytes; the mesh's
// MeshThumbnailFreshness), not by a call from here.
namespace Desert::Editor::ThumbnailEdit
{
    /// The orbit @p asset (a .demat, or a static mesh's source or .stmesh as the browser lists it) states now.
    [[nodiscard]] Common::ResultStr<Assets::ThumbnailOrbit> ReadOrbit( const std::filesystem::path& asset );

    /// Writes @p orbit into @p asset's home with no undo record: what Undo/Redo call. EditOrbit for an edit.
    [[nodiscard]] Common::BoolResultStr WriteOrbit( const std::filesystem::path&  asset,
                                                    const Assets::ThumbnailOrbit& orbit );

    /// THE EDIT: @p orbit written into @p asset's home and one undo entry pushed (old -> new). Nothing is
    /// written or recorded when the orbit is the one stated already.
    [[nodiscard]] Common::BoolResultStr EditOrbit( const std::filesystem::path&  asset,
                                                   const Assets::ThumbnailOrbit& orbit );

    /// Degrees per dragged pixel, and the zoom one wheel notch moves (UE's thumbnail orbit feel).
    inline constexpr float kDegreesPerPixel = 0.5f;
    inline constexpr float kZoomPerNotch    = 0.1f;

    /// The orbit a drag of (@p dx, @p dy) pixels and @p wheel notches makes of @p from: yaw follows x, pitch y
    /// (clamped to +-89), yaw wrapped to (-180, 180], wheel forward zooms in (zoom kept above -0.9, the record
    /// refuses -1 and below).
    [[nodiscard]] Assets::ThumbnailOrbit Orbited( const Assets::ThumbnailOrbit& from, float dx, float dy,
                                                  float wheel );

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
    [[nodiscard]] std::string_view OrbitStepName( OrbitStep step );

    /// @p from moved by @p step: yaw +-45 (wrapped as a drag wraps it), pitch +-15 (clamped as a drag clamps it),
    /// zoom +-0.25 (kept above -0.9), Reset = the default orbit.
    [[nodiscard]] Assets::ThumbnailOrbit Stepped( const Assets::ThumbnailOrbit& from, OrbitStep step );

    /// The palette's command: @p asset's stated orbit moved by @p step, through EditOrbit (one write, one undo).
    [[nodiscard]] Common::BoolResultStr EditOrbitStep( const std::filesystem::path& asset, OrbitStep step );
} // namespace Desert::Editor::ThumbnailEdit
