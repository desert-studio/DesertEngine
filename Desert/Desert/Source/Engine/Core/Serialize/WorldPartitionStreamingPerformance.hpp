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
// SLOW is UE's warning band (GetStreamingPerformanceForCell, wp.Runtime.BlockOnSlowStreamingRatio x
// ...WarningFactor): a cell the sources want that is not activated yet lies within SlowStreamingRatio x
// SlowStreamingWarningFactor of a source's loading range - the cell's distance (the 3D distance THE QUERY uses)
// over that range. A ratio and not one frame's activation budget (WP-STREAM-SETTINGS): a long queue far away is
// the ordinary state of a moving camera, a missing cell close to the player is what is slow. Until the cell
// arrives its HLOD draws it (WP11). Nothing stops. The queue it reports - units wanted and read (Loaded) or part
// made (Activating) but not yet activated, and units still being read (Loading) - is priced at what a record has
// cost to make so far this session (the executor's ActivationCostModel, WorldPartitionActivationCost.hpp; 0
// before anything was made).
//
// CRITICAL is narrower and is the only state that blocks (while Engine.json's WorldPartition.BlockOnSlowStreaming
// is on; off, the WorldStreamer logs it once as "slow streaming (blocking disabled)" and play goes on): a cell
// whose square holds a streaming source is not Activated — and Activated means every record made AND every body
// in the physics world (WP-ACTIVATE-SLICED, WorldPartitionResidencyExecutor.hpp THE SLICES), so the block lifts
// on a frame the ground under the player collides, never on one where the player would fall through it. There
// the HLOD is no help — it stands in for a cell seen from outside its range, not for the ground under the player
// — so the game's time stops and a loading overlay is drawn while the loader keeps reading (the host's Tick goes
// on; nothing is read synchronously). The block lifts on the first frame every such cell is Activated.
//
// Two cases do NOT block, on purpose:
//   * no planned cell holds the source at all: an empty stretch of the world has nothing to wait for;
//   * the cell under the source FAILED to load: the failure is logged with its reason and retried with backoff
//     (WorldPartitionResidencyRules.hpp, FAILURE); blocking on it would lock a game behind a missing file.
//
// PURE: a function of the plan, the settings, the residency state and the sources.
#include <Engine/Core/Serialize/WorldPartitionActivationCost.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>

namespace Desert::Core::Rules
{
    enum class StreamingPerformance : std::uint8_t
    {
        Good,     // every cell near a source is activated
        Slow,     // a cell near a source is not activated yet: its HLOD draws it until it is
        Critical, // a cell under a source is not resident: the game waits behind the loading overlay
    };

    struct StreamingAssessment
    {
        StreamingPerformance Performance    = StreamingPerformance::Good;
        std::size_t          QueuedUnits    = 0;   // Loading + Loaded + Activating (not yet Activated)
        std::size_t          QueuedRecords  = 0;   // still to make
        double               QueuedMs       = 0.0; // QueuedRecords · the measured ms per record (0 unmeasured)
        std::size_t          UnderSource    = kNoRecord; // first cell unit under a source that blocks
        std::size_t          FailedUnderSrc = 0;         // cells under a source that failed (do not block)

        // ResidencySettings::BlockOnSlowStreaming the assessment was made with: off, a Critical frame is reported
        // (and logged) but play goes on.
        bool BlockOnSlowStreaming = true;

        [[nodiscard]] bool Blocks() const
        {
            return Performance == StreamingPerformance::Critical && BlockOnSlowStreaming;
        }
    };

    // A source holds a cell only on a grid it targets (TargetsGrid): a source streaming the Near grid alone does
    // not wait for the Main cell it stands in.
    [[nodiscard]] inline bool CellHolds( const PlannedCell& cell, const StreamingSource& source,
                                         const WorldPartitionSerialized& partition )
    {
        if ( cell.Grid >= partition.Grids.size() || !TargetsGrid( source, partition.Grids[cell.Grid] ) )
            return false;
        const float x = source.Position.x;
        const float z = source.Position.z;
        if ( !( x >= cell.Square.MinX && x < cell.Square.MaxX && z >= cell.Square.MinZ && z < cell.Square.MaxZ ) )
            return false;
        // WP-SRC-ALTITUDE: under the source's feet only while the source also REACHES the cell. A plane 2 km above
        // a 25 m cell stands in its square, but THE QUERY does not want that cell (its distance is 3D): waiting
        // for it would block forever, and from up there the cell's HLOD stands in for it.
        for ( const SourceSector& sector : SourceSectors( source, partition.Grids[cell.Grid] ) )
            if ( WithAltitude( DistanceToCellInSector( sector, cell.Bounds ), sector, cell.Height ) <= sector.Radius )
                return true;
        return false;
    }

    // The smallest distance from a source streaming @p cell's grid to the cell, over that source's loading range
    // on the grid (UE's DistanceToCell / LoadingRange), in the 3D distance THE QUERY uses; +infinity when no
    // source streams the grid.
    [[nodiscard]] inline double CellRangeRatio( const PlannedCell& cell, std::span<const StreamingSource> sources,
                                                const WorldPartitionSerialized& partition )
    {
        double best = std::numeric_limits<double>::infinity();
        if ( cell.Grid >= partition.Grids.size() )
            return best;
        const auto& grid = partition.Grids[cell.Grid];
        for ( const StreamingSource& source : sources )
        {
            if ( !source.TargetGrids.empty() && !TargetsGrid( source, grid ) )
                continue;
            for ( const SourceSector& sector : SourceSectors( source, grid ) )
            {
                if ( !( sector.Radius > 0.0f ) )
                    continue;
                const double distance = static_cast<double>(
                     WithAltitude( DistanceToCellInSector( sector, cell.Bounds ), sector, cell.Height ) );
                best = std::min( best, distance / static_cast<double>( sector.Radius ) );
            }
        }
        return best;
    }

    // @p state sized for @p plan (an unsized state — before the first step — has nothing resident).
    [[nodiscard]] inline StreamingAssessment
    AssessStreaming( const WorldPartitionPlan& plan, const WorldPartitionSerialized& partition,
                     const ResidencySettings& settings, const ActivationCostModel& cost, const ResidencyState& state,
                     std::span<const StreamingSource>       sources,
                     std::span<const DataLayerRuntimeState> unitLayerStates = {} )
    {
        StreamingAssessment out;
        // WP-DATA-LAYERS-RUNTIME: how far its data layers want a unit (empty = all the way).
        const auto layerState = [&]( std::size_t unit )
        { return unit < unitLayerStates.size() ? unitLayerStates[unit] : DataLayerRuntimeState::Activated; };
        const std::size_t   units   = ResidencyUnitCount( plan );
        const auto          stateOf = [&]( std::size_t unit )
        { return unit < state.Units.size() ? state.Units[unit].State : Residency::Unloaded; };

        for ( std::size_t unit = 0; unit < units; ++unit )
        {
            const Residency now = stateOf( unit );
            const bool parked = now == Residency::Loaded && layerState( unit ) != DataLayerRuntimeState::Activated;
            if ( ( now == Residency::Loading || now == Residency::Loaded || now == Residency::Activating ) && !parked )
            {
                ++out.QueuedUnits;
                const std::size_t made = now == Residency::Activating ? state.Units[unit].Made : 0;
                out.QueuedRecords += ResidencyUnitRecords( plan, unit ) - made;
            }
        }
        const std::optional<double> msPerRecord = cost.MeanMsPerRecord();
        out.QueuedMs             = static_cast<double>( out.QueuedRecords ) * msPerRecord.value_or( 0.0 );
        out.BlockOnSlowStreaming = settings.BlockOnSlowStreaming;

        const double slowRatio = static_cast<double>( settings.SlowStreamingRatio ) *
                                 static_cast<double>( settings.SlowStreamingWarningFactor );
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            const std::size_t unit  = plan.AlwaysLoaded.size() + cell;
            const Residency   residency = stateOf( unit );
            if ( out.Performance == StreamingPerformance::Good && residency != Residency::Activated &&
                 residency != Residency::Failed && CellRangeRatio( plan.Cells[cell], sources, partition ) < slowRatio )
                out.Performance = StreamingPerformance::Slow;
            bool              held = false;
            for ( const StreamingSource& source : sources )
                held = held || CellHolds( plan.Cells[cell], source, partition );
            if ( !held || layerState( unit ) == DataLayerRuntimeState::Unloaded )
                continue;
            const Residency now = stateOf( unit );
            if ( layerState( unit ) == DataLayerRuntimeState::Loaded && now == Residency::Loaded )
                continue; // as far as its layers want it
            if ( now == Residency::Failed )
                ++out.FailedUnderSrc;
            else if ( now != Residency::Activated && out.UnderSource == kNoRecord )
                out.UnderSource = unit;
        }
        if ( out.UnderSource != kNoRecord )
            out.Performance = StreamingPerformance::Critical;
        return out;
    }

    // HAS ONE SOURCE'S WORLD ARRIVED (UE's UWorldPartitionSubsystem::IsStreamingCompleted for one streaming
    // source provider): the cells THE QUERY wants for that source alone — its own shapes, without the unload
    // band the residency widens them by — counted by their state. A failed cell does not hold completion back,
    // for the reason it does not block play (above): it is logged and retried, and waiting on it would lock a
    // game behind a missing file. The always-loaded part is resident before Play starts and is not counted.
    struct SourceCompletion
    {
        std::size_t Wanted    = 0;
        std::size_t Activated = 0;
        std::size_t Failed    = 0;

        [[nodiscard]] bool Complete() const
        {
            return Activated + Failed == Wanted;
        }
    };

    // @p state sized for @p plan (an unsized state has nothing resident). Refused as THE QUERY refuses.
    [[nodiscard]] inline Common::ResultStr<SourceCompletion>
    AssessSourceCompletion( const WorldPartitionPlan& plan, const WorldPartitionSerialized& partition,
                            const ResidencyState& state, const StreamingSource& source,
                            std::span<const DataLayerRuntimeState> unitLayerStates = {} )
    {
        auto wish = QueryStreamingCells( plan, partition, std::span<const StreamingSource>( &source, 1 ) );
        if ( !wish )
            return Common::MakeError<SourceCompletion>( wish.GetError() );
        SourceCompletion out;
        for ( const WantedCell& wanted : wish.GetValue().Cells )
        {
            const std::size_t unit = plan.AlwaysLoaded.size() + wanted.Cell;
            const Residency   now  = unit < state.Units.size() ? state.Units[unit].State : Residency::Unloaded;
            const DataLayerRuntimeState layers =
                 unit < unitLayerStates.size() ? unitLayerStates[unit] : DataLayerRuntimeState::Activated;
            if ( layers != DataLayerRuntimeState::Activated )
                continue; // its data layers do not want it as an entity: nothing a source waits for
            ++out.Wanted;
            if ( now == Residency::Activated )
                ++out.Activated;
            else if ( now == Residency::Failed )
                ++out.Failed;
        }
        return Common::MakeSuccess( out );
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
        // Each enabled StreamingSourceComponent's entity (entt id) and how much of what it wants is resident,
        // as of this tick: what a script asks with entity:streamingCellsPending().
        std::unordered_map<std::uint32_t, Rules::SourceCompletion> Sources;
    };

    // WHAT CHANGED IN THE LAST STREAMING TICK, FOR GAMEPLAY (UE UWorldPartitionSubsystem::OnStreamingStateUpdated
    // and a cell's activation / unload): set on the scene's registry context by WorldStreamer::Tick every frame,
    // replacing the last tick's, and delivered once to the scripts by ScriptEngine::DeliverWorldPartitionEvents
    // (Publication tells a delivered tick from a new one). Cells are named as the streaming log names them
    // (Rules::DescribeResidencyUnit).
    struct WorldPartitionStreamingEvents
    {
        std::uint64_t            Publication = 0;
        bool                     Updated     = false; // some cell changed its residency this tick
        std::vector<std::string> Activated;           // cells whose entities entered the world this tick
        std::vector<std::string> Unloaded;            // cells that were Loaded or Activated and left this tick
        bool                     Delivered = false;   // the scripts heard this publication
    };
} // namespace Desert::Core
