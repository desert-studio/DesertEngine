#define IMGUI_DEFINE_MATH_OPERATORS

#include "ContentBrowserPathView.hpp"

#include <Editor/Core/EditorPreferences.hpp> // the pinned folders live in editor.json (К5)
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserDragDrop.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserUtils.hpp>
#include <Editor/Panels/FileExplorer/ContentDirectoryModel.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>

#include <ImGui/imgui.h>
#include <ImGui/imgui_internal.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
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
                              std::format( "  " ICON_MDI_FOLDER " {}", label.empty() ? fav : label ).c_str() ) )
                        m_On.OnFavouriteSelected( fav );
                    AcceptMoveDropOnLastItem( fav ); // a pinned folder is a drop target, as in UE's Favorites
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
    }

    void ContentBrowserPathView::AcceptMoveDropOnLastItem( const std::string& targetFolder ) const
    {
        if ( !ImGui::BeginDragDropTarget() )
            return;
        // The source's typed payload (TEXTURE_ASSET, MESH_ASSET, ...) is accepted whatever the kind: on a
        // folder every kind means "move it here". The payload is the dragged path's bytes with its NUL.
        for ( const char* type : ContentBrowserDragDrop::MovablePayloads )
        {
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload( type );
            if ( payload == nullptr || payload->DataSize <= 0 )
                continue;
            const std::string              dragged( static_cast<const char*>( payload->Data ) );
            const std::vector<std::string> selection =
                 m_On.SelectedPaths ? m_On.SelectedPaths() : std::vector<std::string>{};
            bool moved = false;
            for ( const std::string& path :
                  ContentBrowserDragDrop::PlanFolderDrop( dragged, selection, targetFolder ) )
                moved = ContentBrowserUtils::MoveFileTo( path, targetFolder ) || moved;
            if ( moved && m_On.OnMoved )
                m_On.OnMoved();
            break;
        }
        ImGui::EndDragDropTarget();
    }

    // A directory tree is walked by its own depth, which the file system bounds.
    // NOLINTNEXTLINE(misc-no-recursion)
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

            const bool isOpen = ImGui::TreeNodeEx( static_cast<const void*>( dirInfo ), nodeFlags, "" );
            if ( ImGui::IsItemClicked() )
                m_On.OnFolderSelected( dirInfo );
            AcceptMoveDropOnLastItem( dirInfo->AssetPath ); // the node row spans the width: drop onto it

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

                    // Spelled per component: in a Windows unity blob imgui.h may arrive first without
                    // IMGUI_DEFINE_MATH_OPERATORS, and ImVec2 then has no operator+.
                    const ImRect childRect( currentPos,
                                            ImVec2( currentPos.x, currentPos.y + ImGui::GetFontSize() ) );

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
    }
} // namespace Desert::Editor
