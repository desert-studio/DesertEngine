#pragma once

#include <Common/Core/Events/EventHandler.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace Common
{
    struct EventNodeId
    {
        static constexpr uint32_t kNone = UINT32_MAX;

        uint32_t Index      = kNone;
        uint32_t Generation = 0;

        bool IsSet() const
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
            std::vector<Doomed> doomed;
            for ( uint32_t child : std::vector<uint32_t>( m_Slots[0].Children ) )
                Kill( child, doomed );
            DestroyAll( doomed );
            FlushGraveyard();
        }

        EventTree( const EventTree& )            = delete;
        EventTree& operator=( const EventTree& ) = delete;
        EventTree( EventTree&& )                 = delete;
        EventTree& operator=( EventTree&& )      = delete;

        EventNodeId Root() const
        {
            return { 0, m_Slots[0].Generation };
        }

        template <typename T, typename... Args>
        EmplacedNode<T> Emplace( EventNodeId parent, Args&&... args )
        {
            T* object = new T( std::forward<Args>( args )... );
            return { Insert<T>( parent, object, &DestroyAs<T> ), *object };
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
            std::vector<Doomed> doomed;
            Kill( id.Index, doomed );
            if ( m_DispatchDepth > 0 )
                m_Graveyard.insert( m_Graveyard.end(), doomed.begin(), doomed.end() );
            else
                DestroyAll( doomed );
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
            for ( uint32_t child : std::vector<uint32_t>( m_Slots[id.Index].Children ) )
                Remove( { child, m_Slots[child].Generation } );
            T* object                = static_cast<T*>( m_Slots[id.Index].Object );
            m_Slots[id.Index].Object = nullptr;
            Unlink( id.Index );
            std::vector<Doomed> none;
            Kill( id.Index, none );
            return std::unique_ptr<T>( object );
        }

        bool Contains( EventNodeId id ) const
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

        EventNodeId Parent( EventNodeId id ) const
        {
            if ( !IsLive( id ) || id.Index == 0 )
                return {};
            const uint32_t parent = m_Slots[id.Index].Parent;
            return { parent, m_Slots[parent].Generation };
        }

        const EventHandlerTable* HandlerTableOf( EventNodeId id ) const
        {
            return IsLive( id ) ? m_Slots[id.Index].Table : nullptr;
        }

        std::size_t Size() const
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
        EventNodeId Focus() const
        {
            return IsLive( m_Focus ) ? m_Focus : EventNodeId{};
        }
        EventNodeId Hovered() const
        {
            return IsLive( m_Hovered ) ? m_Hovered : EventNodeId{};
        }
        EventNodeId Capture() const
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
                const EventNodeId target = EventRoute<E> == EventRouteKind::Focus ? Focus()
                                           : Capture().IsSet()                    ? Capture()
                                                                                  : Hovered();
                if ( !target.IsSet() )
                    return {};
                CollectChain( target.Index, path );

                for ( auto it = path.rbegin(); it != path.rend(); ++it )
                {
                    if ( CallPreview( *it, event ) )
                        return { true, *it, EventPhase::Preview };
                }
                for ( const EventNodeId id : path )
                {
                    if ( CallBubble( id, event ) )
                        return { true, id, EventPhase::Bubble };
                }
                return {};
            }
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
            uint32_t                 Generation = 0;
            uint32_t                 VisitStamp = 0;
            bool                     Alive      = false;
            std::vector<uint32_t>    Children;
        };

        struct Doomed
        {
            void* Object;
            void ( *Destroy )( void* );
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

        bool IsLive( EventNodeId id ) const
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
            delete static_cast<T*>( object );
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
            if ( !m_Free.empty() )
            {
                index = m_Free.back();
                m_Free.pop_back();
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

        void Kill( uint32_t index, std::vector<Doomed>& doomed )
        {
            for ( uint32_t child : m_Slots[index].Children )
                Kill( child, doomed );
            Slot& slot = m_Slots[index];
            if ( slot.Object != nullptr && slot.Destroy != nullptr )
                doomed.push_back( { slot.Object, slot.Destroy } );
            slot.Object = nullptr;
            slot.Alive  = false;
            ++slot.Generation;
            slot.Children.clear();
            m_Free.push_back( index );
            --m_LiveCount;
        }

        static void DestroyAll( const std::vector<Doomed>& doomed )
        {
            for ( const Doomed& d : doomed )
                d.Destroy( d.Object );
        }

        void FlushGraveyard()
        {
            while ( !m_Graveyard.empty() )
            {
                std::vector<Doomed> doomed = std::move( m_Graveyard );
                m_Graveyard.clear();
                DestroyAll( doomed );
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
            chain( Capture().IsSet() ? Capture() : Hovered() );

            m_Stack.clear();
            m_Stack.push_back( 0 );
            while ( !m_Stack.empty() )
            {
                const uint32_t index = m_Stack.back();
                m_Stack.pop_back();
                visit( index );
                const std::vector<uint32_t>& children = m_Slots[index].Children;
                for ( auto it = children.rbegin(); it != children.rend(); ++it )
                    m_Stack.push_back( *it );
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

        std::vector<Slot>                     m_Slots;
        std::vector<uint32_t>                 m_Free;
        std::vector<Doomed>                   m_Graveyard;
        std::vector<std::vector<EventNodeId>> m_PathBuffers;
        std::vector<uint32_t>                 m_Stack;
        std::size_t                           m_LiveCount     = 0;
        std::size_t                           m_DispatchDepth = 0;
        uint32_t                              m_VisitStamp    = 0;
        EventNodeId                           m_Focus{};
        EventNodeId                           m_Hovered{};
        EventNodeId                           m_Capture{};
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

        EventTree* Tree() const
        {
            return m_Tree;
        }
        EventNodeId Id() const
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
