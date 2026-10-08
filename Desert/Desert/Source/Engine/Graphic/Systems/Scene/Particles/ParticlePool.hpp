#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

namespace Desert::Graphic::System
{
    // THE WORLD'S PARTICLE POOL, the device-free half (VFX-07; port of the bookkeeping UE's Niagara GPU dispatcher
    // keeps per emitter instance - FNiagaraGPUInstanceCountManager hands each instance its count slot, the data
    // buffer holds its particles - folded into ONE pool for the scene's emitters).
    //
    // One persistent GPU pool holds every emitter's particles; each emitter owns a contiguous RANGE of it
    // [Base, Base + Count), Count = its MaxParticles. The free list and the alive list are pool-sized arrays of
    // absolute pool indices; an emitter's entries live in the same range of each list (entry k of its alive list
    // is at AliveList[Base + k]). ParticlePoolRanges hands the ranges out; ParticlePoolLists is the C++ statement
    // of what the two compute passes do with the lists (Programs/Particles/ParticleCompact.shader,
    // ParticleSimulate.shader), which the device-free suite checks.

    struct ParticlePoolRange
    {
        uint32_t Base  = 0;
        uint32_t Count = 0;
    };

    // First-fit range allocator over the pool, keyed by the emitter (ParticleRenderer: the entity id). A released
    // range returns to the free ranges (coalesced with its neighbours); a request no free range fits is appended
    // at the end, which raises End - the size the GPU pool must have.
    class ParticlePoolRanges
    {
    public:
        // The range of @p key, (re)allocated when it has none or a different count.
        ParticlePoolRange Acquire( const uint32_t key, const uint32_t count )
        {
            if ( const auto it = m_Ranges.find( key ); it != m_Ranges.end() )
            {
                if ( it->second.Count == count )
                    return it->second;
                Release( key );
            }
            ParticlePoolRange range{ m_End, count };
            for ( auto free = m_Free.begin(); free != m_Free.end(); ++free )
            {
                if ( free->Count < count )
                    continue;
                range.Base = free->Base;
                free->Base += count;
                free->Count -= count;
                if ( free->Count == 0 )
                    m_Free.erase( free );
                m_Ranges[key] = range;
                return range;
            }
            m_End += count;
            m_Ranges[key] = range;
            return range;
        }

        void Release( const uint32_t key )
        {
            const auto it = m_Ranges.find( key );
            if ( it == m_Ranges.end() )
                return;
            const ParticlePoolRange range = it->second;
            m_Ranges.erase( it );
            const auto at       = std::lower_bound( m_Free.begin(), m_Free.end(), range,
                                                    []( const ParticlePoolRange& a, const ParticlePoolRange& b )
                                                    { return a.Base < b.Base; } );
            const auto inserted = m_Free.insert( at, range );
            const auto index    = static_cast<size_t>( inserted - m_Free.begin() );
            // Coalesce with the next, then with the previous free range.
            if ( index + 1 < m_Free.size() && m_Free[index].Base + m_Free[index].Count == m_Free[index + 1].Base )
            {
                m_Free[index].Count += m_Free[index + 1].Count;
                m_Free.erase( m_Free.begin() + static_cast<std::ptrdiff_t>( index + 1 ) );
            }
            if ( index > 0 && m_Free[index - 1].Base + m_Free[index - 1].Count == m_Free[index].Base )
            {
                m_Free[index - 1].Count += m_Free[index].Count;
                m_Free.erase( m_Free.begin() + static_cast<std::ptrdiff_t>( index ) );
            }
        }

        // Releases every key @p keep says no to (the emitters whose entity is gone).
        template <typename Keep>
        void ReleaseUnless( Keep&& keep )
        {
            std::vector<uint32_t> gone;
            for ( const auto& [key, range] : m_Ranges )
                if ( !keep( key ) )
                    gone.push_back( key );
            for ( const uint32_t key : gone )
                Release( key );
        }

        void Clear()
        {
            m_Ranges.clear();
            m_Free.clear();
            m_End = 0;
        }

        // One past the highest index any range has used: the pool holds at least this many particles.
        [[nodiscard]] uint32_t End() const
        {
            return m_End;
        }
        [[nodiscard]] size_t RangeCount() const
        {
            return m_Ranges.size();
        }

    private:
        std::map<uint32_t, ParticlePoolRange> m_Ranges;
        std::vector<ParticlePoolRange>        m_Free; // sorted by Base, never two adjacent
        uint32_t                              m_End = 0;
    };

    // One emitter's lists, as the passes keep them (the GPU writes these through atomics; the ORDER of the entries
    // a compact produces is the GPU's, so nothing reads an order - only membership and counts):
    //   * Compact rebuilds both from the pool: every index of the range is in exactly one of them, alive when its
    //     particle's lifetime is positive.
    //   * Spawn+Update of a step updates alive entry t for thread t < AliveCount, and spawns spawn t < min(budget,
    //     FreeCount) into FreeList[FreeCount - 1 - t] - never an index the compact found alive, and never one
    //     index twice in a step.
    class ParticlePoolLists
    {
    public:
        explicit ParticlePoolLists( const ParticlePoolRange range ) : m_Range( range )
        {
        }

        // ParticleCompact over @p alive (indexed by absolute pool index; the range's entries are read).
        void Compact( const std::vector<bool>& alive )
        {
            m_Alive.clear();
            m_Free.clear();
            for ( uint32_t i = m_Range.Base; i < m_Range.Base + m_Range.Count; ++i )
                ( alive[i] ? m_Alive : m_Free ).push_back( i );
        }

        // The indices ParticleSimulate's spawn threads write for a step budget of @p budget.
        [[nodiscard]] std::vector<uint32_t> SpawnIndices( const uint32_t budget ) const
        {
            const auto            spawned = static_cast<uint32_t>( std::min<size_t>( budget, m_Free.size() ) );
            std::vector<uint32_t> out;
            out.reserve( spawned );
            for ( uint32_t t = 0; t < spawned; ++t )
                out.push_back( m_Free[m_Free.size() - 1 - t] );
            return out;
        }

        [[nodiscard]] const std::vector<uint32_t>& Alive() const
        {
            return m_Alive;
        }
        [[nodiscard]] const std::vector<uint32_t>& Free() const
        {
            return m_Free;
        }

    private:
        ParticlePoolRange     m_Range;
        std::vector<uint32_t> m_Alive;
        std::vector<uint32_t> m_Free;
    };
} // namespace Desert::Graphic::System
