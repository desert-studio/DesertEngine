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
// range grown by `UnloadMargin` still wants it, and unloaded the frame it is not. The band between the two
// radii is where nothing changes: no load starts there and nothing resident leaves.
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

#include <Common/Core/ResultStr.hpp>
#include <Engine/Core/Serialize/SceneFormat.hpp>
#include <Engine/Core/Serialize/WorldPartitionRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingRules.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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

    struct ResidencySettings
    {
        // Main-thread time one frame may spend activating, milliseconds.
        double ActivationBudgetMs = 2.0;
        // Activation cost of one record, MEASURED (WP5b, 2026-09-24): WorldStreamer times every
        // SceneSerializer::InstantiateRecords it makes and logs the total at the end of Play. Release editor,
        // World_Grid8km with a 128 m grid (StaticMesh cubes, ~9 records a unit), headless --play flights across
        // the map on an M-series Mac: 0.0152 ms/record over 3350 records, 0.0198 over 3159. Rounded UP to the
        // worse run, so a frame is not promised more than it gets; a world of heavier records (prefab
        // instances, skinned meshes) is a new measurement, and the streamer's log line is where it comes from.
        double MsPerRecord = 0.02;
        // The hysteresis band: a resident unit is kept while within LoadingRange·RangeScale·(1 + UnloadMargin).
        float UnloadMargin = 0.25f;
        // Loads in flight at once.
        std::uint32_t MaxConcurrentLoads = 4;
        // The wait before retrying a failed load, growing by RetryBackoff per consecutive failure.
        double RetryDelaySeconds    = 1.0;
        double RetryBackoff         = 2.0;
        double MaxRetryDelaySeconds = 30.0;
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
        std::size_t                  StaleOutcomes    = 0;
    };

    [[nodiscard]] inline std::size_t ResidencyUnitCount( const WorldPartitionPlan& plan )
    {
        return plan.AlwaysLoaded.size() + plan.Cells.size();
    }

    // A unit's name for a report: "L1(3,-2)" for the level-1 cell at X 3, Z -2, "A5" for the sixth
    // always-loaded composite. Short, because a flight's CSV writes one per activation.
    [[nodiscard]] inline std::string DescribeResidencyUnit( const WorldPartitionPlan& plan, std::size_t unit )
    {
        if ( unit < plan.AlwaysLoaded.size() )
            return "A" + std::to_string( unit );
        const PlannedCell& cell = plan.Cells.at( unit - plan.AlwaysLoaded.size() );
        return "L" + std::to_string( cell.Level ) + "(" + std::to_string( cell.Cell.X ) + "," +
               std::to_string( cell.Cell.Z ) + ")";
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
                 !( std::isfinite( s.RetryDelaySeconds ) && s.RetryDelaySeconds > 0.0 ) ||
                 !finiteAtLeast( s.RetryBackoff, 1.0 ) ||
                 !finiteAtLeast( s.MaxRetryDelaySeconds, s.RetryDelaySeconds ) )
            {
                return Common::MakeError<bool>(
                     "StepResidency: settings ActivationBudgetMs " + std::to_string( s.ActivationBudgetMs ) +
                     ", MsPerRecord " + std::to_string( s.MsPerRecord ) + ", UnloadMargin " +
                     std::to_string( s.UnloadMargin ) + ", MaxConcurrentLoads " +
                     std::to_string( s.MaxConcurrentLoads ) + ", RetryDelaySeconds " +
                     std::to_string( s.RetryDelaySeconds ) + ", RetryBackoff " + std::to_string( s.RetryBackoff ) +
                     ", MaxRetryDelaySeconds " + std::to_string( s.MaxRetryDelaySeconds ) +
                     "; budget, cost and margin must be finite and >= 0, loads >= 1, the delay > 0, the backoff "
                     ">= 1 and the longest delay >= the first" );
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
                   ResidencyState state, std::span<const LoadOutcome> outcomes, double nowSeconds )
    {
        if ( auto valid = Detail::CheckResidencySettings( settings ); !valid.IsSuccess() )
            return Common::MakeError<ResidencyStep>( valid.GetError() );
        if ( !std::isfinite( nowSeconds ) )
            return Common::MakeError<ResidencyStep>( "StepResidency: the clock reads " +
                                                     std::to_string( nowSeconds ) );

        const std::size_t unitCount = ResidencyUnitCount( plan );
        if ( state.Units.empty() )
            state.Units.resize( unitCount );
        if ( state.Units.size() != unitCount )
        {
            return Common::MakeError<ResidencyStep>(
                 "StepResidency: the residency state has " + std::to_string( state.Units.size() ) +
                 " unit(s) and the plan " + std::to_string( unitCount ) + " (" +
                 std::to_string( plan.AlwaysLoaded.size() ) + " always-loaded + " +
                 std::to_string( plan.Cells.size() ) + " cells); a plan does not change under a running world" );
        }

        ResidencyStep step;

        // 1. The loader's reports.
        for ( const LoadOutcome& outcome : outcomes )
        {
            if ( outcome.Unit >= unitCount )
            {
                return Common::MakeError<ResidencyStep>( "StepResidency: a load outcome names unit " +
                                                         std::to_string( outcome.Unit ) + " of " +
                                                         std::to_string( unitCount ) );
            }
            if ( !outcome.Ok && outcome.Reason.empty() )
            {
                return Common::MakeError<ResidencyStep>(
                     "StepResidency: the load of unit " + std::to_string( outcome.Unit ) + " (ticket " +
                     std::to_string( outcome.Ticket ) + ") failed and the loader gave no reason" );
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
            source.RangeScale *= 1.0f + settings.UnloadMargin;
        auto keptWish = QueryStreamingCells( plan, partition, widened );
        if ( !keptWish.IsSuccess() )
            return Common::MakeError<ResidencyStep>( "StepResidency: " + keptWish.GetError() );

        const std::vector<std::size_t> wanted = Detail::UnitsInOrder( plan, wished.GetValue() );
        std::vector<bool>              kept( unitCount, false );
        for ( const std::size_t unit : Detail::UnitsInOrder( plan, keptWish.GetValue() ) )
            kept[unit] = true;

        // 3. Whatever is outside the band leaves, in unit order.
        for ( std::size_t index = 0; index < unitCount; ++index )
        {
            UnitResidency& unit = state.Units[index];
            if ( kept[index] || unit.State == Residency::Unloaded )
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

        // 4. Loads, in priority order, up to the number in flight.
        std::size_t inFlight = static_cast<std::size_t>(
             std::count_if( state.Units.begin(), state.Units.end(),
                            []( const UnitResidency& unit ) { return unit.State == Residency::Loading; } ) );
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
