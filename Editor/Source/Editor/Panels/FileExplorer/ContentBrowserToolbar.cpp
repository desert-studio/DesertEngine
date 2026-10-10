#include "ContentBrowserToolbar.hpp"

#include <Editor/Core/EditorPreferences.hpp> // the pinned folders live in editor.json (К5)
#include <Editor/Core/IconsMaterialDesignIcons.hpp>
#include <Editor/Panels/FileExplorer/AssetViewState.hpp>
#include <Editor/Panels/FileExplorer/ContentBrowserHistory.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>
#include <Editor/Panels/FileExplorer/FileType.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        struct TypeFilterRow
        {
            const char* Label;
            int         Type;
        };

        // Type filter — show only one asset kind (folders always stay visible).
        constexpr TypeFilterRow kTypeFilters[] = {
             { "All Types", -1 },
             { "Scenes", static_cast<int>( FileType::Scene ) },
             { "Prefabs", static_cast<int>( FileType::Prefab ) },
             { "Scripts", static_cast<int>( FileType::Script ) },
             { "Textures", static_cast<int>( FileType::Texture ) },
             { "Materials", static_cast<int>( FileType::Material ) },
             { "Models", static_cast<int>( FileType::Model ) },
             { "Shader Graphs", static_cast<int>( FileType::ShaderGraph ) },
             { "Audio", static_cast<int>( FileType::Audio ) },
             { "Clouds", static_cast<int>( FileType::Cloud ) },
             { "Skeletal Meshes", static_cast<int>( FileType::SkinnedMesh ) },
             { "Skeletons", static_cast<int>( FileType::Skeleton ) },
             { "Animations", static_cast<int>( FileType::Animation ) },
             { "Foliage Types", static_cast<int>( FileType::FoliageType ) },
             { "Level Sequences", static_cast<int>( FileType::LevelSequence ) },
             { "VFX Systems", static_cast<int>( FileType::VFXSystem ) },
             { "Fractures", static_cast<int>( FileType::Fracture ) },
             { "Water Waves", static_cast<int>( FileType::WaterWaves ) },
             { "VFX Data Channels", static_cast<int>( FileType::VFXDataChannel ) },
        };
    } // namespace

    ContentBrowserToolbar::ContentBrowserToolbar( Delegates delegates ) : m_On( std::move( delegates ) )
    {
    }

    void ContentBrowserToolbar::Draw( AssetViewState& state, const ContentBrowserHistory& history,
                                      DirectoryInformation* current, DirectoryInformation* root )
    {
        ImGui::BeginChild( "##cb_toolbar", ImVec2( 0.0f, ImGui::GetFrameHeightWithSpacing() * 2.0f ), false,
                           ImGuiWindowFlags_NoScrollbar );

        ImGui::AlignTextToFramePadding();
        // View settings: grid/list, refresh, new folder, tile size.
        if ( ImGui::Button( ICON_MDI_COGS ) )
            ImGui::OpenPopup( "SettingsPopup" );
        if ( ImGui::BeginPopup( "SettingsPopup" ) )
        {
            if ( ImGui::Button( state.ListView ? ICON_MDI_VIEW_LIST " Switch to Grid View"
                                               : ICON_MDI_VIEW_GRID " Switch to List View" ) )
                state.ListView = !state.ListView;

            if ( ImGui::Selectable( "Refresh" ) )
                m_On.OnRefresh();

            if ( ImGui::Selectable( "New folder" ) )
                m_On.OnNewFolder();

            if ( !state.ListView )
                ImGui::SliderFloat( "##GridSize", &state.GridSize, AssetViewState::kMinGridSize,
                                    AssetViewState::kMaxGridSize );

            ImGui::EndPopup();
        }
        ImGui::SameLine();

        ImGui::TextUnformatted( ICON_MDI_MAGNIFY );
        ImGui::SameLine();

        // Name filter — substring match against filenames (see BuildDisplayOrder).
        ImGui::SetNextItemWidth( 180.0f );
        ImGui::InputTextWithHint( "##AssetSearch", "Filter by name...", state.SearchBuf,
                                  sizeof( state.SearchBuf ) );
        ImGui::SameLine();

        // Sort mode + ascending/descending toggle.
        ImGui::SetNextItemWidth( 130.0f );
        const char* const sortNames[] = { "Name", "Date Modified", "Type", "Size" };
        int               sortIdx     = static_cast<int>( state.Sort );
        if ( ImGui::Combo( "##AssetSort", &sortIdx, sortNames, IM_ARRAYSIZE( sortNames ) ) )
            state.Sort = static_cast<AssetViewState::SortMode>( sortIdx );
        ImGui::SameLine();
        if ( ImGui::Button( state.SortDescending ? ICON_MDI_SORT_DESCENDING : ICON_MDI_SORT_ASCENDING ) )
            state.SortDescending = !state.SortDescending;
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( state.SortDescending ? "Descending" : "Ascending" );
        ImGui::SameLine();

        ImGui::SetNextItemWidth( 130.0f );
        const char* currentFilter = "All Types";
        for ( const auto& f : kTypeFilters )
            if ( f.Type == state.TypeFilter )
                currentFilter = f.Label;
        if ( ImGui::BeginCombo( "##AssetTypeFilter", currentFilter ) )
        {
            for ( const auto& f : kTypeFilters )
                if ( ImGui::Selectable( f.Label, f.Type == state.TypeFilter ) )
                    state.TypeFilter = f.Type;
            ImGui::EndCombo();
        }
        ImGui::SameLine();

        // Back / Forward / Up navigation.
        ImGui::BeginDisabled( !history.CanGoBack() );
        if ( ImGui::Button( ICON_MDI_ARROW_LEFT ) )
            m_On.OnBack();
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Back" );
        ImGui::SameLine();

        ImGui::BeginDisabled( !history.CanGoForward() );
        if ( ImGui::Button( ICON_MDI_ARROW_RIGHT ) )
            m_On.OnForward();
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Forward" );
        ImGui::SameLine();

        ImGui::BeginDisabled( !current || current == root );
        if ( ImGui::Button( ICON_MDI_ARROW_UP_BOLD ) && current )
            m_On.OnFolderSelected( current->Parent );
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Up" );
        ImGui::SameLine();

        // Favorites: jump to a pinned folder (added via a folder's right-click menu).
        if ( ImGui::Button( ICON_MDI_STAR ) )
            ImGui::OpenPopup( "##favMenu" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Favorite folders" );
        if ( ImGui::BeginPopup( "##favMenu" ) )
        {
            const std::vector<std::string> favourites = EditorPreferences::CurrentFavouriteFolders();
            if ( favourites.empty() )
                ImGui::TextDisabled( "No favorites — right-click a folder -> Add to Favorites." );
            for ( const auto& fav : favourites )
            {
                const std::string label = std::filesystem::path( fav ).filename().string();
                if ( ImGui::MenuItem( ( label.empty() ? fav : label ).c_str() ) )
                    m_On.OnFavouriteSelected( fav );
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( ICON_MDI_FILE_IMPORT " Import" ) )
            m_On.OnImport();
        ImGui::SameLine();

        DrawBreadcrumbs( current, root );

        ImGui::EndChild();
    }

    void ContentBrowserToolbar::DrawBreadcrumbs( DirectoryInformation* current, DirectoryInformation* root )
    {
        if ( m_BreadcrumbsStale )
        {
            m_Breadcrumbs.clear();
            for ( DirectoryInformation* node = current; node; node = node->Parent )
                m_Breadcrumbs.push_back( node->Parent ? node : root );
            std::reverse( m_Breadcrumbs.begin(), m_Breadcrumbs.end() );
            m_BreadcrumbsStale = false;
        }

        ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.1f, 0.2f, 0.7f, 0.0f ) );
        for ( std::size_t i = 0; i < m_Breadcrumbs.size(); ++i )
        {
            DirectoryInformation* directory = m_Breadcrumbs[i];
            const std::string     fileName  = std::filesystem::path( directory->AssetPath ).filename().string();

            ImGui::PushID( directory );
            if ( ImGui::SmallButton( fileName.c_str() ) )
                m_On.OnFolderSelected( directory );
            ImGui::PopID();
            ImGui::SameLine();

            if ( i + 1 < m_Breadcrumbs.size() )
            {
                ImGui::TextDisabled( ">" );
                ImGui::SameLine();
            }
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
} // namespace Desert::Editor
