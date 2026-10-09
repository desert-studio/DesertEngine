#pragma once

#include <cstddef>
#include <vector>

namespace Desert::Editor
{
    struct DirectoryInformation;

    /// WHAT THE ASSET VIEW SHOWS AND HOW (UE: SAssetView's filter text, sort manager, thumbnail size and view
    /// type). One value, owned by the panel: the toolbar edits it, the asset view and the selection's shift-range
    /// read the same order from it, so the tiles on screen and the range a shift-click selects never disagree.
    struct AssetViewState
    {
        enum class SortMode
        {
            Name = 0,
            DateModified,
            Type,
            Size
        };

        static constexpr float kMinGridSize = 40.0f;
        static constexpr float kMaxGridSize = 400.0f;

        char     SearchBuf[128] = { 0 }; // name filter, case-insensitive substring of the filename
        SortMode Sort           = SortMode::Name;
        bool     SortDescending = false;
        int      TypeFilter     = -1;     // FileType value to show, or -1 for "All" (folders always shown)
        float    GridSize       = 120.0f; // tile width in the grid view
        bool     ListView       = false;  // list rows instead of tiles
    };

    /// The children of @p dir the view shows, in the order it shows them: hidden entries dropped unless
    /// @p showHidden, the type filter and the name search applied, folders first, then by @p state's sort key
    /// with the case-insensitive filename as the tiebreak. Empty for a null @p dir.
    std::vector<std::size_t> BuildDisplayOrder( const DirectoryInformation* dir, const AssetViewState& state,
                                                bool showHidden );
} // namespace Desert::Editor
