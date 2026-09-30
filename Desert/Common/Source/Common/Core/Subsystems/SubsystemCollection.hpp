#pragma once

#include <Common/Core/Events/EventTree.hpp>

#include <concepts>
#include <cstddef>
#include <memory>
#include <vector>

#define DESERT_SUBSYSTEM( Owner )

namespace Common
{
    template <typename Owner>
    class SubsystemCollection
    {
    public:
        SubsystemCollection( Owner& owner, EventTree& events, EventNodeId parent )
             : m_Owner( &owner ), m_Events( &events ), m_Parent( parent )
        {
            CreateSubsystems( *this );
        }

        ~SubsystemCollection()
        {
            while ( !m_Entries.empty() )
                m_Entries.pop_back();
        }

        SubsystemCollection( const SubsystemCollection& )            = delete;
        SubsystemCollection& operator=( const SubsystemCollection& ) = delete;
        SubsystemCollection( SubsystemCollection&& )                 = delete;
        SubsystemCollection& operator=( SubsystemCollection&& )      = delete;

        template <typename T>
        T& Create()
        {
            std::unique_ptr<T> created = Construct<T>();
            T&                 object  = *created;
            EventNodeLink      node;
            if constexpr ( ReceivesEvents<T> )
                node = EventNodeLink( *m_Events, m_Events->Attach<T>( m_Parent, object ) );
            m_Entries.push_back( Entry{ &EventDetail::TypeTag<T>, ErasedObject( created.release(), &DestroyAs<T> ),
                                        std::move( node ) } );
            return object;
        }

        template <typename T>
        [[nodiscard]] T* Get() const
        {
            for ( const Entry& entry : m_Entries )
            {
                if ( entry.Type == &EventDetail::TypeTag<T> )
                    return static_cast<T*>( entry.Object.get() );
            }
            return nullptr;
        }

        template <typename T>
        [[nodiscard]] EventNodeId NodeOf() const
        {
            for ( const Entry& entry : m_Entries )
            {
                if ( entry.Type == &EventDetail::TypeTag<T> )
                    return entry.Node.Id();
            }
            return {};
        }

        [[nodiscard]] std::size_t Count() const
        {
            return m_Entries.size();
        }

        [[nodiscard]] Owner& GetOwner() const
        {
            return *m_Owner;
        }

    private:
        using ErasedObject = std::unique_ptr<void, void ( * )( void* )>;

        struct Entry
        {
            const void*   Type = nullptr;
            ErasedObject  Object;
            EventNodeLink Node;
        };

        template <typename T>
        static void DestroyAs( void* object )
        {
            delete static_cast<T*>( object );
        }

        template <typename T>
        std::unique_ptr<T> Construct()
        {
            if constexpr ( std::constructible_from<T, Owner&> )
                return std::make_unique<T>( *m_Owner );
            else
                return std::make_unique<T>();
        }

        Owner*             m_Owner  = nullptr;
        EventTree*         m_Events = nullptr;
        EventNodeId        m_Parent{};
        std::vector<Entry> m_Entries;
    };
} // namespace Common
