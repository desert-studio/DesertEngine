#include <UI/UIOverlayRequests.hpp>

#include <Common/Core/Logger.hpp>

#include <utility>

namespace Desert::UI
{
    void UIOverlayRequests::Raise( std::string overlay, std::string text )
    {
        if ( overlay.empty() )
        {
            LOG_ERROR( "[UI] a notification was raised with no overlay name and is dropped; name the Toast "
                       "overlay it belongs to" );
            return;
        }
        m_Pending.push_back( { std::move( overlay ), std::move( text ) } );
    }

    std::vector<UIOverlayRequests::Request> UIOverlayRequests::Drain()
    {
        std::vector<Request> out;
        out.swap( m_Pending );
        return out;
    }

    void UIOverlayRequests::Clear()
    {
        m_Pending.clear();
    }
} // namespace Desert::UI
