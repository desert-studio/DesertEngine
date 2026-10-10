#include "ContentBrowserSelection.hpp"

#include <Editor/Panels/FileExplorer/AssetViewState.hpp>
#include <Editor/Panels/FileExplorer/DirectoryInformation.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace Desert::Editor
{
    bool ContentBrowserSelection::Contains( const DirectoryInformation* entry ) const
    {
        return entry != nullptr && m_Paths.contains( entry->AssetPath );
    }

    std::vector<std::string> ContentBrowserSelection::Paths() const
    {
        if ( !m_Paths.empty() )
            return { m_Paths.begin(), m_Paths.end() };
        if ( m_Current != nullptr )
            return { m_Current->AssetPath };
        return {};
    }

    std::vector<DirectoryInformation*>
    ContentBrowserSelection::EntriesIn( const DirectoryInformation* folder ) const
    {
        std::vector<DirectoryInformation*> entries;
        if ( folder == nullptr )
            return entries;
        const std::vector<std::string> selected = Paths();
        for ( DirectoryInformation* child : folder->Children )
            if ( std::find( selected.begin(), selected.end(), child->AssetPath ) != selected.end() )
                entries.push_back( child );
        return entries;
    }

    void ContentBrowserSelection::Click( DirectoryInformation& entry, int shownIndex,
                                         const DirectoryInformation& folder, const AssetViewState& view,
                                         bool showHidden )
    {
        const ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyShift && m_AnchorShown >= 0 )
        {
            const auto order = BuildDisplayOrder( &folder, view, showHidden );
            const int  lo    = std::min( m_AnchorShown, shownIndex );
            const int  hi    = std::max( m_AnchorShown, shownIndex );
            m_Paths.clear();
            for ( int i = lo; i <= hi && i < static_cast<int>( order.size() ); ++i )
                m_Paths.insert( folder.Children[order[static_cast<std::size_t>( i )]]->AssetPath );
        }
        else if ( io.KeyCtrl || io.KeySuper ) // Cmd on macOS
        {
            if ( m_Paths.contains( entry.AssetPath ) )
                m_Paths.erase( entry.AssetPath );
            else
                m_Paths.insert( entry.AssetPath );
            m_AnchorShown = shownIndex;
        }
        else
        {
            m_Paths       = { entry.AssetPath };
            m_AnchorShown = shownIndex;
        }
        m_Current = &entry;
    }

    void ContentBrowserSelection::SelectOnly( DirectoryInformation& entry )
    {
        m_Paths       = { entry.AssetPath };
        m_Current     = &entry;
        m_AnchorShown = -1;
    }

    void ContentBrowserSelection::Deselect()
    {
        m_Paths.clear();
        m_Current = nullptr;
    }

    void ContentBrowserSelection::Clear()
    {
        Deselect();
        m_AnchorShown = -1;
    }

    std::string ContentBrowserSelection::ReleaseCurrent()
    {
        std::string path = m_Current != nullptr ? m_Current->AssetPath : std::string();
        m_Current        = nullptr;
        return path;
    }

    void ContentBrowserSelection::Copy( bool cut )
    {
        m_Clipboard    = Paths();
        m_ClipboardCut = cut;
    }

    void ContentBrowserSelection::Pasted()
    {
        if ( m_ClipboardCut )
            m_Clipboard.clear();
    }
} // namespace Desert::Editor
