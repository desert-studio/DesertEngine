#pragma once

#include <functional>

namespace Desert::Editor
{
    struct AssetViewState;
    struct DirectoryInformation;

    /// THE CONTENT BROWSER'S BODY (UE: SAssetView): the open folder's entries in AssetViewState's order, as a
    /// grid of tiles or a list of rows, only those on screen built. The tile itself is the panel's (thumbnail,
    /// selection, item menu, drag source — UE's SAssetTileItem with the thumbnail pool behind it), reached through
    /// OnDrawTile; so is the background menu's content. A click on no tile clears the selection.
    class ContentBrowserAssetView
    {
    public:
        /// What drawing one tile reported.
        struct TileResult
        {
            bool DoubleClicked = false; // the entry was activated (a folder opens; the listing is stale)
            bool Hovered       = false; // the cursor is over the tile's card / row
        };

        struct Delegates
        {
            /// Draws child @p dirIndex of the open folder at display position @p shownIndex, as a tile when
            /// @p gridView, as a row otherwise.
            std::function<TileResult( int dirIndex, bool folder, int shownIndex, bool gridView )> OnDrawTile;
            /// Whether an entry is selected: a selected tile is drawn even off screen (keyboard navigation).
            std::function<bool( const DirectoryInformation* )> IsSelected;
            /// The items of the right-click menu on the body's background (the grid-size slider follows them).
            std::function<void()> OnBackgroundContextMenu;
            /// A left click in the body that hit no tile.
            std::function<void()> OnClearSelection;
        };

        explicit ContentBrowserAssetView( Delegates delegates );

        /// The body's child window, filling the rest of the pane.
        void Draw( const DirectoryInformation& current, AssetViewState& state, bool showHidden );

    private:
        Delegates m_On;
        // Height of one grid tile / list row as last drawn: an off-screen one is a Dummy of this size.
        float m_CellHeight[2] = { 0.0f, 0.0f };
    };
} // namespace Desert::Editor
