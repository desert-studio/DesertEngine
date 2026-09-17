#pragma once

#include <Common/Utilities/ProcessMemory.hpp>
#include <Engine/Core/Device.hpp>
#include <Engine/Graphic/ResourceLedger.hpp>

#include <cstdint>
#include <string>

namespace Desert::Graphic
{
    /**
     * @brief THREE MEMORY NUMBERS, NAMED SEPARATELY, BECAUSE ONE NUMBER WAS BLIND.
     *
     * THE MEASUREMENT THAT MADE THIS NECESSARY. `ResourceLedger::Report()` is the only memory figure the
     * engine printed by itself, and between the control scene and a 50 179-entity world it moved by
     * 677 829 bytes — 0.15 % — while the process grew by 315 MB (419.5 -> 734.6 MB peak resident). The
     * ledger was not lying: it counts device objects, and a world of 50 000 cubes referencing one mesh
     * adds almost no device objects. It was answering a question nobody was asking. The ledger's own
     * header had already named the alternative and dismissed it — "the only instrument for it was the
     * process's RSS, which mixes device memory, the asset layer's CPU copies and the allocator's own
     * slack" — and the conclusion drawn from that was to count one of the three. The conclusion this type
     * draws instead is to count all three and SAY WHICH IS WHICH, because "mixes three things" is an
     * argument for separating them, not for dropping two.
     *
     * The three, and what each can and cannot see:
     *
     *  1. **Device heaps** (`Engine::DeviceMemoryReport`, from `VK_EXT_memory_budget`) — what the DRIVER
     *     thinks this process holds on the device. The only one of the three that includes the driver's
     *     own allocations and its padding, and the only one that knows the budget we are spending
     *     against. Not invariant, by specification; re-queried, never stored.
     *  2. **The ledger** (`ResourceCensus`) — what WE think we hold on the device, attributable to an
     *     owner and to an asset. Its value is not the total but the ATTRIBUTION: it is the only one of
     *     the three that can answer "whose". It is also the only one whose coverage is partial, and it
     *     reports that coverage (`known for k of N`) rather than presenting a subtotal as a total.
     *  3. **The process** (`Common::Utils::ProcessFootprint`) — every byte, device and host alike. The
     *     only one of the three that saw the 315 MB.
     *
     * WHAT IS DELIBERATELY NOT COMPUTED HERE: `Process.Resident - Device.Usage`, "the host-side bytes".
     * It is the number a reader wants and it is not derivable on this machine. Apple silicon has one
     * physical pool, so a Metal allocation backing a Vulkan buffer is counted by BOTH the heap usage and
     * the process footprint, and the subtraction would remove real host bytes along with the double
     * count. On a discrete Windows GPU the same subtraction is roughly right. A number that means two
     * different things on the development machine and the target platform is worse than no number, so
     * the three are printed side by side and the reader is told they overlap.
     */
    struct MemoryReadout
    {
        Engine::DeviceMemoryReport      Device;
        ResourceCensus                  Ledger;
        Common::Utils::ProcessFootprint Process;

        /**
         * @brief Three live queries. Cached by nothing, at any layer.
         *
         * Safe before the device exists: `EngineContext` has no device during static initialisation and
         * in every test binary, and this then reports `Device.BudgetKnown == false` with no heaps rather
         * than dereferencing a null. The ledger and the process footprint answer regardless, which is
         * what makes this callable from a suite that never opened a window.
         */
        [[nodiscard]] static MemoryReadout Take();

        /// One line per number, for the log and the control channel. Says "unknown" wherever a source
        /// declined to answer — never 0, which a reader would take for "empty".
        [[nodiscard]] std::string Report() const;
    };

    /**
     * @brief THE PER-FRAME SAMPLER, AND THE HIGH-WATER MARK THAT MAKES GROWTH A FACT.
     *
     * WHY A SAMPLER AT ALL, WHEN `MemoryReadout::Take()` IS ALREADY CHEAP. Because the question is not
     * "how much now" but "did it grow", and a single instantaneous reading cannot answer that. Resident
     * size FALLS under memory pressure with nothing having been freed, so a reading taken after the
     * pressure reports a smaller process than the one that actually happened. The peak cannot fall, and
     * it is the only one of the two that a "the map costs 300 MB" claim can rest on.
     *
     * WHY IT IS TICKED FROM `Renderer::BeginFrame` AND NOT FROM A HUD. The shipping host draws no HUD,
     * and §0.4 of the world programme is explicit that the host we cannot measure is the one that
     * matters. A detector that only runs while an editor panel is open measures the editor.
     *
     * AND WHY `BeginFrame` RATHER THAN `EndFrame` — the same reason the draw counters chose it: a frame
     * that loses the device is never recorded, so work hung off the end of a frame either does not run
     * or runs after the state that would explain the loss has been cleared.
     *
     * The baseline is taken on the FIRST sampled frame, i.e. after the process has booted and before it
     * has loaded a world. Every delta is against that, which is what makes two runs comparable.
     */
    class MemoryWatch final
    {
    public:
        /**
         * @brief Fold one reading into the peaks. Called once per frame, by the renderer.
         *
         * THE READING IS PASSED IN RATHER THAN TAKEN HERE, and that is not a style choice. `Take()` has
         * to reach `EngineContext` — hence the whole engine — while everything this class decides (what
         * the baseline is, when a peak rises, what "unknown" means as against "zero") is arithmetic. Put
         * the query inside and the arithmetic becomes unreachable from any suite that has not opened a
         * window, which is all of them. The caller writes `MemoryWatch::SampleFrame(
         * MemoryReadout::Take() )`, which also says at the call site that a fresh reading is taken every
         * frame.
         */
        static void SampleFrame( const MemoryReadout& reading );

        /// Frames sampled so far. Zero means the detector never ran, which a report must distinguish
        /// from "nothing grew".
        [[nodiscard]] static uint64_t FramesSampled();

        /// Peak process-resident bytes seen across sampled frames, and the same for device-local usage.
        /// Both 0 when nothing was sampled or the source was unknown.
        [[nodiscard]] static uint64_t PeakProcessResident();
        [[nodiscard]] static uint64_t PeakDeviceLocalUsage();

        /// The first sampled frame's readings, i.e. the boot baseline every delta is measured from.
        [[nodiscard]] static uint64_t BaselineProcessResident();
        [[nodiscard]] static uint64_t BaselineDeviceLocalUsage();

        /// The sampler's own lines: baseline, peak, and the growth between them. Separate from
        /// `MemoryReadout::Report()` because one is an instant and the other is a history, and a reader
        /// who cannot tell which he is holding will quote the wrong one.
        [[nodiscard]] static std::string Report();

        /// Forget everything sampled, so the next frame becomes the baseline again. For tests, and for
        /// nothing else: a caller resetting this in the editor would silently move the origin of every
        /// growth number afterwards.
        static void ResetForTest();
    };

} // namespace Desert::Graphic
