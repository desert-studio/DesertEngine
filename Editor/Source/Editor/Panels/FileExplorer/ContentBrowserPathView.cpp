#define IMGUI_DEFINE_MATH_OPERATORS

#include "ContentBrowserPathView.hpp"

#include <Editor/Core/EditorPreferences.hpp> // the pinned folders live in editor.json (К5)
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserUtils.hpp>
#include <Editor/Panels/FileExplorer/ContentDirectoryModel.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>

#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>

#include <cstdint>
#include <filesystem>
#include <utility>
#include <vector>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    ContentBrowserPathView::ContentBrowserPathView( Delegates delegates ) : m_On( std::move( delegates ) )
    {
    }

    void ContentBrowserPathView::Draw( const ContentDirectoryModel& model, const DirectoryInformation* current,
                                       float width )
    {
        ImGui::BeginChild( "##cb_left", ImVec2( width, 0.0f ), true );
        {
            // The pins OF THE OPEN PROJECT, asked for where they are drawn. A second project's
            // folders used to appear here, because the retired file had one list for every project
            // this user had ever opened (К5).
            const std::vector<std::string> favourites = EditorPreferences::CurrentFavouriteFolders();
            if ( !favourites.empty() )
            {
                ImGui::TextDisabled( ICON_MDI_STAR " FAVORITES" );
                for ( const auto& fav : favourites )
                {
                    const std::string label = std::filesystem::path( fav ).filename().string();
                    ImGui::PushID( fav.c_str() );
                    if ( ImGui::Selectable(
                              ( std::string( "  " ) + ICON_MDI_FOLDER " " + ( label.empty() ? fav : label ) )
                                   .c_str() ) )
                        m_On.OnFavouriteSelected( fav );
                    if ( ImGui::BeginPopupContextItem( "##favctx" ) )
                    {
                        if ( ImGui::MenuItem( "Remove from Favorites" ) )
                            EditorPreferences::ToggleFavouriteFolder( fav );
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
            }
            ImGui::TextDisabled( ICON_MDI_FOLDER_MULTIPLE " CONTENT" );
            DrawFolder( model, current, model.Root(), true );
        }
        ImGui::EndChild();

        // The folder tree is a move-drop target: drag an asset onto a folder to move it there (the hovered
        // folder sets m_MovePath inside DrawFolder).
        AcceptMoveDropOnLastItem();
    }

    void ContentBrowserPathView::AcceptMoveDropOnLastItem()
    {
        if ( ImGui::BeginDragDropTarget() )
        {
            if ( const ImGuiPayload* data =
                      ImGui::AcceptDragDropPayload( "selectable", ImGuiDragDropFlags_AcceptNoDrawDefaultRect ) )
            {
                const auto* file = static_cast<const std::string*>( data->Data );
                ContentBrowserUtils::MoveFileTo( *file, m_MovePath );
                m_IsDragging = false;
            }
            ImGui::EndDragDropTarget();
        }
    }

    void ContentBrowserPathView::DrawFolder( const ContentDirectoryModel& model,
                                             const DirectoryInformation* current, DirectoryInformation* dirInfo,
                                             bool defaultOpen )
    {
        ImGuiTreeNodeFlags nodeFlags = ( ( dirInfo == current ) ? ImGuiTreeNodeFlags_Selected : 0 );
        nodeFlags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;

        if ( dirInfo->Parent == nullptr )
            nodeFlags |= ImGuiTreeNodeFlags_Framed;

        const ImColor TreeLineColor = ImColor( 128, 128, 128, 128 );
        const float   SmallOffsetX  = 6.0f;
        ImDrawList*   drawList      = ImGui::GetWindowDrawList();

        if ( !dirInfo->IsFile )
        {
            if ( dirInfo->Leaf )
                nodeFlags |= ImGuiTreeNodeFlags_Leaf;

            if ( defaultOpen )
                nodeFlags |= ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Leaf;

            nodeFlags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

            const bool isOpen = ImGui::TreeNodeEx(
                 reinterpret_cast<void*>( reinterpret_cast<intptr_t>( dirInfo ) ), nodeFlags, "" );
            if ( ImGui::IsItemClicked() )
                m_On.OnFolderSelected( dirInfo );

            const char* folderIcon =
                 ( ( isOpen && !dirInfo->Leaf ) || current == dirInfo ) ? ICON_MDI_FOLDER_OPEN : ICON_MDI_FOLDER;
            ImGui::SameLine();
            ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 1.0f, 1.0f, 1.0f ) );
            ImGui::TextUnformatted( folderIcon );
            ImGui::PopStyleColor();
            ImGui::SameLine();

            const std::string fileName = std::filesystem::path( dirInfo->AssetPath ).filename().string();
            ImGui::TextUnformatted( fileName.c_str() );

            ImVec2 verticalLineStart = ImGui::GetCursorScreenPos();

            if ( isOpen && !dirInfo->Leaf )
            {
                verticalLineStart.x += SmallOffsetX; // to nicely line up with the arrow symbol
                ImVec2 verticalLineEnd = verticalLineStart;

                for ( auto* child : dirInfo->Children )
                {
                    if ( !model.ShowsHiddenFiles() && child->Hidden )
                        continue;
                    if ( child->IsFile )
                        continue;

                    const ImVec2 currentPos = ImGui::GetCursorScreenPos();

                    ImGui::Indent( 10.0f );

                    float horizontalTreeLineSize = 16.0f; // chosen arbitrarily
                    if ( !child->Leaf )
                        horizontalTreeLineSize *= 0.5f;
                    DrawFolder( model, current, child );

                    const ImRect childRect =
                         ImRect( currentPos, currentPos + ImVec2( 0.0f, ImGui::GetFontSize() ) );

                    const float midpoint = ( childRect.Min.y + childRect.Max.y ) * 0.5f;
                    drawList->AddLine( ImVec2( verticalLineStart.x, midpoint ),
                                       ImVec2( verticalLineStart.x + horizontalTreeLineSize, midpoint ),
                                       TreeLineColor );
                    verticalLineEnd.y = midpoint;

                    ImGui::Unindent( 10.0f );
                }

                drawList->AddLine( verticalLineStart, verticalLineEnd, TreeLineColor );

                ImGui::TreePop();
            }

            if ( isOpen && dirInfo->Leaf )
                ImGui::TreePop();
        }

        if ( m_IsDragging && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem ) )
            m_MovePath = dirInfo->AssetPath;
    }
} // namespace Desert::Editor
