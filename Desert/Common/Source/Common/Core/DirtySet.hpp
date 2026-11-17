#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace Common
{
    // THE DIRTY LIST (UE: FScene's dirty primitive lists, UPrimitiveComponent::MarkRenderStateDirty queued for
    // SendAllEndOfFrameUpdates). A producer MARKS the handle whose state changed at the moment it changes; the
    // one consumer DRAINS the marks once per frame and does its work for those handles only. An unchanged
    // object costs nothing per frame: no per-object version compare, no scan of every object.
    //
    // Dense array of the marked handles (iteration in mark order) + a slot per handle index (index -> dense
    // position, kAbsent when unmarked): Mark and Contains are O(1), Drain is O(marked). A handle that is marked
    // again before the drain is not listed twice. Two handles that share an index (a recycled entity: same
    // index, new version) are both listed: the slot names the newest, the older one stays in the dense list,
    // so a destroy + create on one index within a frame delivers both.
    //
    // NO ALLOCATION PER EVENT in a steady state: both arrays keep their capacity across Drain / Clear; they
    // grow only when a frame marks more handles, or a higher index, than any frame before.
    //
    // NOT THREAD-SAFE: one writer at a time. A set fed from several threads is guarded by its owner (the Scene's
    // render-state set sits under the Scene's mutex).
    //
    // @tparam Handle  a small copyable handle compared with ==.
    // @tparam IndexOf a functor `uint32_t(Handle) noexcept` giving the handle's dense index (an entity's index
    //                 without its version; an integral or an enum handle is its own index by default).
    template <typename Handle>
    struct DirtySetIndexOf
    {
        [[nodiscard]] uint32_t operator()( const Handle handle ) const noexcept
        {
            static_assert( std::is_integral_v<Handle> || std::is_enum_v<Handle>,
                           "DirtySet: give a handle that is not integral an IndexOf functor" );
            return static_cast<uint32_t>( handle );
        }
    };

    template <typename Handle, typename IndexOf = DirtySetIndexOf<Handle>>
    class DirtySet
    {
    public:
        static constexpr uint32_t kAbsent = UINT32_MAX;

        // Marks @p handle; returns true when it was not marked yet.
        bool Mark( const Handle handle ) noexcept
        {
            const uint32_t index = IndexOf{}( handle );
            if ( index >= m_Slot.size() )
                m_Slot.resize( GrownSize( index ), kAbsent );
            uint32_t& slot = m_Slot[index];
            if ( slot != kAbsent && m_Dense[slot] == handle )
                return false;
            slot = static_cast<uint32_t>( m_Dense.size() );
            m_Dense.push_back( handle );
            return true;
        }

        [[nodiscard]] bool Contains( const Handle handle ) const noexcept
        {
            const uint32_t index = IndexOf{}( handle );
            if ( index >= m_Slot.size() )
                return false;
            const uint32_t slot = m_Slot[index];
            return slot != kAbsent && m_Dense[slot] == handle;
        }

        [[nodiscard]] bool Empty() const noexcept
        {
            return m_Dense.empty();
        }

        [[nodiscard]] size_t Size() const noexcept
        {
            return m_Dense.size();
        }

        // The marked handles in mark order, until the next Mark / Drain / Clear.
        [[nodiscard]] const std::vector<Handle>& Marked() const noexcept
        {
            return m_Dense;
        }

        // Unmarks everything; capacities kept.
        void Clear() noexcept
        {
            for ( const Handle handle : m_Dense )
                m_Slot[IndexOf{}( handle )] = kAbsent;
            m_Dense.clear();
        }

        // Calls @p visit( handle ) for every handle marked before the call, in mark order, and unmarks them. A
        // handle @p visit marks (again) stays marked for the next drain: a change made while the consumer works
        // is never lost and never visited twice in one drain.
        template <typename Visit>
        void Drain( Visit&& visit ) noexcept
        {
            const size_t count = m_Dense.size();
            for ( size_t i = 0; i < count; ++i )
            {
                // Unmarked before the visit, so a re-mark from inside it lands after `count`.
                const Handle handle = m_Dense[i];
                uint32_t&    slot   = m_Slot[IndexOf{}( handle )];
                if ( slot == static_cast<uint32_t>( i ) )
                    slot = kAbsent;
                visit( handle );
            }
            if ( m_Dense.size() == count )
            {
                m_Dense.clear();
                return;
            }
            // Marks made during the visits move to the front; their slots follow them.
            m_Dense.erase( m_Dense.begin(), m_Dense.begin() + static_cast<std::ptrdiff_t>( count ) );
            for ( size_t i = 0; i < m_Dense.size(); ++i )
                m_Slot[IndexOf{}( m_Dense[i] )] = static_cast<uint32_t>( i );
        }

        // Appends the marked handles to @p out (its capacity is the caller's to keep) and unmarks them.
        void DrainInto( std::vector<Handle>& out ) noexcept
        {
            out.insert( out.end(), m_Dense.begin(), m_Dense.end() );
            Clear();
        }

    private:
        [[nodiscard]] size_t GrownSize( const uint32_t index ) const noexcept
        {
            size_t size = m_Slot.empty() ? size_t{ 64 } : m_Slot.size();
            while ( size <= index )
                size *= 2;
            return size;
        }

        std::vector<Handle>   m_Dense;
        std::vector<uint32_t> m_Slot;
    };
} // namespace Common
