#include "HandlePool.hpp"

#include <cstdint>

namespace Common::Core
{
    Handle HandlePool::Allocate()
    {
        uint32_t index;

        if ( !m_FreeList.empty() )
        {
            index = m_FreeList.back();
            m_FreeList.pop_back();
        }
        else
        {
            index = static_cast<uint32_t>( m_Entries.size() );
            m_Entries.emplace_back();
        }

        Entry& entry = m_Entries[index];
        entry.Generation++;
        entry.Alive = true;

        return Handle{ index, entry.Generation };
    }

    void HandlePool::Release( const Handle& handle )
    {
        if ( handle.Index >= m_Entries.size() )
            return;

        Entry& entry = m_Entries[handle.Index];

        if ( !entry.Alive || entry.Generation != handle.Generation )
            return;

        entry.Alive = false;
        entry.Generation++;

        m_FreeList.push_back( handle.Index );
    }

    void HandlePool::Clear()
    {
        m_Entries.clear();
        m_FreeList.clear();
    }
} // namespace Common::Core
