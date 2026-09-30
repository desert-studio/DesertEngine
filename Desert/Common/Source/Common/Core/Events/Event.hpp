#pragma once

#include <cstdint>
#include <functional>

#define DESERT_ROUTED_EVENT( EventClass, HandlerSuffix, RouteKind )                                               \
    static constexpr ::Common::EventRouteKind Route = ::Common::EventRouteKind::RouteKind;                        \
    template <typename Handler>                                                                                   \
    static constexpr bool HasHandler =                                                                            \
         requires { static_cast<bool ( Handler::* )( EventClass& )>( &Handler::On##HandlerSuffix ); };            \
    template <typename Handler>                                                                                   \
    static constexpr bool HasPreview =                                                                            \
         requires { static_cast<bool ( Handler::* )( EventClass& )>( &Handler::OnPreview##HandlerSuffix ); };     \
    template <typename Handler>                                                                                   \
    static constexpr bool HasLooseHandler =                                                                       \
         requires( Handler& handler, EventClass& event ) { handler.On##HandlerSuffix( event ); };                 \
    template <typename Handler>                                                                                   \
    static constexpr bool HasLoosePreview =                                                                       \
         requires( Handler& handler, EventClass& event ) { handler.OnPreview##HandlerSuffix( event ); };          \
    template <typename Handler>                                                                                   \
    static bool Deliver( Handler& handler, EventClass& event )                                                    \
    {                                                                                                             \
        return ( handler.*                                                                                        \
                 static_cast<bool ( Handler::* )( EventClass& )>( &Handler::On##HandlerSuffix ) )( event );       \
    }                                                                                                             \
    template <typename Handler>                                                                                   \
    static bool DeliverPreview( Handler& handler, EventClass& event )                                             \
    {                                                                                                             \
        return ( handler.*static_cast<bool ( Handler::* )( EventClass& )>(                                        \
                               &Handler::OnPreview##HandlerSuffix ) )( event );                                   \
    }

namespace Common
{
    enum class EventRouteKind : uint8_t
    {
        Focus,
        Pointer,
        Broadcast
    };

    enum class EventType
    {
        None = 0,
        WindowClose,
        WindowResize,
        WindowFileDrop, // Window
        KeyPressed,
        KeyTyped, // Keys
        MouseMoved,
        MouseScroll,
        MousePressed // Mouse
    };

    class Event
    {
    public:
        virtual EventType GetEventType() const = 0;

        bool m_Handled = false;
    };

    class EventManager final // NOTE: Should be static ?
    {
    public:
        template <typename T>
        using EventFN = std::function<bool( T& )>;

        explicit EventManager( Event& e ) : m_Event( e )
        {
        }

        template <typename T>
        bool Notify( EventFN<T> func )
        {
            if ( m_Event.GetEventType() == T::GetStaticType() )
            {
                m_Event.m_Handled = func( *(T*)&m_Event ); // TODO: static_cast
                return true;
            }
            return false;
        }

        Event& m_Event;
    };

} // namespace Common