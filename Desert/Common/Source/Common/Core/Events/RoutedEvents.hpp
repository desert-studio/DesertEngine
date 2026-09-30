#pragma once

#include <Common/Core/Events/KeyEvents.hpp>
#include <Common/Core/Events/MouseEvents.hpp>
#include <Common/Core/Events/WindowEvents.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Common
{
    template <typename... Events>
    struct EventTypeList
    {
        static constexpr std::size_t Count = sizeof...( Events );
    };

    using RoutedEvents =
         EventTypeList<KeyPressedEvent, KeyTypedEvent, MouseMovedEvent, MouseScrolledEvent,
                       MouseButtonPressedEvent, EventWindowFileDrop, EventWindowResize, EventWindowClose>;

    namespace EventDetail
    {
        template <typename E, typename List>
        struct IndexIn;

        template <typename E, typename... Events>
        struct IndexIn<E, EventTypeList<Events...>>
        {
            static constexpr std::size_t Value = []
            {
                constexpr bool matches[] = { std::is_same_v<E, Events>... };
                for ( std::size_t i = 0; i < sizeof...( Events ); ++i )
                {
                    if ( matches[i] )
                        return i;
                }
                return sizeof...( Events );
            }();
        };
    } // namespace EventDetail

    template <typename E>
    concept RoutedEvent = EventDetail::IndexIn<E, RoutedEvents>::Value < RoutedEvents::Count;

    template <RoutedEvent E>
    inline constexpr std::size_t EventIndex = EventDetail::IndexIn<E, RoutedEvents>::Value;

    template <RoutedEvent E>
    inline constexpr uint32_t EventBit = uint32_t{ 1 } << EventIndex<E>;

    template <RoutedEvent E>
    inline constexpr EventRouteKind EventRoute = E::Route;

    static_assert( RoutedEvents::Count <= 32, "the handler masks hold one bit per routed event in 32 bits" );
} // namespace Common
