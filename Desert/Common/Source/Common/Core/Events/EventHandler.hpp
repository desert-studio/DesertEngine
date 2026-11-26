#pragma once

#include <Common/Core/Events/RoutedEvents.hpp>

#include <cstdint>
#include <string_view>
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

        // The handler's type, as a value every module DLL computes alike: the address of a header-defined tag
        // would differ per DLL (each holds its own copy), so a node emplaced in one module failed the typed
        // lookup of another (BINT-STATE). FNV-1a of the compiler's spelling of this function, which names T.
        template <typename T>
        consteval std::uint64_t TypeKey()
        {
#if defined( _MSC_VER )
            constexpr std::string_view name = __FUNCSIG__;
#else
            constexpr std::string_view name = __PRETTY_FUNCTION__;
#endif
            std::uint64_t hash = 14695981039346656037ull;
            for ( const char c : name )
            {
                hash = ( hash ^ static_cast<unsigned char>( c ) ) * 1099511628211ull;
            }
            return hash;
        }

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

        std::uint64_t Type      = 0;
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
            table.Type = TypeKey<T>();
            ( FillSlot<T, Events>( table ), ... );
            return table;
        }
    } // namespace EventDetail

    template <typename T>
    inline constexpr EventHandlerTable EventHandlerTableFor = EventDetail::BuildTable<T>( RoutedEvents{} );

    template <typename T>
    concept ReceivesEvents = EventHandlerTableFor<T>.BubbleMask != 0 || EventHandlerTableFor<T>.PreviewMask != 0;
} // namespace Common
