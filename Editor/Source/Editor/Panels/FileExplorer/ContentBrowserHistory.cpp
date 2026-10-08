#include "ContentBrowserHistory.hpp"

namespace Desert::Editor
{
    void ContentBrowserHistory::Record( const std::string& path )
    {
        if ( m_Stepping )
            return;
        if ( m_Pos + 1 < static_cast<int>( m_Visited.size() ) )
            m_Visited.resize( static_cast<std::size_t>( m_Pos + 1 ) ); // drop the forward branch
        if ( m_Visited.empty() || m_Visited.back() != path )
        {
            m_Visited.push_back( path );
            m_Pos = static_cast<int>( m_Visited.size() ) - 1;
        }
    }
} // namespace Desert::Editor
