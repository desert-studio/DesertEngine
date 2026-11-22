#pragma once

#include <Engine/Graphic/RDG/RDGCompileResult.hpp>

#include <array>
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
// THE MODEL IS THE MEASURED TIMELINE (FPS-ASYNC-BALANCE-MODEL). Every GPU row of the window is placed: a pass
// costs its INCLUSIVE time (a pass's nested scopes - "DLSS-RR: NGX Evaluate", the mesh draw groups - are its
// work), a pass whose name carries a per-frame count ("(256 pages)") is costed by its family (the variants the
// window averaged, summed), a row shared by views or graphs is split over every run of every graph drawn in the
// window, the async queue's own time (its remainder: barriers and gaps inside the "RDG Async: a .. b" segments)
// is shared over the segments by span and rides on each head pass while it is on AsyncCompute, and the
// graphics time no pass of a drawn graph holds (other
// GPU work, the unaccounted remainder) is a constant beside the graphs. The modelled frame is that constant plus
// each drawn graph's list schedule times its runs per frame; the "RDG Wait" rows are what the schedule must
// reproduce, never a cost. (It was: one graph's pass SELF times, by exact name - on FPS-FIELD-BARN-TURN
// scn-1121-214723 the SceneView graph was modelled at 2.445 ms of a 9.38 ms GPU frame, missed the 3.1 ms DLSS-RR
// evaluate nested in its pass, and gave 'Clouds: March' to the graphics queue, the longer one.)
// A FLIP TO GRAPHICS FOLLOWS A MODELLED WAIT (FPS-ASYNC-BALANCE-ACT, UE: none - r.*.AsyncCompute is fixed; here
// the profile-guided queue layout of id Tech / Frostbite): only while the schedule has the graphics queue
// waiting more than kWaitThresholdMs on the async queue (at a join, or at the graph end), and only an async pass
// at or before the async pass that join waits for (the async queue runs in position order, so each of them
// delays it), cheapest on Graphics first, the first that shortens the modelled graph by kFlipMarginMs. A pass
// back to AsyncCompute is the one whose flip shortens the graph most. (It was: no flip to Graphics while the
// graph's graphics queue held more work than its async queue - a whole-graph sum. On FPS-FIELD-BARN-TURN
// scn-1122-113538 the SceneView graph's graphics queue (DLSS-RR, the G-buffer, the shadows) always held more, so
// no pass ever moved while graphics waited 1.87 ms before 'Deferred: Composite (Simple Tiles)' on the
// ScreenProbes segment forked 7 graphics passes earlier: "RDG: Async balance" ran every window with 0 moves.)
// A graph drawn while graphics waits on async and nothing moved logs why (warm-up, no modelled wait, no
// gaining pass) once per change of the reason, with the modelled wait beside the measured one.
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
        // The modelled graphics wait on the async queue (ms per run of the graph) a flip to Graphics answers.
        static constexpr double kWaitThresholdMs = 0.1;
        // FPS-ASYNC-WARMUP: no flip from a shape's first kWarmupWindows measured windows, nor from a window whose
        // modelled graph is over kWarmupRatio x the shortest one seen - shader compiles and first-use uploads
        // inflate costs there. (FPS-FIELD-BARN-TURN scn-1121-111104: a 58.9 ms warm-up window gave
        // 'ScreenProbes: Integrate Classify' to Graphics for 0.315 ms; beside the async traces it then measured
        // 0.757 ms there, and the model never saw a gain taking it back.) The shortest is the shortest of the
        // last kWarmupWindows windows, not of the run: one short window (a load screen, a frame with half the
        // passes unmeasured) would otherwise mark every later window a warm-up for the rest of the run.
        static constexpr uint32_t kWarmupWindows = 4;
        static constexpr double   kWarmupRatio   = 1.5;

        // One GPU row of the published window (the profiler's averaged columns, ms per frame), its view prefix
        // ("<view> | ") already taken off. The whole-frame bracket is not a row: it is Window::GpuFrameMs.
        struct MeasuredRow
        {
            std::string_view Name;
            double           GpuMs          = 0.0; // inclusive of nested scopes
            double           GpuSelfMs      = 0.0; // both queues
            double           GpuAsyncSelfMs = 0.0; // the part on the async compute queue
        };
        // One published window: every GPU row and the measured GPU frame. No row: GPU timing off.
        struct Window
        {
            std::span<const MeasuredRow> Rows;
            double                       GpuFrameMs = 0.0;
        };
        using LogFn = std::function<void( std::string_view line )>;

        // The plan graph @p graph begins with: its shape is kept when it differs from the one kept (passes,
        // pipes, edges), so a cached plan is walked once. Every call counts one run of the graph in the window.
        void NotePlan( std::string_view graph, const CompileResult& plan );
        // One frame of the window (the backend's BeginFrame): runs per frame = runs / frames.
        void NoteFrame() noexcept
        {
            ++m_Frames;
        }
        // One published profiler window: per graph drawn in it, the flips the model gains from. True when the
        // list changed.
        bool Rebalance( const Window& window, const LogFn& log );
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
            // Warm-up guard (FPS-ASYNC-WARMUP): measured windows seen and the modelled graph of the last
            // kWarmupWindows of them (a ring, by Windows).
            uint32_t Windows                = 0;
            std::array<double, kWarmupWindows> Recent{};
            uint32_t Runs = 0; // NotePlan calls since the last Rebalance: drawn in the window when > 0
            // Why the last window with a measured graphics wait moved nothing (HoldReason), logged on a change.
            uint8_t LastHold = 0;
        };
        // The schedule's largest graphics wait on the async queue: at a graphics pass that starts after an async
        // pass it depends on, or at the graph end. Async: the async position it waits for (count: no wait).
        struct ModelledWait
        {
            double Largest = 0.0;
            double Total   = 0.0;
            size_t Join    = 0; // the graphics position that waits (count: the graph end)
            size_t Async   = 0;
        };
        struct Decision
        {
            uint64_t Key        = 0;
            uint32_t FrozenLeft = 0;
            uint32_t Holds      = 0;     // flips to Graphics while the decision lived: the next one holds longer
            bool     OnGraphics = false; // false: given back to AsyncCompute, kept while frozen
        };

        // @p asyncExtra: a position's added cost while it runs on AsyncCompute (its segment's own time).
        [[nodiscard]] static double Simulate( const Shape& shape, std::span<const Pipe> pipes,
                                              std::span<const double> cost, std::span<const double> asyncExtra,
                                              std::vector<double>& finish );
        [[nodiscard]] static ModelledWait WaitOf( const Shape& shape, std::span<const Pipe> pipes,
                                                  std::span<const double> cost,
                                                  std::span<const double> asyncExtra );
        [[nodiscard]] Decision*     Find( uint64_t key ) noexcept;
        void                        SortList();

        std::vector<Shape>    m_Shapes;
        std::vector<Decision> m_Decisions; // every pass on Graphics by the balance, and the passes frozen
        std::vector<uint64_t> m_GraphicsPasses;
        uint32_t              m_Frames = 0; // NoteFrame calls since the last Rebalance
    };
} // namespace Desert::Graphic::RDG
