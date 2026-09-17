#include "MemoryReadout.hpp"

#include <Engine/Core/EngineContext.hpp>

#include <atomic>
#include <cstdio>

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

    MemoryReadout MemoryReadout::Take()
    {
        MemoryReadout out;

        // NULL-CHECKED, and this is what makes the type testable. A suite has no device and no window;
        // if this dereferenced unconditionally, the only place the readout could be exercised would be a
        // running editor, and the assertion that it reports "unknown" instead of zero — the whole point
        // of the `BudgetKnown` flag — would have nowhere to live.
        if ( const std::shared_ptr<Engine::Device> device = EngineContext::GetInstance().GetDevice() )
        {
            out.Device = device->QueryMemory();
        }

        out.Ledger  = ResourceLedger::Take();
        out.Process = Common::Utils::ReadProcessFootprint();

        return out;
    }

    std::string MemoryReadout::Report() const
    {
        std::string text;

        // 1. The device, as the driver sees it.
        if ( !Device.BudgetKnown )
        {
            text += "device=unknown (VK_EXT_memory_budget not enabled)";
        }
        else
        {
            text += "device-local usage=" + Bytes( Device.DeviceLocalUsage() ) + " of budget=" +
                    Bytes( Device.DeviceLocalBudget() );
        }
        text += " over " + std::to_string( Device.Heaps.size() ) + " heap(s)";

        // 2. The ledger — the total AND its coverage, because a subtotal presented as a total is the
        // defect this whole type exists because of.
        text += "\n  ledger: " + Bytes( Ledger.Bytes ) + " over " + std::to_string( Ledger.BytesKnownFor ) +
                " of " + std::to_string( Ledger.Live ) + " live objects";
        if ( Ledger.Live > Ledger.BytesKnownFor )
        {
            text += " (" + std::to_string( Ledger.Live - Ledger.BytesKnownFor ) +
                    " report no size, so this total is a FLOOR)";
        }

        // 3. The process — every byte, and the one number that saw the world scene.
        text += "\n  " + Process.Describe();
        if ( Process.Known )
        {
            text += " = resident " + Bytes( Process.Resident ) + ", peak " + Bytes( Process.Peak );
        }

        return text;
    }

    void MemoryWatch::SampleFrame()
    {
        const MemoryReadout readout = MemoryReadout::Take();

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

        std::string text = "watch over " + std::to_string( frames ) + " frame(s):";
        if ( baseResident == 0 && peakResident == 0 )
        {
            // Same rule as the device line below: both zero means the platform never answered, and
            // "grew 0 B" would be read as a process that did not grow.
            text += "\n  process resident: unknown (the platform query never answered)";
        }
        else
        {
            text += "\n  process resident: baseline " + Bytes( baseResident ) + " -> peak " +
                    Bytes( peakResident ) + ", grew " + Bytes( peakResident - baseResident );
        }
        if ( peakDevice == 0 && baseDevice == 0 )
        {
            text += "\n  device-local usage: unknown (no budget extension, or nothing device-local)";
        }
        else
        {
            text += "\n  device-local usage: baseline " + Bytes( baseDevice ) + " -> peak " +
                    Bytes( peakDevice ) + ", grew " + Bytes( peakDevice - baseDevice );
        }
        return text;
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
