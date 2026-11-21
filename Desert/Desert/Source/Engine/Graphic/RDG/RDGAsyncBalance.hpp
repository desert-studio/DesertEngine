#pragma once

#include <Engine/Graphic/RDG/RDGCompileResult.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// FPS-ASYNC-BALANCE. The queue of an AsyncCompute pass chosen from what the passes measured (UE: the async
// compute choice is a fixed per-feature switch, r.Lumen.*.AsyncCompute; here it is decided per pass from the
// GPU times of the previous frames, so a run that outlasts the graphics work of its window gives passes back to
// Graphics, and a pass that only lengthens the graphics queue goes back to AsyncCompute).
//
// Responsibility: hold the shape of each graph's last plan (executed passes in position order, their pipe,
// whether they declared AsyncCompute, their dependency edges as positions), and from one cost per pass (the
// published profiler window: an average over many frames) choose the set of AsyncCompute passes that run on
// Graphics. The choice minimises a two-queue list schedule of the plan (each queue in position order, a pass
// starts after its queue's previous pass and after every pass it depends on), one flip at a time, accepting a
// flip only when it shortens the modelled graph by at least kFlipMarginMs.
// Hysteresis: a flip is final for kFrozenWindows published windows; a pass given to Graphics again while its
// decision lives (it went back to AsyncCompute and the model gave it to Graphics once that flip thawed) holds
// twice as long each time, up to kFrozenWindows << kMaxHoldDoublings, so a pass that ran slower beside the
// graphics queue does not flip back and forth because it runs faster alone. Taking a pass back is modelled with
// its CURRENT cost (FPS-ASYNC-JOIN-2): no cost measured in an earlier window is held, because a window can be
// one the workload no longer has. (It was: the AsyncCompute cost measured at the flip, kept forever. On
// FPS-FIELD-BARN-TURN scn-1117-213616 a warm-up window - shader compiles, a 13.5 ms modelled graph - measured
// 'RadianceCache: Filter' at 1.043 ms and gave it to Graphics; it measured 0.18 ms after, never went back, and
// the graphics queue waited 0.19 ms for the cache's Mark .. Trace before it, every frame.)
// Who calls it: the backend - NotePlan when a graph begins with a plan it has not seen, Rebalance once per
// published profiler window, GraphicsPasses when a graph compiles (IBackend::GetAsyncBalance). The compile
// (Compiler::ChoosePipes) runs a listed pass on Graphics; the compile cache keys the list (AsyncBalanceKeyOf).
// Never: change what a pass reads or writes, or the order of any dependency edge - a flipped pass keeps every
// edge, so the graph's results are the same; decide from no measurement (no cost: the choice stays); move a pass
// declared PassFlags::AsyncComputeFixed (its frame chose the queue of its whole chain once - the cloud trace).
namespace Desert::Graphic::RDG
{
    // A pass's identity for the balance: its graph's name and its own name (FNV-1a, 64 bit).
    [[nodiscard]] uint64_t AsyncBalancePassKey( std::string_view graph, std::string_view pass ) noexcept;
    // The compile-cache word of a GraphicsPasses list (0 for an empty one).
    [[nodiscard]] uint64_t AsyncBalanceKeyOf( std::span<const uint64_t> graphicsPasses ) noexcept;

    class AsyncBalance
    {
    public:
        // The modelled gain a flip must reach (ms of the modelled graph), and the published windows a flip holds.
        static constexpr double   kFlipMarginMs   = 0.05;
        static constexpr uint32_t kFrozenWindows    = 4;
        static constexpr uint32_t kMaxHoldDoublings = 4; // a repeated flip to Graphics holds at most 64 windows
        static constexpr uint32_t kFlipsPerWindow   = 4;
        // FPS-ASYNC-WARMUP: no flip from a shape's first kWarmupWindows measured windows, nor from a window whose
        // modelled graph is over kWarmupRatio x the shortest one seen - shader compiles and first-use uploads
        // inflate costs there. (FPS-FIELD-BARN-TURN scn-1121-111104: a 58.9 ms warm-up window gave
        // 'ScreenProbes: Integrate Classify' to Graphics for 0.315 ms; beside the async traces it then measured
        // 0.757 ms there, and the model never saw a gain taking it back.)
        static constexpr uint32_t kWarmupWindows = 4;
        static constexpr double   kWarmupRatio   = 1.5;

        // One pass's cost for Rebalance: GPU self time per frame (both queues), Measured false when the window
        // has no row for it.
        struct Cost
        {
            double Ms       = 0.0;
            bool   Measured = false;
        };
        using CostOf = std::function<Cost( std::string_view pass )>;
        using LogFn  = std::function<void( std::string_view line )>;

        // The plan graph @p graph begins with: its shape is kept when it differs from the one kept (passes,
        // pipes, edges), so a cached plan is walked once.
        void NotePlan( std::string_view graph, const CompileResult& plan );
        // One published profiler window: per graph, the flips the model gains from. True when the list changed.
        bool Rebalance( const CostOf& costOf, const LogFn& log );
        // Sorted AsyncBalancePassKey of every AsyncCompute pass that runs on Graphics.
        [[nodiscard]] std::span<const uint64_t> GraphicsPasses() const noexcept
        {
            return m_GraphicsPasses;
        }

    private:
        struct Shape
        {
            std::string              Graph;
            const CompileResult*     Plan = nullptr; // identity only, never read after NotePlan
            std::vector<std::string> Names;          // by position
            std::vector<uint64_t>    Keys;           // by position
            std::vector<Pipe>        Pipes;          // by position
            std::vector<bool>        Eligible;       // declared AsyncCompute on a device with an async queue
            std::vector<uint32_t>    PredFirst;      // by position, into Preds; one more entry at the end
            std::vector<uint32_t>    Preds;          // positions this position depends on
            // Warm-up guard (FPS-ASYNC-WARMUP): measured windows seen and the shortest modelled graph so far.
            uint32_t Windows   = 0;
            double   MinLength = 0.0;
        };
        struct Decision
        {
            uint64_t Key        = 0;
            uint32_t FrozenLeft = 0;
            uint32_t Holds      = 0;     // flips to Graphics while the decision lived: the next one holds longer
            bool     OnGraphics = false; // false: given back to AsyncCompute, kept while frozen
        };

        [[nodiscard]] static double Simulate( const Shape& shape, std::span<const Pipe> pipes,
                                              std::span<const double> cost, std::vector<double>& finish );
        [[nodiscard]] Decision*     Find( uint64_t key ) noexcept;
        void                        SortList();

        std::vector<Shape>    m_Shapes;
        std::vector<Decision> m_Decisions; // every pass on Graphics by the balance, and the passes frozen
        std::vector<uint64_t> m_GraphicsPasses;
    };
} // namespace Desert::Graphic::RDG
