#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Desert::Editor
{
    /// THE CONTENT BROWSER'S BACK / FORWARD (UE: ContentBrowserHistory / FHistoryManager): the folders visited, in
    /// order, and where in that list the browser stands. A navigation made BY a step is not recorded — Step
    /// suppresses Record for the duration of its own navigation.
    class ContentBrowserHistory
    {
    public:
        /// Remembers @p path as the newest visit: drops the forward branch, and a repeat of the current folder is
        /// not a new entry. Ignored while a Step is navigating.
        void Record( const std::string& path );

        [[nodiscard]] bool CanGoBack() const
        {
            return m_Pos > 0;
        }
        [[nodiscard]] bool CanGoForward() const
        {
            return m_Pos + 1 < static_cast<int>( m_Visited.size() );
        }

        /// Moves one entry back (@p delta = -1) or forward (+1) and calls @p navigate with that entry's path; does
        /// nothing at either end. The position moves whether or not @p navigate finds the folder.
        template <typename Navigate>
        void Step( int delta, Navigate&& navigate )
        {
            if ( delta < 0 ? !CanGoBack() : !CanGoForward() )
                return;
            m_Pos += delta < 0 ? -1 : 1;
            m_Stepping = true;
            navigate( m_Visited[static_cast<std::size_t>( m_Pos )] );
            m_Stepping = false;
        }

    private:
        std::vector<std::string> m_Visited;
        int                      m_Pos      = -1;
        bool                     m_Stepping = false;
    };
} // namespace Desert::Editor
