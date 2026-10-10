#include "ContentBrowserAssetView.hpp"

#include <Editor/Panels/FileExplorer/AssetViewState.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    ContentBrowserAssetView::ContentBrowserAssetView( Delegates delegates ) : m_On( std::move( delegates ) )
    {
    }

    void ContentBrowserAssetView::Draw( const DirectoryInformation& current, AssetViewState& state,
                                        bool showHidden )
    {
        // The grid takes the whole body: asset details live in the hover tooltip, not in a strip that a
        // selection carves out of the panel.
        ImGui::BeginChild( "##assetBodyRegion", ImVec2( 0.0f, 0.0f ), false );

        int shownIndex = 0;

        // Column stride MUST match the cell the tile actually draws (GridSize wide: centered icon + wrapped
        // label + the ~6px card padding). Card outsets around the tile (±2px horizontal, 8px above / 6px below
        // the content) plus breathing room so neighbouring cards and their shadow tiles never touch.
        const float cardPadX = 8.0f;
        const float cardPadY = 14.0f;
        const float cellSize = state.GridSize + 4.0f + cardPadX * 2.0f + ImGui::GetStyle().ItemSpacing.x;

        const float panelWidth  = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
        int         columnCount = static_cast<int>( panelWidth / cellSize );
        if ( columnCount < 1 )
            columnCount = 1;

        int flags = ImGuiTableFlags_ContextMenuInBody | ImGuiTableFlags_ScrollY;

        if ( state.ListView )
        {
            ImGui::PushStyleVar( ImGuiStyleVar_CellPadding, { 0, 0 } );
            columnCount = 1;
            flags |= ImGuiTableFlags_RowBg | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoPadInnerX |
                     ImGuiTableFlags_SizingStretchSame;
        }
        else
        {
            ImGui::PushStyleVar( ImGuiStyleVar_CellPadding, { cardPadX, cardPadY } );
            flags |= ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit;
        }

        const ImVec2 cursorPos = ImGui::GetCursorPos();
        const ImVec2 region    = ImGui::GetContentRegionAvail();
        // Skip the drop-target background when the body has no area (a zero size asserts inside
        // InvisibleButton) — there is nothing to drop onto in a collapsed/zero-size panel.
        if ( region.x > 0.0f && region.y > 0.0f )
            ImGui::InvisibleButton( "##DragDropTargetAssetPanelBody", region );

        ImGui::SetCursorPos( cursorPos );

        // Tile-hover tracking for the empty-click deselect below: the ScrollY table is its own child window,
        // so a backdrop item in THIS window never sees hover — the tiles report it instead.
        bool tileHovered = false;

        if ( ImGui::BeginTable( "BodyTable", columnCount, flags ) )
        {
            // Grid: pin every column to the card's real width. SizingFixedFit alone sizes a column to its
            // CONTENT (icon/label), which can be narrower than the GridSize-wide card the tile paints —
            // neighbouring cards then overlapped horizontally.
            if ( !state.ListView )
                for ( int ci = 0; ci < columnCount; ++ci )
                    ImGui::TableSetupColumn( nullptr, ImGuiTableColumnFlags_WidthFixed, state.GridSize + 4.0f );

            // Filtered (search) + sorted (name/date/type/size) display order; both views share it.
            const std::vector<std::size_t> displayOrder = BuildDisplayOrder( &current, state, showHidden );
            for ( const std::size_t idx : displayOrder )
            {
                ImGui::TableNextColumn();
                // ONLY WHAT IS ON SCREEN IS DRAWN (THUMB3), as UE's tile view only builds the widgets in view:
                // a tile scrolled away is a Dummy of the last drawn tile's height — no thumbnail lookup, no
                // capture request, no file probe. The selected tile is always drawn, so keyboard navigation can
                // scroll to it.
                float&      cellHeight = m_CellHeight[state.ListView ? 1 : 0];
                const float cellWidth  = ImGui::GetContentRegionAvail().x;
                if ( cellHeight > 0.0f && !ImGui::IsRectVisible( ImVec2( cellWidth, cellHeight ) ) &&
                     !m_On.IsSelected( current.Children[idx] ) )
                {
                    ImGui::Dummy( ImVec2( cellWidth, cellHeight ) );
                    shownIndex++;
                    continue;
                }
                const float      cellTop = ImGui::GetCursorPosY();
                const TileResult tile = m_On.OnDrawTile( static_cast<int>( idx ), !current.Children[idx]->IsFile,
                                                         shownIndex, !state.ListView );
                tileHovered           = tileHovered || tile.Hovered;
                cellHeight            = std::max( cellHeight, ImGui::GetCursorPosY() - cellTop );
                if ( tile.DoubleClicked )
                    break;
                shownIndex++;
            }

            if ( ImGui::BeginPopupContextWindow( "AssetPanelHierarchyContextWindow",
                                                 ImGuiPopupFlags_MouseButtonRight |
                                                      ImGuiPopupFlags_NoOpenOverItems ) )
            {
                m_On.OnBackgroundContextMenu();
                if ( !state.ListView )
                    ImGui::SliderFloat( "##GridSize", &state.GridSize, AssetViewState::kMinGridSize,
                                        AssetViewState::kMaxGridSize );
                ImGui::EndPopup();
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        // Left-click anywhere in the body that is NOT over a tile clears the selection. Hover is checked
        // window-wide including the table's child.
        if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !tileHovered &&
             ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows ) )
            m_On.OnClearSelection();

        ImGui::EndChild();
    }
} // namespace Desert::Editor
