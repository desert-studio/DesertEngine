#include "MemoryReadout.hpp"

#include <Engine/Core/EngineContext.hpp>

namespace Desert::Graphic
{
    // WHY `Take()` IS IN ITS OWN TRANSLATION UNIT, AWAY FROM THE REST OF THE TYPE.
    //
    // It is the only impure part of the readout: it asks the device, and reaching the device means
    // `EngineContext`, which includes `Application`, `Window` and `RendererContext` — the whole engine.
    // Everything else about this type is a decision rather than a query: which of three numbers is
    // printed, whether a zero means "empty" or "nobody answered", how a peak is folded, what growth is
    // measured against. Those are exactly the parts a defect would live in, and a suite that had to link
    // the whole renderer to reach them could not exist — so the assertions would have been written
    // against a running editor, or not at all.
    //
    // The same split, for the same reason, as `AssetEvictionServices.cpp` beside `AssetEviction.cpp`.

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

    MemoryReadout MemoryReadout::TakeFrameSample()
    {
        MemoryReadout out;

        if ( const std::shared_ptr<Engine::Device> device = EngineContext::GetInstance().GetDevice() )
        {
            out.Device = device->QueryMemory();
        }
        out.Process = Common::Utils::ReadProcessFootprint();

        // AND NO `ResourceLedger::Take()` HERE, ON PURPOSE. See the declaration: a locked walk over
        // every live device object, once a frame, for two fields the watch does not track.
        return out;
    }

} // namespace Desert::Graphic
