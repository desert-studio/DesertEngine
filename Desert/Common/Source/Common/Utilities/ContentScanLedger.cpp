#include <Common/Utilities/ContentScanLedger.hpp>

#include <atomic>
#include <cstdio>

namespace Common::Utils::ContentScanLedger
{
    namespace
    {
        // ATOMICS RATHER THAN A MUTEX, and the choice follows from what the counter is for. A walk is
        // seconds of syscalls; contending a lock around three increments would be free either way, but
        // atomics make `NoteWalk` callable from a JobSystem worker without the caller having to know
        // whether the primitive it is inside takes a lock. `Milliseconds` is a double, which has no
        // lock-free atomic add on every target, so it is accumulated with a compare-exchange loop —
        // twenty instructions once per WALK, against a walk that costs millions.
        std::atomic<uint64_t> s_Walks{ 0 };
        std::atomic<uint64_t> s_Entries{ 0 };
        std::atomic<double>   s_Milliseconds{ 0.0 };
    } // namespace

    void NoteWalk( std::size_t entries, double milliseconds )
    {
        s_Walks.fetch_add( 1, std::memory_order_relaxed );
        s_Entries.fetch_add( static_cast<uint64_t>( entries ), std::memory_order_relaxed );

        double expected = s_Milliseconds.load( std::memory_order_relaxed );
        while ( !s_Milliseconds.compare_exchange_weak( expected, expected + milliseconds,
                                                       std::memory_order_relaxed ) )
        {
        }
    }

    Readout Take()
    {
        Readout readout;
        readout.Walks        = s_Walks.load( std::memory_order_relaxed );
        readout.Entries      = s_Entries.load( std::memory_order_relaxed );
        readout.Milliseconds = s_Milliseconds.load( std::memory_order_relaxed );
        return readout;
    }

    std::string Report()
    {
        const Readout readout = Take();

        // THE ZERO CASE SAYS SOMETHING DIFFERENT FROM "0 walk(s) over 0 entries in 0.00 ms", and the
        // difference is the whole point of this line: zero walks is the state the cooked registry
        // exists to produce, so it is reported as a claim rather than as three empty quantities a
        // reader has to interpret.
        if ( readout.Walks == 0 )
            return "no directory walk happened — every content identity came from the cooked registry";

        std::string text = std::to_string( readout.Walks ) + " directory walk(s) over " +
                           std::to_string( readout.Entries ) + " entr(ies) in ";
        // Two decimals, spelled the way SyncLoadLedger spells its own, so the three boot lines can be
        // read as one paragraph.
        char millis[32] = {};
        std::snprintf( millis, sizeof( millis ), "%.2f", readout.Milliseconds );
        text += millis;
        text += " ms";
        return text;
    }

    void Reset()
    {
        s_Walks.store( 0, std::memory_order_relaxed );
        s_Entries.store( 0, std::memory_order_relaxed );
        s_Milliseconds.store( 0.0, std::memory_order_relaxed );
    }
} // namespace Common::Utils::ContentScanLedger
