#include "MemoryReadout.hpp"

#include <Engine/Core/TextTemplate.hpp>

#include <atomic>
#include <cstdio>
#include <string_view>

namespace Desert::Graphic
{
    namespace
    {
        /// Bytes as a human reads them, beside the exact figure. Both, always: MB is what a person
        /// compares across runs and the byte count is what a test pins, and a report that carried only
        /// the rounded one could not be asserted on.
        std::string Bytes( const uint64_t bytes )
        {
            const double mb = static_cast<double>( bytes ) / ( 1024.0 * 1024.0 );
            char         text[64];
            std::snprintf( text, sizeof( text ), "%llu B (%.1f MB)", static_cast<unsigned long long>( bytes ),
                           mb );
            return text;
        }

        // THE SAMPLER'S STATE, AND WHY IT IS ATOMIC RATHER THAN MUTEXED. Every field is written by the
        // render thread in BeginFrame and read by the log, the HUD and the control channel, so the only
        // hazard is a torn 64-bit read; relaxed atomics remove it for the price of nothing. A mutex here
        // would put a lock on the frame's first instruction to protect six independent counters that
        // never have to agree with each other at an instant.
        std::atomic<uint64_t> g_Frames{ 0 };
        std::atomic<uint64_t> g_BaselineResident{ 0 };
        std::atomic<uint64_t> g_BaselineDeviceUsage{ 0 };
        std::atomic<uint64_t> g_PeakResident{ 0 };
        std::atomic<uint64_t> g_PeakDeviceUsage{ 0 };

        void RaisePeak( std::atomic<uint64_t>& peak, const uint64_t value )
        {
            uint64_t seen = peak.load( std::memory_order_relaxed );
            while ( value > seen && !peak.compare_exchange_weak( seen, value, std::memory_order_relaxed ) )
            {
            }
        }
    } // namespace

    namespace
    {
        // 1. The device, as the driver sees it. 2. The ledger: the total AND its coverage, because a subtotal
        // presented as a total is the defect this whole type exists because of. 3. The process: every byte,
        // and the one number that saw the world scene.
        constexpr std::string_view kReadoutText =
             "{% if budgetKnown %}device-local usage={{ deviceUsage }} of budget={{ deviceBudget }}"
             "{% else %}device=unknown (VK_EXT_memory_budget not enabled){% endif %} over {{ heaps }} heap(s)"
             "\n  ledger: {{ ledgerBytes }} over {{ knownFor }} of {{ live }} live objects"
             "{% if unsized > 0 %} ({{ unsized }} report no size, so this total is a FLOOR){% endif %}"
             "\n  {{ process }}{% if processKnown %} = resident {{ resident }}, peak {{ peak }}{% endif %}";

        // Both unknowns are SAID: both zero means the platform never answered, and "grew 0 B" would be read
        // as a process that did not grow.
        constexpr std::string_view kWatchText =
             "watch over {{ frames }} frame(s):"
             "\n  process resident: {% if residentKnown %}baseline {{ baseResident }} -> peak {{ peakResident }}, "
             "grew {{ residentGrowth }}{% else %}unknown (the platform query never answered){% endif %}"
             "\n  device-local usage: {% if deviceKnown %}baseline {{ baseDevice }} -> peak {{ peakDevice }}, "
             "grew {{ deviceGrowth }}{% else %}unknown (no budget extension, or nothing device-local){% endif %}";
    } // namespace

    std::string MemoryReadout::Report() const
    {
        static const Desert::Core::TextTemplate kReadout( "MemoryReadout", kReadoutText );
        Common::Json::ObjectBuilder             model;
        model.Set( "budgetKnown", Device.BudgetKnown )
             .Set( "deviceUsage", Device.BudgetKnown ? Bytes( Device.DeviceLocalUsage() ) : std::string() )
             .Set( "deviceBudget", Device.BudgetKnown ? Bytes( Device.DeviceLocalBudget() ) : std::string() )
             .Set( "heaps", Device.Heaps.size() )
             .Set( "ledgerBytes", Bytes( Ledger.Bytes ) )
             .Set( "knownFor", Ledger.BytesKnownFor )
             .Set( "live", Ledger.Live )
             .Set( "unsized", Ledger.Live > Ledger.BytesKnownFor ? Ledger.Live - Ledger.BytesKnownFor : 0 )
             .Set( "process", Process.Describe() )
             .Set( "processKnown", Process.Known )
             .Set( "resident", Bytes( Process.Resident ) )
             .Set( "peak", Bytes( Process.Peak ) );
        return kReadout.RenderOrLog( Common::Json::Value( model.Build() ) );
    }

    void MemoryWatch::SampleFrame( const MemoryReadout& readout )
    {
        const uint64_t resident    = readout.Process.Known ? readout.Process.Resident : 0;
        const uint64_t deviceUsage = readout.Device.BudgetKnown ? readout.Device.DeviceLocalUsage() : 0;

        // The baseline is the FIRST sample, claimed exactly once. `fetch_add` returning the previous
        // value is what makes "am I the first frame" a single atomic question rather than a check
        // followed by a store that two threads could both pass.
        if ( g_Frames.fetch_add( 1, std::memory_order_relaxed ) == 0 )
        {
            g_BaselineResident.store( resident, std::memory_order_relaxed );
            g_BaselineDeviceUsage.store( deviceUsage, std::memory_order_relaxed );
        }

        RaisePeak( g_PeakResident, resident );
        RaisePeak( g_PeakDeviceUsage, deviceUsage );
    }

    uint64_t MemoryWatch::FramesSampled()
    {
        return g_Frames.load( std::memory_order_relaxed );
    }

    uint64_t MemoryWatch::PeakProcessResident()
    {
        return g_PeakResident.load( std::memory_order_relaxed );
    }

    uint64_t MemoryWatch::PeakDeviceLocalUsage()
    {
        return g_PeakDeviceUsage.load( std::memory_order_relaxed );
    }

    uint64_t MemoryWatch::BaselineProcessResident()
    {
        return g_BaselineResident.load( std::memory_order_relaxed );
    }

    uint64_t MemoryWatch::BaselineDeviceLocalUsage()
    {
        return g_BaselineDeviceUsage.load( std::memory_order_relaxed );
    }

    std::string MemoryWatch::Report()
    {
        const uint64_t frames = FramesSampled();
        if ( frames == 0 )
        {
            // NOT "0 MB of growth". The detector having never run and the process having never grown are
            // different facts, and a reader given the second when the first is true stops looking.
            return "watch: never sampled (no frame has begun yet)";
        }

        const uint64_t baseResident = BaselineProcessResident();
        const uint64_t peakResident = PeakProcessResident();
        const uint64_t baseDevice   = BaselineDeviceLocalUsage();
        const uint64_t peakDevice   = PeakDeviceLocalUsage();

        static const Desert::Core::TextTemplate kWatch( "MemoryWatch", kWatchText );
        Common::Json::ObjectBuilder             model;
        model.Set( "frames", frames )
             .Set( "residentKnown", baseResident != 0 || peakResident != 0 )
             .Set( "baseResident", Bytes( baseResident ) )
             .Set( "peakResident", Bytes( peakResident ) )
             .Set( "residentGrowth", Bytes( peakResident - baseResident ) )
             .Set( "deviceKnown", baseDevice != 0 || peakDevice != 0 )
             .Set( "baseDevice", Bytes( baseDevice ) )
             .Set( "peakDevice", Bytes( peakDevice ) )
             .Set( "deviceGrowth", Bytes( peakDevice - baseDevice ) );
        return kWatch.RenderOrLog( Common::Json::Value( model.Build() ) );
    }

    void MemoryWatch::ResetForTest()
    {
        g_Frames.store( 0, std::memory_order_relaxed );
        g_BaselineResident.store( 0, std::memory_order_relaxed );
        g_BaselineDeviceUsage.store( 0, std::memory_order_relaxed );
        g_PeakResident.store( 0, std::memory_order_relaxed );
        g_PeakDeviceUsage.store( 0, std::memory_order_relaxed );
    }

} // namespace Desert::Graphic
