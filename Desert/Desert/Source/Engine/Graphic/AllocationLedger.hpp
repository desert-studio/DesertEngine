#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Desert::Graphic
{
    /**
     * @brief Every live GPU allocation by the tag its creator gave it, and the bytes behind each tag.
     *
     * WHY IT EXISTS: the device's own usage number (VK_EXT_memory_budget) says THAT memory grew and never
     * which object holds it. RT2k found 8.16 MiB growing per open-close of a view while every view's ledger
     * (ViewResources, SceneRenderer::HeldBytes) stayed flat — so the leak was by definition outside what views
     * account for, and the only way to name it was a census of the allocator itself. A leak then reads as one
     * tag whose count climbs by one per cycle.
     *
     * Device-free on purpose: the allocator records opaque handle values, and the rule — a row is added at
     * creation and removed when the object is really destroyed (not when it is queued) — is testable
     * without a device.
     */
    class AllocationLedger
    {
    public:
        struct TagTotal
        {
            std::string Tag;
            std::size_t Count = 0;
            std::size_t Bytes = 0;
        };

        void Record( const uint64_t handle, std::string tag, const std::size_t bytes )
        {
            const std::scoped_lock lock( m_Mutex );
            m_Rows[handle] = Row{ std::move( tag ), bytes };
        }

        // True when the handle was live. A false answer is not an error here — allocations made before the
        // ledger existed, or through a path that does not record, are simply not counted.
        bool Release( const uint64_t handle )
        {
            const std::scoped_lock lock( m_Mutex );
            return m_Rows.erase( handle ) > 0;
        }

        [[nodiscard]] std::size_t LiveCount() const
        {
            const std::scoped_lock lock( m_Mutex );
            return m_Rows.size();
        }

        [[nodiscard]] std::size_t LiveBytes() const
        {
            const std::scoped_lock lock( m_Mutex );
            std::size_t            total = 0;
            for ( const auto& [handle, row] : m_Rows )
                total += row.Bytes;
            return total;
        }

        // Largest first, ties by name, so two readings of the same state print in the same order.
        [[nodiscard]] std::vector<TagTotal> ByTag() const
        {
            std::map<std::string, TagTotal> totals;
            {
                const std::scoped_lock lock( m_Mutex );
                for ( const auto& [handle, row] : m_Rows )
                {
                    TagTotal& total = totals[row.Tag];
                    total.Tag       = row.Tag;
                    ++total.Count;
                    total.Bytes += row.Bytes;
                }
            }
            std::vector<TagTotal> out;
            out.reserve( totals.size() );
            for ( auto& [tag, total] : totals )
                out.push_back( std::move( total ) );
            std::stable_sort( out.begin(), out.end(),
                              []( const TagTotal& a, const TagTotal& b ) { return a.Bytes > b.Bytes; } );
            return out;
        }

    private:
        struct Row
        {
            std::string Tag;
            std::size_t Bytes = 0;
        };

        mutable std::mutex                m_Mutex;
        std::unordered_map<uint64_t, Row> m_Rows;
    };
} // namespace Desert::Graphic
