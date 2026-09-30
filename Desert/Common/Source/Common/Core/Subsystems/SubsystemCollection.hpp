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
    inline constexpr bool SubsystemsJoinEventTree = true;

    template <typename Owner, typename T>
    concept SubsystemOf = !ReceivesEvents<T> || SubsystemsJoinEventTree<Owner>;

    template <typename Owner>
    class SubsystemCollection
    {
    public:
        SubsystemCollection( Owner& owner, EventTree& events, EventNodeId parent )
            requires SubsystemsJoinEventTree<Owner>
             : m_Owner( &owner ), m_Events( &events ), m_Parent( parent )
        {
            CreateSubsystems( *this );
        }

        explicit SubsystemCollection( Owner& owner )
            requires( !SubsystemsJoinEventTree<Owner> )
             : m_Owner( &owner )
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
            static_assert(
                 SubsystemOf<Owner, T>,
                 "this owner is not a node of an event tree: its subsystems cannot handle routed events" );
            std::unique_ptr<HolderOf<T>> created = Construct<T>();
            T&                           object  = created->Value;
            EventNodeLink                node;
            if constexpr ( ReceivesEvents<T> )
                node = EventNodeLink( *m_Events, m_Events->Attach<T>( m_Parent, object ) );
            m_Entries.push_back( Entry{ std::move( created ), std::move( node ) } );
            return object;
        }

        template <typename T>
        [[nodiscard]] T* Get() const
        {
            for ( const Entry& entry : m_Entries )
            {
                if ( auto* holder = dynamic_cast<HolderOf<T>*>( entry.Object.get() ) )
                    return &holder->Value;
            }
            return nullptr;
        }

        template <typename T>
        [[nodiscard]] EventNodeId NodeOf() const
        {
            for ( const Entry& entry : m_Entries )
            {
                if ( dynamic_cast<const HolderOf<T>*>( entry.Object.get() ) != nullptr )
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
        struct Holder
        {
            Holder()                           = default;
            virtual ~Holder()                  = default;
            Holder( const Holder& )            = delete;
            Holder& operator=( const Holder& ) = delete;
            Holder( Holder&& )                 = delete;
            Holder& operator=( Holder&& )      = delete;
        };

        template <typename T>
        struct HolderOf final : Holder
        {
            HolderOf() : Value()
            {
            }

            explicit HolderOf( Owner& owner ) : Value( owner )
            {
            }

            T Value;
        };

        struct Entry
        {
            std::unique_ptr<Holder> Object;
            EventNodeLink           Node;
        };

        template <typename T>
        std::unique_ptr<HolderOf<T>> Construct()
        {
            if constexpr ( std::constructible_from<T, Owner&> )
                return std::make_unique<HolderOf<T>>( *m_Owner );
            else
                return std::make_unique<HolderOf<T>>();
        }

        Owner*             m_Owner  = nullptr;
        EventTree*         m_Events = nullptr;
        EventNodeId        m_Parent{};
        std::vector<Entry> m_Entries;
    };
} // namespace Common
