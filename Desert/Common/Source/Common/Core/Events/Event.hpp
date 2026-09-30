#pragma once

#include <cstdint>

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
} // namespace Common