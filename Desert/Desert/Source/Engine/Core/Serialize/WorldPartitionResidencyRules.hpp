#pragma once

// HOW A PARTITIONED WORLD GETS FROM WHAT IS RESIDENT TO WHAT THE STREAMING QUERY WANTS, ONE FRAME AT A TIME.
//
// A pure function over the partition plan (WorldPartitionRules.hpp), the streaming query
// (WorldPartitionStreamingRules.hpp), the residency state of every unit, the load outcomes that arrived since
// the last frame and the time: no ECS, no GPU, no I/O. It answers what to DO this frame — start loads, cancel
// them, activate, deactivate, unload — and the state that results. Loading itself is asynchronous and outside:
// the caller performs a StartLoad and reports back, a frame or many later, that it completed or failed.
// This is the pattern of UE's UWorldPartitionStreamingPolicy::UpdateStreamingState (a target state computed
// from the sources, then applied under a per-frame limit) and not its code.
//
// ── UNITS ─────────────────────────────────────────────────────────────────────────────────────────
//
// What becomes resident is a UNIT: each always-loaded composite of the plan, then each cell. Unit u below
// `plan.AlwaysLoaded.size()` is the composite `plan.AlwaysLoaded[u]`; the rest are `plan.Cells[u - that]`.
// One index space, so one state vector and one action type serve both.
//
// ── STATES ────────────────────────────────────────────────────────────────────────────────────────
//
//   Unloaded ──StartLoad──▶ Loading ──completed──▶ Loaded ──Activate (budget)──▶ Activated
//                             │ failed                 ▲
//                             ▼                        └── an in-range cell waits here for the budget
//                           Failed ──retry time reached, still wanted──▶ Loading
//
// Leaving range: Activated ─Deactivate+Unload─▶ Unloaded; Loaded ─Unload─▶ Unloaded; Loading ─CancelLoad─▶
// Unloaded; Failed ──▶ Unloaded (the failure is forgotten: a cell that comes back is tried at once).
//
// A load is identified by a TICKET. An outcome whose ticket is not the unit's current load — it was cancelled,
// or superseded by a later load — is stale: it changes nothing and is counted in `StaleOutcomes`, because an
// outcome for a cell the world has since left behind is the ordinary case of a fast camera, not an error.
//
// ── HYSTERESIS: A DISTANCE, NOT A TIMER ───────────────────────────────────────────────────────────
//
// A unit is WANTED when the query wants it. A resident unit is KEPT while the same query with every source's
// range (and look-ahead sweep) grown by `UnloadMargin` still wants it, and unloaded the frame it is not. The band
// between the two radii is where nothing changes: no load starts there and nothing resident leaves.
//
// Why distance and not "N seconds out of range": that is how UE does it. Its policy unloads a cell the frame
// the cell is outside every source's shape (WorldPartitionStreamingPolicy.cpp, the ToUnloadCells block) and
// has no timer; what keeps an edge from flickering is a source shape widened by an extra radius
// (WorldPartitionSubsystem.cpp, `ExtraRadius`, derived from the location quantization). And on its own merits:
// a distance band cannot flicker at any speed or frame rate — to unload a cell a source must travel the whole
// band away from it — while a timer both flickers for a source that paces back and forth slower than N, and
// keeps a teleport's entire old neighbourhood resident for N seconds next to the new one. The result depends on
// where the sources are and not on how long the caller's frames took.
//
// ── THE BUDGET ────────────────────────────────────────────────────────────────────────────────────
//
// Activation runs on the main thread, so it is what a frame pays for. Each Loaded unit that is wanted is
// activated in priority order — always-loaded composites first, then cells in the query's nearest-first order —
// while the frame's estimated activation time stays within `ActivationBudgetMs`. The estimate is the unit's
// record count times `MsPerRecord`, a coefficient measured in WP5b (see its field). Activation stops at the
// first unit that does not fit rather than skipping to a cheaper one behind it: priority is the order the world
// fills in around the camera, and a cheap far cell must not overtake a nearer one. The first activation of
// a frame always goes ahead even when it alone exceeds the budget; otherwise a unit larger than the budget
// would never activate at all. `MaxConcurrentLoads` bounds the loads in flight, in the same priority order —
// UE's MaxCellsToLoad.
//
// Deactivation and unloading are not budgeted: whatever leaves range leaves in the frame it does. UE does the
// same ("Deactivating is not concerned by MaxCellsToLoad", WorldPartitionStreamingPolicy.cpp), and the memory
// that leaving units hold is what the arriving ones need.
//
// ── FAILURE ───────────────────────────────────────────────────────────────────────────────────────
//
// A failed load is a state with its reason and time, never a silent Unloaded: `Failed` keeps the loader's
// message, and the unit is retried only when still wanted and `RetryDelaySeconds · RetryBackoff^(failures-1)`
// (at most `MaxRetryDelaySeconds`) has passed since the failure. Not every frame: a missing file would
// otherwise be re-read sixty times a second.

#include <format>
#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingRules.hpp>
#include <Projects/WorldPartitionSettings.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Core::Rules
{
    enum class Residency : std::uint8_t
    {
        Unloaded,
        Loading,
        Loaded,
        Activated,
        Failed,
    };

    struct UnitResidency
    {
        Residency     State    = Residency::Unloaded;
        std::uint64_t Ticket   = 0; // the current (or last) load's ticket; 0 before the first load
        std::uint32_t Failures = 0; // consecutive failed loads; reset by a success or by leaving range
        double        FailedAt = 0.0;
        std::string   FailureReason; // the loader's message; set in Failed only
    };

    struct ResidencyState
    {
        // One entry per unit (see UNITS). Empty before the first step, which sizes it to the plan.
        std::vector<UnitResidency> Units;
        std::uint64_t              NextTicket = 1;
    };

    // The project's streaming settings (Projects/WorldPartitionSettings.hpp: the WorldPartition section of
    // Config/Engine.json with the platform layer over it, read by the WorldStreamer as Play or the game begins)
    // and what the engine MEASURED rather than a project chooses.
    struct ResidencySettings : Project::WorldPartitionSettings
    {
        // Activation cost of one record, MEASURED (WP5b, 2026-09-24): WorldStreamer times every
        // SceneSerializer::InstantiateRecords it makes and logs the total at the end of Play. Release editor,
        // World_Grid8km with a 128 m grid (StaticMesh cubes, ~9 records a unit), headless --play flights across
        // the map on an M-series Mac: 0.0152 ms/record over 3350 records, 0.0198 over 3159. Rounded UP to the
        // worse run, so a frame is not promised more than it gets; a world of heavier records (prefab
        // instances, skinned meshes) is a new measurement, and the streamer's log line is where it comes from.
        double MsPerRecord = 0.02;
    };

    // What the loader reports about one StartLoad.
    struct LoadOutcome
    {
        std::size_t   Unit   = kNoRecord;
        std::uint64_t Ticket = 0;
        bool          Ok     = true;
        std::string   Reason; // required when !Ok
    };

    enum class ResidencyActionKind : std::uint8_t
    {
        StartLoad,
        CancelLoad,
        Activate,
        Deactivate,
        Unload,
    };

    struct ResidencyAction
    {
        ResidencyActionKind Kind   = ResidencyActionKind::StartLoad;
        std::size_t         Unit   = kNoRecord;
        std::uint64_t       Ticket = 0; // StartLoad and CancelLoad: the load's ticket
    };

    struct ResidencyStep
    {
        ResidencyState State;
        // In the order to perform them: every leaving unit (unit order), then loads started, then activations
        // (both in priority order).
        std::vector<ResidencyAction> Actions;
        double                       ActivationMs     = 0.0; // the estimate spent by this frame's activations
        std::size_t                  WaitingForBudget = 0;   // wanted Loaded units left for a later frame
        std::size_t                  WaitingForGate   = 0;   // wanted Loaded units a ResidencyGate held back
        std::size_t                  StaleOutcomes    = 0;
        std::size_t                  HeldByDataLayers = 0; // wanted Loaded units their data layers keep Loaded
    };

    // WHAT ANOTHER RESIDENCY ASKS OF THIS STEP (WP-FAR-8: a world's cells and the HLOD cells that stand in for
    // them, WorldPartitionHLODSwitch.hpp). Per unit, empty = nothing held:
    //  HoldActivation 1 = a Loaded unit stays Loaded this step even when wanted (what stands in for it is not
    //                     yet in a state to give way to it); counted in WaitingForGate.
    //  HoldRelease    1 = an Activated unit stays Activated this step even outside its band (something beneath
    //                     it is still drawn and would be drawn twice once it goes).
    // A gate only delays; it never starts a load, activates or releases anything by itself.
    struct ResidencyGate
    {
        std::vector<std::uint8_t> HoldActivation;
        std::vector<std::uint8_t> HoldRelease;
    };

    [[nodiscard]] inline std::size_t ResidencyUnitCount( const WorldPartitionPlan& plan )
    {
        return plan.AlwaysLoaded.size() + plan.Cells.size();
    }

    // ── RUNTIME DATA LAYERS (WP-DATA-LAYERS-RUNTIME; UE UDataLayerManager + GetCellEffectiveWantedState) ─────
    //
    // Every runtime layer of the world has a state (DataLayerRuntimeState), starting at its InitialRuntimeState
    // and moved by the game (WorldStreamer::SetDataLayerRuntimeState). A unit of runtime layers - a cell of that
    // set (PlannedCell::DataLayers) or an always-loaded composite in such layers - has an EFFECTIVE state, its
    // layers' states combined by the world's DataLayersLogicOperator (Or: the furthest, And: the least); a unit of
    // no runtime layer is Activated by its layers (only distance moves it). StepResidency then:
    //   Unloaded  - the unit is neither wanted nor kept: not read, and released when it was resident;
    //   Loaded    - wanted and kept like any unit in range, read, and NOT activated; an Activated one is
    //               deactivated and stays Loaded (its records in memory, no entity);
    //   Activated - as before: distance decides.
    // An editor layer's entry is Activated: it gates nothing (no unit counts it among its runtime layers).

    // A state's name in a log line or a palette command (UE's enum names).
    [[nodiscard]] constexpr std::string_view DataLayerRuntimeStateName( DataLayerRuntimeState state )
    {
        switch ( state )
        {
            case DataLayerRuntimeState::Unloaded:
                return "Unloaded";
            case DataLayerRuntimeState::Loaded:
                return "Loaded";
            case DataLayerRuntimeState::Activated:
                return "Activated";
        }
        return "Unknown";
    }

    // The runtime layers of @p unit: a cell's third coordinate, or an always-loaded composite's runtime layers.
    [[nodiscard]] inline std::vector<std::size_t> UnitRuntimeDataLayers( const WorldPartitionPlan&       plan,
                                                                         const WorldPartitionSerialized& partition,
                                                                         std::size_t                     unit )
    {
        if ( unit < plan.AlwaysLoaded.size() )
            return RuntimeDataLayersOf( partition, plan.Composites.at( plan.AlwaysLoaded[unit] ).DataLayers );
        return plan.Cells.at( unit - plan.AlwaysLoaded.size() ).DataLayers;
    }

    // The state each layer of the world starts Play (and a packaged game) in, index = layer: a Runtime layer's
    // InitialRuntimeState. Refused, by the layer's name, for a Runtime layer that states none (CheckDataLayers
    // names the same Issue) - which state it starts in is not guessed.
    [[nodiscard]] inline Common::ResultStr<std::vector<DataLayerRuntimeState>>
    InitialDataLayerStates( const WorldPartitionSerialized& partition )
    {
        std::vector<DataLayerRuntimeState> states;
        if ( !partition.DataLayers.has_value() )
            return Common::MakeSuccess( std::move( states ) );
        for ( const DataLayerSerialized& layer : *partition.DataLayers )
        {
            if ( layer.Type == DataLayerType::Editor )
            {
                states.push_back( DataLayerRuntimeState::Activated );
                continue;
            }
            if ( !layer.InitialRuntimeState.has_value() )
                return Common::MakeError<std::vector<DataLayerRuntimeState>>( std::format(
                     "the Runtime data layer '{}' states no InitialRuntimeState (Unloaded, Loaded or Activated)",
                     layer.Name ) );
            states.push_back( *layer.InitialRuntimeState );
        }
        return Common::MakeSuccess( std::move( states ) );
    }

    // UE UWorldPartitionRuntimeCell::GetCellEffectiveWantedState: @p runtimeLayers' states combined by @p logic.
    [[nodiscard]] inline DataLayerRuntimeState
    EffectiveDataLayerState( WorldPartitionDataLayersLogicOperator logic, std::span<const DataLayerRuntimeState> layerStates,
                             std::span<const std::size_t> runtimeLayers )
    {
        if ( runtimeLayers.empty() )
            return DataLayerRuntimeState::Activated;
        const bool            any   = logic == WorldPartitionDataLayersLogicOperator::Or;
        DataLayerRuntimeState state = any ? DataLayerRuntimeState::Unloaded : DataLayerRuntimeState::Activated;
        for ( const std::size_t layer : runtimeLayers )
            state = any ? std::max( state, layerStates[layer] ) : std::min( state, layerStates[layer] );
        return state;
    }

    // UE UDataLayerInstance::GetEffectiveRuntimeState (WP v4, DataLayerSerialized::Parent): a runtime layer is
    // never further along than its parent - its own state clamped by every ancestor's, the least of the chain.
    // Index = the world's layer, as @p layerStates. Bounded by the layer count, so an unchecked cycle ends.
    [[nodiscard]] inline std::vector<DataLayerRuntimeState>
    EffectiveDataLayerStates( const WorldPartitionSerialized&        partition,
                              std::span<const DataLayerRuntimeState> layerStates )
    {
        std::vector<DataLayerRuntimeState> effective( layerStates.begin(), layerStates.end() );
        for ( std::size_t layer = 0; layer < effective.size(); ++layer )
        {
            std::optional<std::size_t> at = DataLayerParentIndex( partition, layer );
            for ( std::size_t steps = 0; at.has_value() && steps < effective.size(); ++steps )
            {
                if ( *at < layerStates.size() )
                    effective[layer] = std::min( effective[layer], layerStates[*at] );
                at = DataLayerParentIndex( partition, *at );
            }
        }
        return effective;
    }

    // Every unit's effective state, index = unit: what StepResidency's @p unitLayerStates takes. Each layer's
    // state is first clamped by its parents' (EffectiveDataLayerStates), then a unit's layers combine by the
    // operator.
    [[nodiscard]] inline std::vector<DataLayerRuntimeState>
    DataLayerUnitStates( const WorldPartitionPlan& plan, const WorldPartitionSerialized& partition,
                         std::span<const DataLayerRuntimeState> layerStates )
    {
        const std::vector<DataLayerRuntimeState> clamped = EffectiveDataLayerStates( partition, layerStates );
        std::vector<DataLayerRuntimeState> units( ResidencyUnitCount( plan ), DataLayerRuntimeState::Activated );
        for ( std::size_t unit = 0; unit < units.size(); ++unit )
            units[unit] = EffectiveDataLayerState( partition.DataLayersLogicOperator, clamped,
                                                   UnitRuntimeDataLayers( plan, partition, unit ) );
        return units;
    }

    // A unit's name for a report: "L1(3,-2)" for the level-1 cell at X 3, Z -2, "A5" for the sixth
    // always-loaded composite. Short, because a flight's CSV writes one per activation.
    [[nodiscard]] inline std::string DescribeResidencyUnit( const WorldPartitionPlan& plan, std::size_t unit )
    {
        if ( unit < plan.AlwaysLoaded.size() )
            return std::format( "A{}", unit );
        const PlannedCell& cell = plan.Cells.at( unit - plan.AlwaysLoaded.size() );
        // A cell of runtime data layers names its set (WP-DATA-LAYERS-RUNTIME): two cells of one square in
        // different sets are two units, and the saved HLOD index keys its rows by this name. "DL0+2" = layers 0, 2.
        std::string layers;
        for ( const std::size_t layer : cell.DataLayers )
            layers += std::format( "{}{}", layers.empty() ? "DL" : "+", layer );
        return std::format( "G{}L{}({},{}){}", cell.Grid, cell.Level, cell.Cell.X, cell.Cell.Z, layers );
    }

    // THE NEAREST AN HLOD OF @p unit IS EVER DRAWN (UE FHLODBuildContext::MinVisibleDistance, WP-FAR-6B): the
    // LoadingRange of its cell's grid - nearer, the cell itself is loaded. Cell units only (an always-loaded unit
    // has no HLOD); one statement for Play and the cook, which pick the landscape HLOD's LOD from it.
    [[nodiscard]] inline float HLODMinVisibleDistance( const WorldPartitionSerialized& partition,
                                                       const WorldPartitionPlan& plan, std::size_t unit )
    {
        return partition.Grids.at( plan.Cells.at( unit - plan.AlwaysLoaded.size() ).Grid ).LoadingRange;
    }

    // Records a unit brings into the world: the members of every composite it holds.
    [[nodiscard]] inline std::size_t ResidencyUnitRecords( const WorldPartitionPlan& plan, std::size_t unit )
    {
        const auto membersOf = [&plan]( std::size_t composite )
        { return plan.Composites.at( composite ).Members.size(); };
        if ( unit < plan.AlwaysLoaded.size() )
            return membersOf( plan.AlwaysLoaded[unit] );
        std::size_t records = 0;
        for ( const std::size_t composite : plan.Cells.at( unit - plan.AlwaysLoaded.size() ).Composites )
            records += membersOf( composite );
        return records;
    }

    // THE RECORDS OF ONE UNIT, AND THEIR ORDER: every composite the unit holds, in the plan's order, and each
    // composite's members in file order. One statement of it, because three readers must agree on it to the
    // record — the executor (which record indices a unit activates), the in-memory cell source (what it hands
    // the streamer) and the world cook (what a cell FILE holds, in what order). A cell file whose order
    // differed from the executor's would still load, and would hand record 3's entity record 4's id.
    [[nodiscard]] inline std::vector<std::size_t> ResidencyUnitMembers( const WorldPartitionPlan& plan,
                                                                        std::size_t               unit )
    {
        std::vector<std::size_t> members;
        members.reserve( ResidencyUnitRecords( plan, unit ) );
        const auto take = [&]( std::size_t composite )
        {
            const auto& held = plan.Composites.at( composite ).Members;
            members.insert( members.end(), held.begin(), held.end() );
        };
        if ( unit < plan.AlwaysLoaded.size() )
            take( plan.AlwaysLoaded[unit] );
        else
            for ( const std::size_t composite : plan.Cells.at( unit - plan.AlwaysLoaded.size() ).Composites )
                take( composite );
        return members;
    }

    [[nodiscard]] inline double ResidencyRetryDelay( const ResidencySettings& settings, std::uint32_t failures )
    {
        const double grown =
             settings.RetryDelaySeconds * std::pow( settings.RetryBackoff, static_cast<double>( failures ) - 1.0 );
        return std::min( grown, settings.MaxRetryDelaySeconds );
    }

    namespace Detail
    {
        [[nodiscard]] inline Common::ResultStr<bool> CheckResidencySettings( const ResidencySettings& s )
        {
            const auto finiteAtLeast = []( double value, double low )
            { return std::isfinite( value ) && value >= low; };
            if ( !finiteAtLeast( s.ActivationBudgetMs, 0.0 ) || !finiteAtLeast( s.MsPerRecord, 0.0 ) ||
                 !finiteAtLeast( s.UnloadMargin, 0.0 ) || s.MaxConcurrentLoads == 0 ||
                 !std::isfinite( s.RetryDelaySeconds ) || s.RetryDelaySeconds <= 0.0 ||
                 !finiteAtLeast( s.RetryBackoff, 1.0 ) ||
                 !finiteAtLeast( s.MaxRetryDelaySeconds, s.RetryDelaySeconds ) ||
                 !finiteAtLeast( s.SlowStreamingRatio, 0.0 ) || !finiteAtLeast( s.SlowStreamingWarningFactor, 1.0 ) )
            {
                return Common::MakeError<bool>( std::format(
                     "StepResidency: settings ActivationBudgetMs {:f}, MsPerRecord {:f}, UnloadMargin {:f}, "
                     "MaxConcurrentLoads {}, RetryDelaySeconds {:f}, RetryBackoff {:f}, MaxRetryDelaySeconds "
                     "{:f}, SlowStreamingRatio {:f}, SlowStreamingWarningFactor {:f}; budget, cost, margin and "
                     "ratio must be finite and >= 0, loads >= 1, the delay > 0, the backoff and the warning "
                     "factor >= 1 and the longest delay >= the first",
                     s.ActivationBudgetMs, s.MsPerRecord, s.UnloadMargin, s.MaxConcurrentLoads,
                     s.RetryDelaySeconds, s.RetryBackoff, s.MaxRetryDelaySeconds, s.SlowStreamingRatio,
                     s.SlowStreamingWarningFactor ) );
            }
            return Common::MakeSuccess( true );
        }

        // Units in priority order: always-loaded composites, then the wish's cells as the query ordered them.
        [[nodiscard]] inline std::vector<std::size_t> UnitsInOrder( const WorldPartitionPlan& plan,
                                                                    const StreamingWish&      wish )
        {
            std::vector<std::size_t> units;
            units.reserve( plan.AlwaysLoaded.size() + wish.Cells.size() );
            for ( std::size_t unit = 0; unit < plan.AlwaysLoaded.size(); ++unit )
                units.push_back( unit );
            for ( const WantedCell& cell : wish.Cells )
                units.push_back( plan.AlwaysLoaded.size() + cell.Cell );
            return units;
        }
    } // namespace Detail

    // ONE FRAME. @p state is the previous step's State (empty on the first frame); @p outcomes are the loader's
    // reports since then, in any order; @p nowSeconds is a monotonic clock, used for retries only.
    //
    // Refused by name and number rather than guessed at: settings out of range, a state sized for another plan
    // (the plan changed under a running world, and whose units the old entries describe is unknowable), an
    // outcome for a unit that does not exist or a failure without a reason, a clock that is not finite — and
    // whatever the streaming query itself refuses.
    [[nodiscard]] inline Common::ResultStr<ResidencyStep>
    StepResidency( const WorldPartitionPlan& plan, const WorldPartitionSerialized& partition,
                   std::span<const StreamingSource> sources, const ResidencySettings& settings,
                   ResidencyState state, std::span<const LoadOutcome> outcomes, double nowSeconds,
                   const ResidencyGate* gate = nullptr, std::span<const DataLayerRuntimeState> unitLayerStates = {} )
    {
        if ( auto valid = Detail::CheckResidencySettings( settings ); !valid.IsSuccess() )
            return Common::MakeError<ResidencyStep>( valid.GetError() );
        if ( !std::isfinite( nowSeconds ) )
            return Common::MakeError<ResidencyStep>(
                 std::format( "StepResidency: the clock reads {:f}", nowSeconds ) );

        const std::size_t unitCount = ResidencyUnitCount( plan );
        if ( state.Units.empty() )
            state.Units.resize( unitCount );
        if ( state.Units.size() != unitCount )
        {
            return Common::MakeError<ResidencyStep>(
                 std::format( "StepResidency: the residency state has {} unit(s) and the plan {} ({} "
                              "always-loaded + {} cells); a plan does not change under a running world",
                              state.Units.size(), unitCount, plan.AlwaysLoaded.size(), plan.Cells.size() ) );
        }

        if ( gate != nullptr && ( ( !gate->HoldActivation.empty() && gate->HoldActivation.size() != unitCount ) ||
                                  ( !gate->HoldRelease.empty() && gate->HoldRelease.size() != unitCount ) ) )
            return Common::MakeError<ResidencyStep>(
                 std::format( "StepResidency: the gate holds {} activation(s) and {} release(s) for {} unit(s)",
                              gate->HoldActivation.size(), gate->HoldRelease.size(), unitCount ) );
        const auto held = []( const std::vector<std::uint8_t>& holds, std::size_t unit )
        { return !holds.empty() && holds[unit] != 0; };
        if ( !unitLayerStates.empty() && unitLayerStates.size() != unitCount )
            return Common::MakeError<ResidencyStep>(
                 std::format( "StepResidency: {} data layer state(s) for {} unit(s)", unitLayerStates.size(), unitCount ) );
        // Empty = a world of no runtime layer: every unit Activated by its layers (see RUNTIME DATA LAYERS).
        const auto layerState = [&unitLayerStates]( std::size_t unit )
        { return unitLayerStates.empty() ? DataLayerRuntimeState::Activated : unitLayerStates[unit]; };

        ResidencyStep step;

        // 1. The loader's reports.
        for ( const LoadOutcome& outcome : outcomes )
        {
            if ( outcome.Unit >= unitCount )
            {
                return Common::MakeError<ResidencyStep>(
                     std::format( "StepResidency: a load outcome names unit {} of {}", outcome.Unit, unitCount ) );
            }
            if ( !outcome.Ok && outcome.Reason.empty() )
            {
                return Common::MakeError<ResidencyStep>( std::format(
                     "StepResidency: the load of unit {} (ticket {}) failed and the loader gave no reason",
                     outcome.Unit, outcome.Ticket ) );
            }
            UnitResidency& unit = state.Units[outcome.Unit];
            if ( unit.State != Residency::Loading || unit.Ticket != outcome.Ticket )
            {
                ++step.StaleOutcomes;
                continue;
            }
            if ( outcome.Ok )
            {
                unit.State    = Residency::Loaded;
                unit.Failures = 0;
            }
            else
            {
                unit.State         = Residency::Failed;
                unit.FailureReason = outcome.Reason;
                unit.FailedAt      = nowSeconds;
                ++unit.Failures;
            }
        }

        // 2. Where the sources are: what is wanted, and what may stay.
        auto wished = QueryStreamingCells( plan, partition, sources );
        if ( !wished.IsSuccess() )
            return Common::MakeError<ResidencyStep>( "StepResidency: " + wished.GetError() );
        std::vector<StreamingSource> widened( sources.begin(), sources.end() );
        for ( StreamingSource& source : widened )
        {
            source.RangeScale *= 1.0f + settings.UnloadMargin;
            source.AngleScale *=
                 1.0f + settings.UnloadMargin; // a sector's edge has the same band (UE's ExtraAngle)
            // The look-ahead's tip has the same band: the sweep (speed x LookAheadTime, capped at MaxLookAhead)
            // grows by the same factor, so a speed that jitters less than the margin does not unload the cell
            // the path's tip has just loaded.
            source.LookAheadTime *= 1.0f + settings.UnloadMargin;
            source.MaxLookAhead *= 1.0f + settings.UnloadMargin;
        }
        auto keptWish = QueryStreamingCells( plan, partition, widened );
        if ( !keptWish.IsSuccess() )
            return Common::MakeError<ResidencyStep>( "StepResidency: " + keptWish.GetError() );

        // A unit whose data layers are Unloaded is neither wanted nor kept, wherever the sources are.
        std::vector<std::size_t> wanted;
        for ( const std::size_t unit : Detail::UnitsInOrder( plan, wished.GetValue() ) )
            if ( layerState( unit ) != DataLayerRuntimeState::Unloaded )
                wanted.push_back( unit );
        std::vector<bool> kept( unitCount, false );
        for ( const std::size_t unit : Detail::UnitsInOrder( plan, keptWish.GetValue() ) )
            kept[unit] = layerState( unit ) != DataLayerRuntimeState::Unloaded;

        // 3. Whatever is outside the band leaves, in unit order.
        for ( std::size_t index = 0; index < unitCount; ++index )
        {
            UnitResidency& unit = state.Units[index];
            if ( kept[index] || unit.State == Residency::Unloaded )
                continue;
            if ( unit.State == Residency::Activated && gate != nullptr && held( gate->HoldRelease, index ) )
                continue;
            switch ( unit.State )
            {
                case Residency::Loading:
                    step.Actions.push_back( { ResidencyActionKind::CancelLoad, index, unit.Ticket } );
                    break;
                case Residency::Activated:
                    step.Actions.push_back( { ResidencyActionKind::Deactivate, index, 0 } );
                    step.Actions.push_back( { ResidencyActionKind::Unload, index, 0 } );
                    break;
                case Residency::Loaded:
                    step.Actions.push_back( { ResidencyActionKind::Unload, index, 0 } );
                    break;
                case Residency::Failed:
                case Residency::Unloaded:
                    break;
            }
            unit.State    = Residency::Unloaded;
            unit.Failures = 0;
            unit.FailureReason.clear();
        }

        // 3b. A kept Activated unit whose data layers went to Loaded stops being entities and stays read (UE: a cell
        //     of a Loaded layer is loaded and not activated). Not budgeted, as any deactivation.
        for ( std::size_t index = 0; index < unitCount; ++index )
        {
            UnitResidency& unit = state.Units[index];
            if ( !kept[index] || unit.State != Residency::Activated || layerState( index ) != DataLayerRuntimeState::Loaded )
                continue;
            if ( gate != nullptr && held( gate->HoldRelease, index ) )
                continue;
            step.Actions.push_back( { ResidencyActionKind::Deactivate, index, 0 } );
            unit.State = Residency::Loaded;
        }

        // 4. Loads, in priority order, up to the number in flight.
        auto inFlight = static_cast<std::size_t>( std::count_if( state.Units.begin(), state.Units.end(),
                                                                 []( const UnitResidency& unit )
                                                                 { return unit.State == Residency::Loading; } ) );
        for ( const std::size_t index : wanted )
        {
            if ( inFlight >= settings.MaxConcurrentLoads )
                break;
            UnitResidency& unit    = state.Units[index];
            const bool     retries = unit.State == Residency::Failed &&
                                 nowSeconds - unit.FailedAt >= ResidencyRetryDelay( settings, unit.Failures );
            if ( unit.State != Residency::Unloaded && !retries )
                continue;
            unit.State  = Residency::Loading;
            unit.Ticket = state.NextTicket++;
            unit.FailureReason.clear();
            step.Actions.push_back( { ResidencyActionKind::StartLoad, index, unit.Ticket } );
            ++inFlight;
        }

        // 5. Activations, in priority order, within the budget; the first always goes (see THE BUDGET).
        std::size_t activated   = 0;
        bool        budgetSpent = false;
        for ( const std::size_t index : wanted )
        {
            UnitResidency& unit = state.Units[index];
            if ( unit.State != Residency::Loaded )
                continue;
            if ( layerState( index ) != DataLayerRuntimeState::Activated )
            {
                ++step.HeldByDataLayers; // read, and its layers want it no further
                continue;
            }
            if ( gate != nullptr && held( gate->HoldActivation, index ) )
            {
                ++step.WaitingForGate;
                continue;
            }
            const double cost = static_cast<double>( ResidencyUnitRecords( plan, index ) ) * settings.MsPerRecord;
            if ( budgetSpent || ( activated > 0 && step.ActivationMs + cost > settings.ActivationBudgetMs ) )
            {
                budgetSpent = true;
                ++step.WaitingForBudget;
                continue;
            }
            ++activated;
            unit.State = Residency::Activated;
            step.ActivationMs += cost;
            step.Actions.push_back( { ResidencyActionKind::Activate, index, 0 } );
        }

        step.State = std::move( state );
        return Common::MakeSuccess( std::move( step ) );
    }
} // namespace Desert::Core::Rules
