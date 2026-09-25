#pragma once

#include <atomic>
#include <cstdint>

namespace Desert::Graphic
{
    // Versions for material-property writes. A write stamps the property with a fresh version; every
    // per-view copy (a uniform buffer's copy, a view's descriptor sets) remembers the version it last
    // applied, and "dirty for this view" is simply `applied != current`.
    //
    // This replaced a per-renderer-slot countdown ("stay dirty for frames x slots frames, clean at most
    // once per frame"). The countdown drained on whichever view recorded, so two views sharing a slot, or
    // a view that did not record for longer than the window, silently kept the old value. A version cannot
    // drain: a copy that has not applied a write is behind until it does, however late that is.
    namespace PropertyVersion
    {
        // No write is ever stamped with this, so a copy that has applied nothing (a new one) is behind
        // every write, and a field nobody has written is behind nothing.
        inline constexpr uint64_t kNeverWritten = 0;

        // A fresh version, greater than every version handed out before. PROCESS-WIDE rather than per
        // property, so the versions of different fields of one block are comparable and a single number per
        // copy ("the newest version I applied") answers for all of them. Thread-safe: materials are built on
        // loader threads too. 64 bits cannot wrap within the life of a process.
        [[nodiscard]] inline uint64_t Next() noexcept
        {
            static std::atomic<uint64_t> counter{ kNeverWritten };
            return counter.fetch_add( 1, std::memory_order_relaxed ) + 1;
        }
    } // namespace PropertyVersion
} // namespace Desert::Graphic
