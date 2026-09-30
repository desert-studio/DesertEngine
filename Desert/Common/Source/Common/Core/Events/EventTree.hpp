#pragma once

#include <functional>
#include <Common/Core/Events/EventHandler.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

namespace Common
{
    struct EventNodeId
    {
        static constexpr uint32_t kNone = UINT32_MAX;

        uint32_t Index      = kNone;
        uint32_t Generation = 0;

        [[nodiscard]] bool IsSet() const
        {
            return Index != kNone;
        }
        friend bool operator==( const EventNodeId&, const EventNodeId& ) = default;
    };

    enum class EventPhase : uint8_t
    {
        None,
        Preview,
        Bubble
    };

    struct EventReply
    {
        bool        Handled = false;
        EventNodeId HandledBy{};
        EventPhase  Phase = EventPhase::None;
    };

    template <typename T>
    struct EmplacedNode
    {
        EventNodeId Id;
        T&          Object;
    };

    class EventTree
    {
    public:
        EventTree()
        {
            m_Slots.emplace_back();
            m_Slots[0].Alive = true;
            m_Slots[0].Table = &EventHandlerTableFor<RootMarker>;
        }

        ~EventTree()
        {
            for ( const uint32_t child : m_Slots[0].Children )
                Kill( child );
            FlushGraveyard();
        }

        EventTree( const EventTree& )            = delete;
        EventTree& operator=( const EventTree& ) = delete;
        EventTree( EventTree&& )                 = delete;
        EventTree& operator=( EventTree&& )      = delete;

        [[nodiscard]] EventNodeId Root() const
        {
            return { 0, m_Slots[0].Generation };
        }

        template <typename T, typename... Args>
        EmplacedNode<T> Emplace( EventNodeId parent, Args&&... args )
        {
            auto owned  = std::make_unique<T>( std::forward<Args>( args )... );
            T&   object = *owned;
            return { Adopt<T>( parent, std::move( owned ) ), object };
        }

        template <typename T>
        EventNodeId Adopt( EventNodeId parent, std::unique_ptr<T> object )
        {
            Require( object != nullptr, "Adopt of a null object" );
            return Insert<T>( parent, object.release(), &DestroyAs<T> );
        }

        template <typename T>
        EventNodeId Attach( EventNodeId parent, T& object )
        {
            return Insert<T>( parent, &object, nullptr );
        }

        bool Remove( EventNodeId id )
        {
            if ( !IsLive( id ) )
                return false;
            Require( id.Index != 0, "the root of an EventTree cannot be removed" );
            Unlink( id.Index );
            Kill( id.Index );
            if ( m_DispatchDepth == 0 )
                FlushGraveyard();
            return true;
        }

        template <typename T>
        std::unique_ptr<T> Take( EventNodeId id )
        {
            if ( !IsLive( id ) )
                return nullptr;
            Require( id.Index != 0, "the root of an EventTree cannot be taken" );
            Require( m_Slots[id.Index].Table->Type == &EventDetail::TypeTag<T>,
                     "Take<T> of a node that was not inserted as T" );
            for ( const uint32_t child : std::vector<uint32_t>( m_Slots[id.Index].Children ) )
                Remove( { child, m_Slots[child].Generation } );
            std::unique_ptr<T> object( static_cast<T*>( m_Slots[id.Index].Object ) );
            m_Slots[id.Index].Object = nullptr;
            Unlink( id.Index );
            Kill( id.Index );
            if ( m_DispatchDepth == 0 )
                FlushGraveyard();
            return object;
        }

        [[nodiscard]] bool Contains( EventNodeId id ) const
        {
            return IsLive( id );
        }

        template <typename T>
        T* Get( EventNodeId id ) const
        {
            if ( !IsLive( id ) || m_Slots[id.Index].Table->Type != &EventDetail::TypeTag<T> )
                return nullptr;
            return static_cast<T*>( m_Slots[id.Index].Object );
        }

        [[nodiscard]] EventNodeId Parent( EventNodeId id ) const
        {
            if ( !IsLive( id ) || id.Index == 0 )
                return {};
            const uint32_t parent = m_Slots[id.Index].Parent;
            return { parent, m_Slots[parent].Generation };
        }

        [[nodiscard]] const EventHandlerTable* HandlerTableOf( EventNodeId id ) const
        {
            return IsLive( id ) ? m_Slots[id.Index].Table : nullptr;
        }

        [[nodiscard]] std::size_t Size() const
        {
            return m_LiveCount;
        }

        bool SetFocus( EventNodeId id )
        {
            return Assign( m_Focus, id );
        }
        bool SetHovered( EventNodeId id )
        {
            return Assign( m_Hovered, id );
        }
        bool SetCapture( EventNodeId id )
        {
            return Assign( m_Capture, id );
        }
        [[nodiscard]] EventNodeId Focus() const
        {
            return IsLive( m_Focus ) ? m_Focus : EventNodeId{};
        }
        [[nodiscard]] EventNodeId Hovered() const
        {
            return IsLive( m_Hovered ) ? m_Hovered : EventNodeId{};
        }
        [[nodiscard]] EventNodeId Capture() const
        {
            return IsLive( m_Capture ) ? m_Capture : EventNodeId{};
        }

        template <RoutedEvent E>
        EventReply Route( E& event )
        {
            DispatchScope             scope( *this );
            std::vector<EventNodeId>& path = scope.Path();

            if constexpr ( EventRoute<E> == EventRouteKind::Broadcast )
            {
                CollectBroadcastOrder( path );
                EventReply reply;
                for ( const EventNodeId id : path )
                {
                    if ( CallBubble( id, event ) && !reply.Handled )
                        reply = { true, id, EventPhase::Bubble };
                }
                return reply;
            }
            else
            {
                const EventNodeId target = EventRoute<E> == EventRouteKind::Focus ? Focus() : PointerTarget();
                if ( !target.IsSet() )
                    return {};
                CollectChain( target.Index, path );

                for ( const EventNodeId id : std::views::reverse( path ) )
                {
                    if ( CallPreview( id, event ) )
                        return { true, id, EventPhase::Preview };
                }
                for ( const EventNodeId id : path )
                {
                    if ( CallBubble( id, event ) )
                        return { true, id, EventPhase::Bubble };
                }
                return {};
            }
        }

        template <RoutedEvent E>
        void Defer( E event )
        {
            m_Deferred.emplace_back( [held = std::move( event )]( EventTree& tree ) mutable
                                     { tree.Route( held ); } );
        }

        [[nodiscard]] std::size_t DeferredCount() const
        {
            return m_Deferred.size();
        }

        void RouteDeferred()
        {
            Require( m_DispatchDepth == 0,
                     "deferred events are routed at the frame boundary, not from a handler" );
            std::vector<std::function<void( EventTree& )>> due;
            due.swap( m_Deferred );
            for ( auto& route : due )
                route( *this );
        }

    private:
        struct RootMarker
        {
        };

        struct Slot
        {
            void* Object                        = nullptr;
            void ( *Destroy )( void* )          = nullptr;
            const EventHandlerTable* Table      = nullptr;
            uint32_t                 Parent     = EventNodeId::kNone;
            uint32_t                 Next       = EventNodeId::kNone;
            uint32_t                 Generation = 0;
            uint32_t                 VisitStamp = 0;
            bool                     Alive      = false;
            std::vector<uint32_t>    Children;
        };

        class DispatchScope
        {
        public:
            explicit DispatchScope( EventTree& tree ) : m_Tree( tree ), m_Level( tree.m_DispatchDepth++ )
            {
                if ( m_Tree.m_PathBuffers.size() <= m_Level )
                    m_Tree.m_PathBuffers.emplace_back();
                m_Tree.m_PathBuffers[m_Level].clear();
            }
            ~DispatchScope()
            {
                if ( --m_Tree.m_DispatchDepth == 0 )
                    m_Tree.FlushGraveyard();
            }
            DispatchScope( const DispatchScope& )            = delete;
            DispatchScope& operator=( const DispatchScope& ) = delete;

            std::vector<EventNodeId>& Path()
            {
                return m_Tree.m_PathBuffers[m_Level];
            }

        private:
            EventTree&  m_Tree;
            std::size_t m_Level;
        };

        static void Require( bool condition, const char* what )
        {
            if ( !condition )
            {
                std::fprintf( stderr, "EventTree: %s\n", what );
                std::abort();
            }
        }

        [[nodiscard]] bool IsLive( EventNodeId id ) const
        {
            return id.Index < m_Slots.size() && m_Slots[id.Index].Alive &&
                   m_Slots[id.Index].Generation == id.Generation;
        }

        bool Assign( EventNodeId& field, EventNodeId id )
        {
            if ( id.IsSet() && !IsLive( id ) )
                return false;
            field = id;
            return true;
        }

        template <typename T>
        static void DestroyAs( void* object )
        {
            std::default_delete<T>{}( static_cast<T*>( object ) );
        }

        [[nodiscard]] EventNodeId PointerTarget() const
        {
            const EventNodeId captured = Capture();
            return captured.IsSet() ? captured : Hovered();
        }

        template <typename T>
        EventNodeId Insert( EventNodeId parent, T* object, void ( *destroy )( void* ) )
        {
            if ( !IsLive( parent ) )
            {
                if ( destroy != nullptr )
                    destroy( object );
                Require( false, "insertion under a node that is not in the tree" );
            }
            uint32_t index = 0;
            if ( m_FreeHead != EventNodeId::kNone )
            {
                index      = m_FreeHead;
                m_FreeHead = m_Slots[index].Next;
            }
            else
            {
                index = static_cast<uint32_t>( m_Slots.size() );
                m_Slots.emplace_back();
            }
            Slot& slot   = m_Slots[index];
            slot.Object  = object;
            slot.Destroy = destroy;
            slot.Table   = &EventHandlerTableFor<T>;
            slot.Parent  = parent.Index;
            slot.Next    = EventNodeId::kNone;
            slot.Alive   = true;
            slot.Children.clear();
            m_Slots[parent.Index].Children.push_back( index );
            ++m_LiveCount;
            return { index, slot.Generation };
        }

        void Unlink( uint32_t index )
        {
            std::vector<uint32_t>& siblings = m_Slots[m_Slots[index].Parent].Children;
            for ( auto it = siblings.begin(); it != siblings.end(); ++it )
            {
                if ( *it == index )
                {
                    siblings.erase( it );
                    return;
                }
            }
        }

        [[nodiscard]] uint32_t Deepest( uint32_t index ) const
        {
            while ( !m_Slots[index].Children.empty() )
                index = m_Slots[index].Children.front();
            return index;
        }

        [[nodiscard]] uint32_t AfterInPostOrder( uint32_t index ) const
        {
            const std::vector<uint32_t>& siblings = m_Slots[m_Slots[index].Parent].Children;
            const auto                   next     = std::ranges::find( siblings, index ) + 1;
            return next == siblings.end() ? m_Slots[index].Parent : Deepest( *next );
        }

        void Kill( uint32_t top )
        {
            uint32_t index = Deepest( top );
            while ( true )
            {
                const bool     last = index == top;
                const uint32_t next = last ? top : AfterInPostOrder( index );
                Slot&          slot = m_Slots[index];
                slot.Alive          = false;
                ++slot.Generation;
                slot.Children.clear();
                slot.Next = EventNodeId::kNone;
                if ( m_GraveyardTail == EventNodeId::kNone )
                    m_GraveyardHead = index;
                else
                    m_Slots[m_GraveyardTail].Next = index;
                m_GraveyardTail = index;
                --m_LiveCount;
                if ( last )
                    return;
                index = next;
            }
        }

        void FlushGraveyard()
        {
            while ( m_GraveyardHead != EventNodeId::kNone )
            {
                const uint32_t index = m_GraveyardHead;
                m_GraveyardHead      = m_Slots[index].Next;
                if ( m_GraveyardHead == EventNodeId::kNone )
                    m_GraveyardTail = EventNodeId::kNone;
                void* const object               = std::exchange( m_Slots[index].Object, nullptr );
                void ( *const destroy )( void* ) = std::exchange( m_Slots[index].Destroy, nullptr );
                m_Slots[index].Next              = m_FreeHead;
                m_FreeHead                       = index;
                if ( object != nullptr && destroy != nullptr )
                    destroy( object );
            }
        }

        void CollectChain( uint32_t index, std::vector<EventNodeId>& out ) const
        {
            for ( ; index != EventNodeId::kNone; index = m_Slots[index].Parent )
                out.push_back( { index, m_Slots[index].Generation } );
        }

        void CollectBroadcastOrder( std::vector<EventNodeId>& out )
        {
            const uint32_t stamp = ++m_VisitStamp;
            auto           visit = [&]( uint32_t index )
            {
                if ( m_Slots[index].VisitStamp == stamp )
                    return;
                m_Slots[index].VisitStamp = stamp;
                out.push_back( { index, m_Slots[index].Generation } );
            };
            auto chain = [&]( EventNodeId from )
            {
                if ( !from.IsSet() )
                    return;
                for ( uint32_t index = from.Index; index != EventNodeId::kNone; index = m_Slots[index].Parent )
                    visit( index );
            };
            chain( Focus() );
            chain( PointerTarget() );

            m_Stack.clear();
            m_Stack.push_back( 0 );
            while ( !m_Stack.empty() )
            {
                const uint32_t index = m_Stack.back();
                m_Stack.pop_back();
                visit( index );
                for ( const uint32_t child : std::views::reverse( m_Slots[index].Children ) )
                    m_Stack.push_back( child );
            }
        }

        template <typename E>
        bool CallBubble( EventNodeId id, E& event )
        {
            if ( !IsLive( id ) )
                return false;
            const Slot& slot = m_Slots[id.Index];
            if ( (slot.Table->BubbleMask & EventBit<E>) == 0 )
                return false;
            return std::get<EventIndex<E>>( slot.Table->Bubble )( slot.Object, event );
        }

        template <typename E>
        bool CallPreview( EventNodeId id, E& event )
        {
            if ( !IsLive( id ) )
                return false;
            const Slot& slot = m_Slots[id.Index];
            if ( (slot.Table->PreviewMask & EventBit<E>) == 0 )
                return false;
            return std::get<EventIndex<E>>( slot.Table->Preview )( slot.Object, event );
        }

        std::vector<Slot>                              m_Slots;
        std::vector<std::vector<EventNodeId>>          m_PathBuffers;
        std::vector<uint32_t>                          m_Stack;
        std::vector<std::function<void( EventTree& )>> m_Deferred;
        std::size_t                                    m_LiveCount     = 0;
        std::size_t                                    m_DispatchDepth = 0;
        uint32_t                                       m_VisitStamp    = 0;
        uint32_t                                       m_FreeHead      = EventNodeId::kNone;
        uint32_t                                       m_GraveyardHead = EventNodeId::kNone;
        uint32_t                                       m_GraveyardTail = EventNodeId::kNone;
        EventNodeId                                    m_Focus{};
        EventNodeId                                    m_Hovered{};
        EventNodeId                                    m_Capture{};
    };
    class EventNodeLink
    {
    public:
        EventNodeLink() = default;
        EventNodeLink( EventTree& tree, EventNodeId id ) : m_Tree( &tree ), m_Id( id )
        {
        }
        ~EventNodeLink()
        {
            Release();
        }
        EventNodeLink( const EventNodeLink& )            = delete;
        EventNodeLink& operator=( const EventNodeLink& ) = delete;
        EventNodeLink( EventNodeLink&& other ) noexcept
             : m_Tree( std::exchange( other.m_Tree, nullptr ) ), m_Id( std::exchange( other.m_Id, {} ) )
        {
        }
        EventNodeLink& operator=( EventNodeLink&& other ) noexcept
        {
            if ( this != &other )
            {
                Release();
                m_Tree = std::exchange( other.m_Tree, nullptr );
                m_Id   = std::exchange( other.m_Id, {} );
            }
            return *this;
        }

        [[nodiscard]] EventTree* Tree() const
        {
            return m_Tree;
        }
        [[nodiscard]] EventNodeId Id() const
        {
            return m_Id;
        }
        void Release()
        {
            if ( m_Tree != nullptr )
                m_Tree->Remove( m_Id );
            m_Tree = nullptr;
            m_Id   = {};
        }

    private:
        EventTree*  m_Tree = nullptr;
        EventNodeId m_Id{};
    };
} // namespace Common
