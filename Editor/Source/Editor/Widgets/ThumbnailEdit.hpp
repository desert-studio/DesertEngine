#pragma once

#include <Common/Core/ResultStr.hpp>
#include <Editor/Widgets/ThumbnailOrbitEdit.hpp>
#include <Engine/Assets/ThumbnailInfo.hpp>

#include <filesystem>

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

    /// The palette's command: @p asset's stated orbit moved by @p step, through EditOrbit (one write, one undo).
    [[nodiscard]] Common::BoolResultStr EditOrbitStep( const std::filesystem::path& asset, OrbitStep step );
} // namespace Desert::Editor::ThumbnailEdit
