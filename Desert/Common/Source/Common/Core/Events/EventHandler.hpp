#pragma once

#include <Common/Core/Events/RoutedEvents.hpp>

#include <tuple>

namespace Common
{
    template <typename T, typename E>
    concept HandlesEvent = RoutedEvent<E> && E::template HasHandler<T>;

    template <typename T, typename E>
    concept PreviewsEvent = RoutedEvent<E> && E::template HasPreview<T>;

    template <typename T, typename E>
    concept AcceptsEventThroughAnotherSignature =
         RoutedEvent<E> && !HandlesEvent<T, E> && E::template HasLooseHandler<T>;

    template <typename T, typename E>
    concept PreviewsEventThroughAnotherSignature =
         RoutedEvent<E> && !PreviewsEvent<T, E> && E::template HasLoosePreview<T>;

    template <typename E>
    using EventThunk = bool ( * )( void* object, E& event );

    namespace EventDetail
    {
        template <typename List>
        struct ThunkRow;

        template <typename... Events>
        struct ThunkRow<EventTypeList<Events...>>
        {
            using Type = std::tuple<EventThunk<Events>...>;
        };

        template <typename T>
        inline char TypeTag = 0;

        template <typename T, typename E>
        bool Deliver( void* object, E& event )
        {
            return E::Deliver( *static_cast<T*>( object ), event );
        }

        template <typename T, typename E>
        bool DeliverPreview( void* object, E& event )
        {
            return E::DeliverPreview( *static_cast<T*>( object ), event );
        }
    } // namespace EventDetail

    struct EventHandlerTable
    {
        using Thunks = EventDetail::ThunkRow<RoutedEvents>::Type;

        const void* Type        = nullptr;
        uint32_t    BubbleMask  = 0;
        uint32_t    PreviewMask = 0;
        Thunks      Bubble{};
        Thunks      Preview{};
    };

    namespace EventDetail
    {
        template <typename T, typename E>
        consteval void FillSlot( EventHandlerTable& table )
        {
            static_assert( !AcceptsEventThroughAnotherSignature<T, E>,
                           "the handler named for this event must be exactly `bool On<Event>( <Event>& )`" );
            static_assert(
                 !PreviewsEventThroughAnotherSignature<T, E>,
                 "the preview handler named for this event must be exactly `bool OnPreview<Event>( <Event>& )`" );
            if constexpr ( HandlesEvent<T, E> )
            {
                std::get<EventIndex<E>>( table.Bubble ) = &Deliver<T, E>;
                table.BubbleMask |= EventBit<E>;
            }
            if constexpr ( PreviewsEvent<T, E> )
            {
                std::get<EventIndex<E>>( table.Preview ) = &DeliverPreview<T, E>;
                table.PreviewMask |= EventBit<E>;
            }
        }

        template <typename T, typename... Events>
        consteval EventHandlerTable BuildTable( EventTypeList<Events...> /*list*/ )
        {
            EventHandlerTable table{};
            table.Type = &TypeTag<T>;
            ( FillSlot<T, Events>( table ), ... );
            return table;
        }
    } // namespace EventDetail

    template <typename T>
    inline constexpr EventHandlerTable EventHandlerTableFor = EventDetail::BuildTable<T>( RoutedEvents{} );

    template <typename T>
    concept ReceivesEvents = EventHandlerTableFor<T>.BubbleMask != 0 || EventHandlerTableFor<T>.PreviewMask != 0;
} // namespace Common
