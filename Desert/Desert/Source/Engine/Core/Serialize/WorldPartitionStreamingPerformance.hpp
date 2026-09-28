#pragma once
// Pattern from UE 5.8
// Engine/Source/Runtime/Engine/Private/WorldPartition/WorldPartitionStreamingPolicy.cpp:868-925
// (UpdateStreamingPerformance: Critical + bBlockOnSlowStreaming -> bRequestedBlockOnAsyncLoading) and
// WorldPartitionRuntimeHash.cpp:246 (GetStreamingPerformanceForCell), adapted: the per-cell distance ratios are
// replaced by the owner's decision O2 below, and the result is a value the hosts read instead of a world flag.
//
// WHEN STREAMING CANNOT KEEP UP (WP12, owner decision O2): "fly on HLOD, block only when there is no cell under
// the player's feet".
//
// Every frame the residency leaves a queue behind: units wanted and read (Loaded) but not yet activated, and
// units still being read (Loading). The queue is weighed against ONE frame's activation budget — its records
// times MsPerRecord against ActivationBudgetMs — and a queue that does not fit is SLOW: the world will fill in
// over the coming frames, and until it does the far cells are drawn by their HLOD (WP11). Nothing stops.
//
// CRITICAL is narrower and is the only state that blocks: a cell whose square holds a streaming source is not
// Activated. There the HLOD is no help — it stands in for a cell seen from outside its range, not for the
// ground under the player — so the game's time stops and a loading overlay is drawn while the loader keeps
// reading (the host's Tick goes on; nothing is read synchronously). The block lifts on the first frame every
// such cell is Activated.
//
// Two cases do NOT block, on purpose:
//   * no planned cell holds the source at all: an empty stretch of the world has nothing to wait for;
//   * the cell under the source FAILED to load: the failure is logged with its reason and retried with backoff
//     (WorldPartitionResidencyRules.hpp, FAILURE); blocking on it would lock a game behind a missing file.
//
// PURE: a function of the plan, the settings, the residency state and the sources.
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace Desert::Core::Rules
{
    enum class StreamingPerformance : std::uint8_t
    {
        Good,     // the queue fits in this frame's activation budget
        Slow,     // it does not: far cells show their HLOD until it drains
        Critical, // a cell under a source is not resident: the game waits behind the loading overlay
    };

    struct StreamingAssessment
    {
        StreamingPerformance Performance    = StreamingPerformance::Good;
        std::size_t          QueuedUnits    = 0; // Loading + Loaded (not yet Activated)
        std::size_t          QueuedRecords  = 0;
        double               QueuedMs       = 0.0;       // QueuedRecords · MsPerRecord
        std::size_t          UnderSource    = kNoRecord; // first cell unit under a source that blocks
        std::size_t          FailedUnderSrc = 0;         // cells under a source that failed (do not block)

        [[nodiscard]] bool Blocks() const
        {
            return Performance == StreamingPerformance::Critical;
        }
    };

    [[nodiscard]] inline bool CellHolds( const PlannedCell& cell, const StreamingSource& source )
    {
        const float x = source.Position.x;
        const float z = source.Position.z;
        return x >= cell.Square.MinX && x < cell.Square.MaxX && z >= cell.Square.MinZ && z < cell.Square.MaxZ;
    }

    // @p state sized for @p plan (an unsized state — before the first step — has nothing resident).
    [[nodiscard]] inline StreamingAssessment AssessStreaming( const WorldPartitionPlan&        plan,
                                                              const ResidencySettings&         settings,
                                                              const ResidencyState&            state,
                                                              std::span<const StreamingSource> sources )
    {
        StreamingAssessment out;
        const std::size_t   units   = ResidencyUnitCount( plan );
        const auto          stateOf = [&]( std::size_t unit )
        { return unit < state.Units.size() ? state.Units[unit].State : Residency::Unloaded; };

        for ( std::size_t unit = 0; unit < units; ++unit )
        {
            const Residency now = stateOf( unit );
            if ( now == Residency::Loading || now == Residency::Loaded )
            {
                ++out.QueuedUnits;
                out.QueuedRecords += ResidencyUnitRecords( plan, unit );
            }
        }
        out.QueuedMs = static_cast<double>( out.QueuedRecords ) * settings.MsPerRecord;
        if ( out.QueuedMs > settings.ActivationBudgetMs )
            out.Performance = StreamingPerformance::Slow;

        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            const std::size_t unit = plan.AlwaysLoaded.size() + cell;
            bool              held = false;
            for ( const StreamingSource& source : sources )
                held = held || CellHolds( plan.Cells[cell], source );
            if ( !held )
                continue;
            const Residency now = stateOf( unit );
            if ( now == Residency::Failed )
                ++out.FailedUnderSrc;
            else if ( now != Residency::Activated && out.UnderSource == kNoRecord )
                out.UnderSource = unit;
        }
        if ( out.UnderSource != kNoRecord )
            out.Performance = StreamingPerformance::Critical;
        return out;
    }
} // namespace Desert::Core::Rules

namespace Desert::Core
{
    // WHAT A HOST READS OFF A STREAMED SCENE: set on the scene's registry context by WorldStreamer::Tick every
    // frame (and removed with the streamer), so the renderer's UI pass and the runtime's overlay ask the scene
    // they draw rather than holding a pointer to the streamer.
    struct WorldStreamingWait
    {
        Rules::StreamingAssessment Assessment;
        std::uint32_t              FramesWaiting = 0; // consecutive blocked ticks; drives the overlay's motion
    };
} // namespace Desert::Core
