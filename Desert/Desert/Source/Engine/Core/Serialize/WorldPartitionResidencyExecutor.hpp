#pragma once

// PERFORMING WHAT StepResidency DECIDES: WHICH RECORDS OF A PARTITIONED WORLD ARE ENTITIES RIGHT NOW.
//
// StepResidency (WorldPartitionResidencyRules.hpp) is a pure decision: it says "start loading unit 7, activate
// unit 3, deactivate unit 12". This file carries those actions out against a WORLD — the thing that turns
// records into entities and back — and keeps the two facts the decision cannot know: which records each unit
// brings, and which references between records point at something that is not resident.
//
// ── WHY THE WORLD IS AN INTERFACE ─────────────────────────────────────────────────────────────────
//
// The real world is a Scene fed by SceneSerializer, and no test project can compile Scene.cpp (it reaches the
// renderer). Behind `ResidencyWorld` the executor is pure: the suite Desert/Tests/Engine/WorldPartitionStreamer
// drives it with a world that keeps a set of live record indices, and the engine's WorldStreamer implements
// the same two calls with CreateEntityWithUUID / DestroyEntity. Everything worth asserting — what is resident
// after a fly-through, what a cross-cell reference does, what leaves at the start of Play — is decided here.
//
// ── THE LOAD IS REPORTED ON THE NEXT FRAME, NOT THIS ONE ──────────────────────────────────────────
//
// A load is the WORLD's (ResidencyWorld's load port): the engine's streamer reads a unit's cell on a JobSystem
// worker (WorldCellLoader.hpp) and a worker can only ever report on a later frame. So an outcome is handed to
// the step of a LATER frame, never fed back into this one — answering on the same frame would need a second
// StepResidency call per frame. A world whose records are already in memory (the port's default) reports on
// the very next frame; the price is one frame of latency per unit, which the unload margin already absorbs.
//
// ── THE UNIT'S ENTITIES ARE ITS RECORDS' IDS ──────────────────────────────────────────────────────
//
// Every record the executor takes must carry an id. The records come from the Play snapshot, which the saver
// writes with an id on every entity, so this is a check and not a restriction; a record without one is REFUSED
// by name, because its entity could never be found again to be destroyed. A prefab instance record is its
// root's id: DestroyEntity takes the whole tree, and the instance's inner entities go with it.
//
// ── REFERENCES ACROSS A CELL BOUNDARY: DEFERRED AND UNBOUND, COUNTED ─────────────────────────────────
//
// Containment never crosses a unit (a composite is never divided — WorldPartitionRules.hpp). Observation does,
// by design. The observations are every Observation Rules::ResolveEntityReferences finds (WP-ENTITY-REFS): the
// register's component fields - ids their readers resolve by lookup when they run (AttachmentSystem, the
// projectile owner, the landscape tile's root) - a script's soft Entity property, and a reference by name. A
// reference to a record that is not resident is DEFERRED; when its target activates while its holder is resident
// it is REBOUND, and when its target is destroyed while its holder stays it is UNBOUND. Each tick lists both
// (ResidencyTick::Rebound / Unbound) for the world to carry out where a value holds the resolved entity - a running
// script's `Properties.<name>` (ECS::EntityReferenceBindings) - and to say: UE's soft reference resolving while
// its actor is loaded, not a "null forever" taken at the holder's start.
//
// ── THE SLICES (WP-ACTIVATE-SLICED) ───────────────────────────────────────────────────────────────
//
// UE makes a streamed level part of the world over several frames (UWorld::AddToWorld: the actors' components
// register incrementally under s.LevelStreamingActorsUpdateTimeLimit) and makes it visible only at the last step
// (ULevel::bIsVisible); a primitive's physics state is created when it registers. Here the same: a unit the step
// lets through (an Activate action) is made a SLICE at a time — a run of whole composites, one Activate call of
// the world, which makes them HIDDEN and builds their bodies — timed on the world's clock, until the frame's
// ActivationBudgetMs (plus PriorityExtraMs while play waits for a cell) is spent. The slice is sized by the
// measured cost of its records' kinds (WorldPartitionActivationCost.hpp) so it ends inside the budget, and the
// clock is read after it. When the last slice is made the world's FinishActivation brings the unit's collision
// up to date and reveals it — or says it cannot yet (a body waits on its mesh, or the physics world of the Play
// is not made yet), and the unit stays Activating, hidden, until a later frame it can. Only then is it
// Activated: what the HLOD switch, the streaming assessment and the loading overlay wait for is a unit drawn and
// collidable at once.
//
// Where UE differs, on purpose: UE makes one level visible at a time (CurrentLevelPendingVisibility) and finishes
// it before the next begins. Here every advanced unit keeps its own cursor and the units are advanced in the
// step's priority order, so the cell a source has just walked into overtakes a far cell that is half made — its
// made part waits hidden — instead of the player waiting behind a cell he is walking away from.

#include <Engine/Core/Serialize/WorldPartitionActivationCost.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Core
{
    struct SceneSerialized;
}

namespace Desert::Core::Rules
{
    // What the executor needs from the place entities live. Record indices are into the span given to
    // ResidencyExecutor::Begin.
    class ResidencyWorld
    {
    public:
        virtual ~ResidencyWorld() = default;

        // Makes entities of ONE SLICE of unit @p unit (THE SLICES): @p records are the members
        // [@p firstMember, firstMember + records.size()) of ResidencyUnitMembers( plan, unit ), whole composites,
        // so a parent and its child always meet in the same stitch. A world that draws makes them hidden until
        // FinishActivation, and builds their bodies here, so the slice's time includes them. The unit is named
        // because a world that reads its records from somewhere (a cell source) asks by unit.
        [[nodiscard]] virtual Common::BoolResultStr Activate( std::size_t unit, std::size_t firstMember,
                                                              std::span<const std::size_t> records ) = 0;

        // Every record of @p unit (@p records, all its members) is an entity: bring its collision up to date and
        // reveal it. False — nothing revealed — while a body of the unit cannot be made yet (its mesh is still
        // being read, the Play's physics world does not exist yet): asked again on a later frame. The default is
        // a world that neither draws nor collides (the suites' sets of live indices): nothing to wait for.
        [[nodiscard]] virtual Common::ResultStr<bool> FinishActivation( std::size_t /*unit*/,
                                                                        std::span<const std::size_t> /*records*/ )
        {
            return Common::MakeSuccess( true );
        }

        // The kind of the unit's member @p member, for the slice estimate. Asked once per member when the unit's
        // first slice is planned, so only of a Loaded unit. The default is a world that holds no payloads (the
        // suites' index sets): every record is one kind to it.
        [[nodiscard]] virtual ActivationRecordKind ActivationKindOf( std::size_t /*unit*/,
                                                                     std::size_t /*member*/ ) const
        {
            return ActivationRecordKind::Plain;
        }

        // The clock the slices are timed on, milliseconds, monotonic. A suite overrides it to step time by hand.
        [[nodiscard]] virtual double ActivationClockMs() const
        {
            return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now().time_since_epoch() )
                 .count();
        }

        // Destroys the entities of these records. A record whose entity is already gone (a child taken with
        // its parent's tree) is not an error.
        virtual void Destroy( std::span<const std::size_t> records ) = 0;

        // THE LOAD PORT. A load fetches a unit's records from wherever the world keeps them, so that a later
        // Activate of that unit has them; its outcome is handed over by TakeLoadOutcomes, at the start of a
        // later Tick, and never from inside StartLoad. The defaults are a world whose records are already in
        // memory (the suites' worlds): every load is done at once and reported on the next Tick. The engine's
        // streamer overrides all four and reads on a JobSystem worker (WorldCellLoader.hpp).
        virtual void StartLoad( std::size_t unit, std::uint64_t ticket )
        {
            m_Finished.push_back( { unit, ticket, true, {} } );
        }
        // A load StepResidency no longer wants. Its outcome, if it arrives, is stale (StepResidency counts and
        // drops it by ticket), so an override may also drop it.
        virtual void CancelLoad( std::size_t /*unit*/, std::uint64_t /*ticket*/ )
        {
        }
        // The unit is neither activated nor loaded any more: what its load fetched may be freed.
        virtual void Unload( std::size_t /*unit*/ )
        {
        }
        [[nodiscard]] virtual std::vector<LoadOutcome> TakeLoadOutcomes()
        {
            return std::exchange( m_Finished, {} );
        }

    private:
        std::vector<LoadOutcome> m_Finished;
    };

    // A unit that became Activated this tick, and what its activation took (the streamer's log line).
    struct ActivationDone
    {
        std::size_t Unit    = kNoRecord;
        std::size_t Records = 0;
        std::size_t Slices  = 0;
        std::size_t Frames  = 0;   // ticks from its first slice to this one, both counted
        double      Ms      = 0.0; // slices and finishing, on the world's clock
    };

    // ONE FRAME'S ACTIVATION TIME, shared by the executors that tick in it (a world's cells and its HLOD cells,
    // WorldPartitionHLODSwitch.hpp TickWithHLODs), so the two together keep to one budget.
    struct ActivationFrame
    {
        std::optional<double> StartMs;            // the clock when the first executor began to activate
        bool                  Progressed = false; // something was made: the next slice must fit
    };

    struct ResidencyTick
    {
        std::size_t UnitsActivated   = 0;
        std::size_t UnitsDeactivated = 0;
        std::size_t RecordsActivated = 0;
        std::size_t RecordsDestroyed = 0;
        std::size_t LoadsStarted     = 0;
        // References from records activated this tick whose target is not resident at the end of it.
        std::size_t DeferredReferences = 0;
        // References from records resident at the end of this tick to a record this tick destroyed.
        std::size_t UnboundReferences = 0;
        std::size_t WaitingForBudget  = 0; // units let through that this frame's budget did not reach
        std::size_t WaitingForGate    = 0; // wanted Loaded/Activating units the step's ResidencyGate held back
        std::size_t HeldByDataLayers  = 0; // wanted Loaded units their data layers keep Loaded (WP-DATA-LAYERS-RUNTIME)
        std::size_t SlicesMade        = 0;
        double      ActivationMs      = 0.0; // the clock time of this tick's slices and finishes
        // Units whose every record is made but whose collision is not ready yet (FinishActivation said no).
        std::size_t                 WaitingForCollision = 0;
        std::vector<ActivationDone> Completed;
        std::size_t StaleOutcomes     = 0;
        // Units whose loaded records were let go this tick (after a Deactivate, or a Loaded unit leaving range).
        std::size_t UnitsUnloaded = 0;
        // (holder, target) record pairs of observation references: REBOUND - the target activated this tick and the
        // holder was resident before it (a holder activated with it starts bound); UNBOUND - the target was
        // destroyed this tick and the holder is resident at the end of it (WP-ENTITY-REFS).
        std::vector<std::pair<std::size_t, std::size_t>> Rebound;
        std::vector<std::pair<std::size_t, std::size_t>> Unbound;
    };

    // WHETHER THIS TICK LET GO OF WHAT A CELL HELD (WP13). A resident cell is what keeps its assets: its entities
    // are roots of the eviction trace, and so is a Loaded cell's record list. When a cell is deactivated or
    // unloaded those holds are gone, and an asset nothing else names can be released — UE's level streaming out
    // forces a collection for the same reason (GLevelStreamingForceGCAfterLevelStreamedOut). The streamer asks
    // for a sweep on this; the cells that leave within the schedule's two frames (EvictionDeadline) share one.
    [[nodiscard]] inline bool ReleasesCellAssets( const ResidencyTick& tick )
    {
        return tick.UnitsDeactivated > 0 || tick.UnitsUnloaded > 0;
    }

    class ResidencyExecutor
    {
    public:
        // Starts streaming a world whose records are ALL entities in @p world right now (Edit → Play): the
        // always-loaded units and the cells @p sources want stay as they are and are marked Activated; every
        // other unit's records are destroyed before this returns. The neighbourhood is therefore present on the
        // first frame of Play instead of streaming back in over the next few.
        //
        // Refused: a record without an id, a record no unit holds (the planner puts every record in exactly one
        // composite, so this is a plan that does not describe these records), and whatever StepResidency or the
        // query refuse.
        [[nodiscard]] static Common::ResultStr<ResidencyExecutor>
        Begin( WorldPartitionPlan plan, WorldPartitionSerialized partition,
               std::span<const Assets::EntityData> records, const ResidencySettings& settings,
               std::span<const StreamingSource> sources, ResidencyWorld& world )
        {
            for ( std::size_t record = 0; record < records.size(); ++record )
            {
                if ( !records[record].id.has_value() || records[record].id->IsNull() )
                {
                    return Common::MakeError<ResidencyExecutor>(
                         std::format( "ResidencyExecutor: record {} ('{}') has no id, so its entity could not be "
                                      "found again to be destroyed",
                                      record, records[record].Tag.value_or( "" ) ) );
                }
            }
            // The observations, as the plan resolved them (WorldPartitionPlan::Observations: register, scripts,
            // names - Rules::ResolveEntityReferences, once). What it could not resolve is in the plan's Issues,
            // which its caller reports (ReportPlanIssues); a target the file does not contain is dangling, not a
            // streaming matter.
            const std::vector<std::pair<std::size_t, std::size_t>> observations = plan.Observations;
            auto made = Make( std::move( plan ), std::move( partition ), settings, records.size(), observations );
            if ( !made.IsSuccess() )
                return made;
            ResidencyExecutor executor = made.ExtractValue();
            for ( std::size_t record = 0; record < records.size(); ++record )
                if ( executor.m_UnitOfRecord[record] == kNoRecord )
                {
                    return Common::MakeError<ResidencyExecutor>(
                         std::format( "ResidencyExecutor: record {} ('{}', id {}) belongs to no unit of the plan; "
                                      "the plan was not made from these records",
                                      record, records[record].Tag.value_or( "" ),
                                      static_cast<std::uint64_t>( *records[record].id ) ) );
                }
            // The units resident from the start: what the first step would want.
            auto wished = QueryStreamingCells( executor.m_Plan, executor.m_Partition, sources );
            if ( !wished.IsSuccess() )
                return Common::MakeError<ResidencyExecutor>( "ResidencyExecutor: " + wished.GetError() );
            // A wanted unit whose data layers are not Activated is destroyed with the rest: Unloaded is not read,
            // and Loaded is read by the first Tick (its records come back from the cell source, not from Edit).
            for ( const std::size_t unit : Detail::UnitsInOrder( executor.m_Plan, wished.GetValue() ) )
                if ( executor.m_UnitLayerStates[unit] == DataLayerRuntimeState::Activated )
                    executor.MarkActivated( unit );
            std::vector<std::size_t> leaving;
            for ( std::size_t record = 0; record < records.size(); ++record )
                if ( !executor.m_Live[record] )
                    leaving.push_back( record );
            world.Destroy( leaving );
            return Common::MakeSuccess( std::move( executor ) );
        }

        // Starts streaming a world of which ONLY THE ALWAYS-LOADED UNITS are entities right now (a cooked world:
        // its caller made them from the always-loaded cell file before this). Every cell is Unloaded, and the
        // first Tick starts loading the ones @p sources want; nothing is destroyed. @p recordCount records are
        // named by index only — their ids are the world's, which resolves them — and @p observations are the
        // observation references between them (from, to) that cross a unit, the only ones that can dangle.
        // @p madeCells: cell units the caller ALSO made entities of before this (the HLOD cells over the world
        // cells Edit -> Play kept, WorldStreamer::BeginHLODCells); they start Activated, read by nobody.
        [[nodiscard]] static Common::ResultStr<ResidencyExecutor>
        BeginFromAlwaysLoaded( WorldPartitionPlan plan, WorldPartitionSerialized partition,
                               const ResidencySettings& settings, std::size_t recordCount,
                               std::span<const std::pair<std::size_t, std::size_t>> observations,
                               std::span<const std::size_t>                         madeCells = {} )
        {
            auto made = Make( std::move( plan ), std::move( partition ), settings, recordCount, observations );
            if ( !made.IsSuccess() )
                return made;
            ResidencyExecutor executor = made.ExtractValue();
            // The always-loaded records the caller made: those of no runtime data layer. One in runtime layers is
            // in a file of its layers (WorldCells: AlwaysLoaded_DL...) and streams by their state from the first Tick.
            for ( std::size_t unit = 0; unit < executor.m_Plan.AlwaysLoaded.size(); ++unit )
                if ( UnitRuntimeDataLayers( executor.m_Plan, executor.m_Partition, unit ).empty() )
                    executor.MarkActivated( unit );
            for ( const std::size_t unit : madeCells )
            {
                if ( unit < executor.m_Plan.AlwaysLoaded.size() || unit >= ResidencyUnitCount( executor.m_Plan ) )
                    return Common::MakeError<ResidencyExecutor>(
                         std::format( "ResidencyExecutor: unit {} was made at the start and is no cell of {}",
                                      unit, ResidencyUnitCount( executor.m_Plan ) ) );
                executor.MarkActivated( unit );
            }
            for ( std::size_t record = 0; record < recordCount; ++record )
                if ( executor.m_UnitOfRecord[record] == kNoRecord )
                    return Common::MakeError<ResidencyExecutor>(
                         std::format( "ResidencyExecutor: record {} of {} belongs to no unit of the plan", record,
                                      recordCount ) );
            return Common::MakeSuccess( std::move( executor ) );
        }

        // ONE FRAME: step the residency with the loads the world finished since the last one, perform what leaves
        // and what loads in the order it gives them, then advance the units it lets through, slice by slice,
        // within the budget (THE SLICES). @p gate: what another residency holds back this step (null = none). @p
        // frame: the activation time already spent this frame by another executor (null = this one alone).
        [[nodiscard]] Common::ResultStr<ResidencyTick> Tick( std::span<const StreamingSource> sources,
                                                             double nowSeconds, ResidencyWorld& world,
                                                             const ResidencyGate* gate  = nullptr,
                                                             ActivationFrame*     frame = nullptr )
        {
            const std::vector<LoadOutcome> finished = world.TakeLoadOutcomes();
            auto stepped = StepResidency( m_Plan, m_Partition, sources, m_Settings, std::move( m_State ), finished,
                                          nowSeconds, gate, m_UnitLayerStates );
            if ( !stepped.IsSuccess() )
                return Common::MakeError<ResidencyTick>( stepped.GetError() );
            ResidencyStep step = stepped.ExtractValue();
            m_State            = std::move( step.State );
            ++m_Ticks;

            ResidencyTick            tick;
            std::vector<std::size_t> activated;
            std::vector<std::size_t> destroyed;
            std::vector<std::size_t> advance;
            tick.WaitingForGate   = step.WaitingForGate;
            tick.StaleOutcomes    = step.StaleOutcomes;
            tick.HeldByDataLayers = step.HeldByDataLayers;
            for ( const ResidencyAction& action : step.Actions )
            {
                const std::vector<std::size_t>& records = m_UnitRecords.at( action.Unit );
                switch ( action.Kind )
                {
                    case ResidencyActionKind::StartLoad:
                        world.StartLoad( action.Unit, action.Ticket );
                        ++tick.LoadsStarted;
                        break;
                    case ResidencyActionKind::CancelLoad:
                        world.CancelLoad( action.Unit, action.Ticket );
                        break;
                    case ResidencyActionKind::Activate:
                        advance.push_back( action.Unit );
                        break;
                    case ResidencyActionKind::Deactivate:
                        // A part-made unit is destroyed whole too: the world passes over records that are no
                        // entity.
                        world.Destroy( records );
                        for ( const std::size_t record : records )
                            m_Live[record] = false;
                        destroyed.insert( destroyed.end(), records.begin(), records.end() );
                        m_Jobs.erase( action.Unit );
                        ++tick.UnitsDeactivated;
                        tick.RecordsDestroyed += records.size();
                        break;
                    case ResidencyActionKind::Unload:
                        world.Unload( action.Unit );
                        ++tick.UnitsUnloaded;
                        break;
                }
            }

            ActivationFrame  own;
            ActivationFrame& shared = frame != nullptr ? *frame : own;
            if ( !shared.StartMs.has_value() )
                shared.StartMs = world.ActivationClockMs();
            const double budget =
                 m_Settings.ActivationBudgetMs + ( m_PriorityActivation ? m_Settings.PriorityExtraMs : 0.0 );
            bool spent = false;
            for ( const std::size_t unit : advance )
            {
                if ( spent )
                {
                    ++tick.WaitingForBudget;
                    continue;
                }
                auto advanced = Advance( unit, world, shared, budget, tick, activated );
                if ( !advanced.IsSuccess() )
                    return Common::MakeError<ResidencyTick>( advanced.GetError() );
                spent = !advanced.GetValue();
                if ( spent )
                    ++tick.WaitingForBudget;
            }

            for ( const std::size_t record : activated )
                for ( const std::size_t target : m_RefsFrom[record] )
                    tick.DeferredReferences += m_Live[target] ? 0 : 1;
            if ( !activated.empty() )
            {
                // A holder activated with its target this tick starts bound (its references resolve at its start);
                // one resident before the target arrived holds what it read then, and is rebound.
                std::vector<bool> arrived( m_Live.size(), false );
                for ( const std::size_t record : activated )
                    arrived[record] = true;
                for ( const std::size_t record : activated )
                    if ( m_Live[record] )
                        for ( const std::size_t referrer : m_RefsTo[record] )
                            if ( m_Live[referrer] && !arrived[referrer] )
                                tick.Rebound.emplace_back( referrer, record );
            }
            for ( const std::size_t record : destroyed )
                if ( !m_Live[record] )
                    for ( const std::size_t referrer : m_RefsTo[record] )
                        if ( m_Live[referrer] )
                            tick.Unbound.emplace_back( referrer, record );
            tick.UnboundReferences = tick.Unbound.size();
            return Common::MakeSuccess( tick );
        }

        [[nodiscard]] const ResidencyState& State() const
        {
            return m_State;
        }

        // UE UDataLayerManager::SetDataLayerInstanceRuntimeState: layer @p layer (index into the world's DataLayers)
        // goes to @p state; the next Tick loads, activates, deactivates or releases its units accordingly. Refused
        // for a layer that does not exist or is an Editor layer (it has no runtime state).
        [[nodiscard]] Common::ResultStr<bool> SetDataLayerState( std::size_t layer, DataLayerRuntimeState state )
        {
            if ( layer >= m_LayerStates.size() || !m_Partition.DataLayers.has_value() )
                return Common::MakeError<bool>(
                     std::format( "data layer {} of {}", layer, m_LayerStates.size() ) );
            if ( ( *m_Partition.DataLayers )[layer].Type != DataLayerType::Runtime )
                return Common::MakeError<bool>( std::format( "the data layer '{}' is an Editor layer and has no "
                                                             "runtime state",
                                                             ( *m_Partition.DataLayers )[layer].Name ) );
            m_LayerStates[layer] = state;
            m_UnitLayerStates    = DataLayerUnitStates( m_Plan, m_Partition, m_LayerStates );
            return Common::MakeSuccess( true );
        }

        // Each world layer's runtime state (index = layer; an Editor layer's is Activated: it gates nothing).
        [[nodiscard]] const std::vector<DataLayerRuntimeState>& DataLayerStates() const
        {
            return m_LayerStates;
        }

        // Each unit's effective state (DataLayerUnitStates of the above).
        [[nodiscard]] const std::vector<DataLayerRuntimeState>& UnitDataLayerStates() const
        {
            return m_UnitLayerStates;
        }

        // While play waits for a cell under a source (StreamingAssessment::Blocks), activation gets
        // PriorityExtraMs more (UE's priority level streaming time). Set by the host before each Tick.
        void SetPriorityActivation( bool priority )
        {
            m_PriorityActivation = priority;
        }

        // What making records of each kind has cost this session (THE SLICES).
        [[nodiscard]] const ActivationCostModel& ActivationCost() const
        {
            return m_Cost;
        }

        // The residency unit holding @p record.
        [[nodiscard]] std::size_t UnitOf( std::size_t record ) const
        {
            return m_UnitOfRecord.at( record );
        }

        [[nodiscard]] bool IsLive( std::size_t record ) const
        {
            return m_Live.at( record );
        }

        [[nodiscard]] std::size_t LiveRecords() const
        {
            return static_cast<std::size_t>( std::count( m_Live.begin(), m_Live.end(), true ) );
        }

        // References from live records naming an entity that is not live (they resolve when it arrives): what the
        // start leaves unresolved, said once as each tick says the references of the records it activated.
        [[nodiscard]] std::size_t UnresolvedReferences() const
        {
            std::size_t count = 0;
            for ( std::size_t record = 0; record < m_RefsFrom.size(); ++record )
                if ( m_Live[record] )
                    for ( const std::size_t target : m_RefsFrom[record] )
                        count += m_Live[target] ? 0 : 1;
            return count;
        }

        [[nodiscard]] const WorldPartitionPlan& Plan() const
        {
            return m_Plan;
        }

    private:
        // Only Begin makes one; the result type needs to name the constructor for its failed state.
        friend class Common::ResultStr<ResidencyExecutor>;
        ResidencyExecutor() = default;

        // What both beginnings share: the plan, the per-unit record lists, and the observation register.
        [[nodiscard]] static Common::ResultStr<ResidencyExecutor>
        Make( WorldPartitionPlan plan, WorldPartitionSerialized partition, const ResidencySettings& settings,
              std::size_t recordCount, std::span<const std::pair<std::size_t, std::size_t>> observations )
        {
            if ( auto valid = Detail::CheckResidencySettings( settings ); !valid.IsSuccess() )
                return Common::MakeError<ResidencyExecutor>( valid.GetError() );
            ResidencyExecutor executor;
            executor.m_Plan             = std::move( plan );
            executor.m_Partition        = std::move( partition );
            executor.m_Settings         = settings;
            const std::size_t unitCount = ResidencyUnitCount( executor.m_Plan );
            executor.m_UnitRecords.resize( unitCount );
            executor.m_UnitOfRecord.assign( recordCount, kNoRecord );
            // Play and a packaged game alike start every runtime layer in its InitialRuntimeState.
            auto initial = InitialDataLayerStates( executor.m_Partition );
            if ( !initial.IsSuccess() )
                return Common::MakeError<ResidencyExecutor>( "ResidencyExecutor: " + initial.GetError() );
            executor.m_LayerStates     = initial.ExtractValue();
            executor.m_UnitLayerStates = DataLayerUnitStates( executor.m_Plan, executor.m_Partition, executor.m_LayerStates );
            for ( std::size_t unit = 0; unit < unitCount; ++unit )
            {
                executor.m_UnitRecords[unit] = ResidencyUnitMembers( executor.m_Plan, unit );
                for ( const std::size_t record : executor.m_UnitRecords[unit] )
                {
                    if ( record >= recordCount )
                        return Common::MakeError<ResidencyExecutor>(
                             std::format( "ResidencyExecutor: unit {} names record {} of {}; the plan was not "
                                          "made from these records",
                                          unit, record, recordCount ) );
                    executor.m_UnitOfRecord[record] = unit;
                }
            }
            executor.m_RefsFrom.resize( recordCount );
            executor.m_RefsTo.resize( recordCount );
            for ( const auto& [from, to] : observations )
            {
                if ( from >= recordCount || to >= recordCount )
                    return Common::MakeError<ResidencyExecutor>(
                         std::format( "ResidencyExecutor: a reference names record {} of {}", std::max( from, to ),
                                      recordCount ) );
                executor.m_RefsFrom[from].push_back( to );
                executor.m_RefsTo[to].push_back( from );
            }
            executor.m_State.Units.resize( unitCount );
            executor.m_Live.assign( recordCount, false );
            return Common::MakeSuccess( std::move( executor ) );
        }

        // One unit's activation in progress: its members' kinds, its composites' ends, and how far it is.
        struct ActivationJob
        {
            std::vector<ActivationRecordKind> Kinds;
            std::vector<std::size_t>          CompositeEnds;
            std::size_t                       Cursor    = 0;
            std::size_t                       Slices    = 0;
            std::uint64_t                     FirstTick = 0;
            double                            Ms        = 0.0;
        };

        [[nodiscard]] ActivationJob StartJob( std::size_t unit, const ResidencyWorld& world ) const
        {
            ActivationJob job;
            job.FirstTick                           = m_Ticks;
            const std::vector<std::size_t>& members = m_UnitRecords[unit];
            job.Kinds.reserve( members.size() );
            for ( std::size_t member = 0; member < members.size(); ++member )
                job.Kinds.push_back( world.ActivationKindOf( unit, member ) );
            // The composites in ResidencyUnitMembers' order, each one's members together.
            const auto sizeOf = [this]( std::size_t composite )
            { return m_Plan.Composites.at( composite ).Members.size(); };
            std::size_t end   = 0;
            const auto  close = [&]( std::size_t composite )
            {
                end += sizeOf( composite );
                if ( job.CompositeEnds.empty() || job.CompositeEnds.back() != end )
                    job.CompositeEnds.push_back( end );
            };
            if ( unit < m_Plan.AlwaysLoaded.size() )
                close( m_Plan.AlwaysLoaded[unit] );
            else
                for ( const std::size_t composite :
                      m_Plan.Cells.at( unit - m_Plan.AlwaysLoaded.size() ).Composites )
                    close( composite );
            return job;
        }

        // Advances @p unit as far as the frame's budget goes. True when the budget is not spent (the unit is
        // done, or waits for its collision); false when the frame ran out of time inside it.
        [[nodiscard]] Common::ResultStr<bool> Advance( std::size_t unit, ResidencyWorld& world,
                                                       ActivationFrame& frame, double budget, ResidencyTick& tick,
                                                       std::vector<std::size_t>& activated )
        {
            UnitResidency& state = m_State.Units[unit];
            auto           found = m_Jobs.find( unit );
            if ( found == m_Jobs.end() )
                found = m_Jobs.emplace( unit, StartJob( unit, world ) ).first;
            ActivationJob&                  job     = found->second;
            const std::vector<std::size_t>& records = m_UnitRecords[unit];
            const auto                      elapsed = [&]() { return world.ActivationClockMs() - *frame.StartMs; };

            while ( job.Cursor < records.size() )
            {
                const double      used = elapsed();
                const std::size_t end =
                     NextActivationSlice( job.CompositeEnds, job.Kinds, job.Cursor, m_Cost, budget - used );
                const std::span<const ActivationRecordKind> kinds =
                     std::span<const ActivationRecordKind>( job.Kinds ).subspan( job.Cursor, end - job.Cursor );
                // The first slice of a frame always goes; after it, a slice must fit what is left.
                if ( frame.Progressed && ( used >= budget || ( m_Cost.AllMeasured( kinds ) &&
                                                               used + m_Cost.Estimate( kinds ) > budget ) ) )
                    return Common::MakeSuccess( false );

                const std::span<const std::size_t> slice =
                     std::span<const std::size_t>( records ).subspan( job.Cursor, end - job.Cursor );
                const double before = world.ActivationClockMs();
                if ( auto made = world.Activate( unit, job.Cursor, slice ); !made.IsSuccess() )
                {
                    return Common::MakeError<bool>(
                         std::format( "ResidencyExecutor: activating unit {} (records {}..{} of {}) failed: {}",
                                      unit, job.Cursor, end, records.size(), made.GetError() ) );
                }
                const double ms = world.ActivationClockMs() - before;
                m_Cost.Observe( kinds, ms );
                for ( const std::size_t record : slice )
                    m_Live[record] = true;
                activated.insert( activated.end(), slice.begin(), slice.end() );
                tick.RecordsActivated += slice.size();
                tick.ActivationMs += ms;
                ++tick.SlicesMade;
                ++job.Slices;
                job.Ms += ms;
                job.Cursor       = end;
                state.State      = Residency::Activating;
                state.Made       = end;
                frame.Progressed = true;
            }

            if ( frame.Progressed && elapsed() >= budget )
                return Common::MakeSuccess( false );
            const double before   = world.ActivationClockMs();
            auto         finished = world.FinishActivation( unit, records );
            const double ms       = world.ActivationClockMs() - before;
            if ( !finished.IsSuccess() )
                return Common::MakeError<bool>(
                     std::format( "ResidencyExecutor: finishing the activation of unit {} "
                                  "({} record(s)) failed: {}",
                                  unit, records.size(), finished.GetError() ) );
            tick.ActivationMs += ms;
            job.Ms += ms;
            // Only a finish that revealed the unit spends the frame's first-slice guarantee: a unit asked again
            // every frame while its collision waits must not hold back the slices of the units behind it.
            if ( !finished.GetValue() )
            {
                state.State = Residency::Activating;
                state.Made  = records.size();
                ++tick.WaitingForCollision;
                return Common::MakeSuccess( true );
            }
            frame.Progressed = true;
            state.State      = Residency::Activated;
            state.Made       = 0;
            ++tick.UnitsActivated;
            tick.Completed.push_back( { unit, records.size(), job.Slices,
                                        static_cast<std::size_t>( m_Ticks - job.FirstTick + 1 ), job.Ms } );
            m_Jobs.erase( found );
            return Common::MakeSuccess( true );
        }

        void MarkActivated( std::size_t unit )
        {
            m_State.Units[unit].State = Residency::Activated;
            for ( const std::size_t record : m_UnitRecords[unit] )
                m_Live[record] = true;
        }

        WorldPartitionPlan       m_Plan;
        WorldPartitionSerialized m_Partition;
        ResidencySettings        m_Settings;
        ResidencyState           m_State;

        std::vector<std::vector<std::size_t>> m_UnitRecords;  // per unit: its records, in composite order
        std::vector<std::size_t>              m_UnitOfRecord; // per record
        std::vector<std::vector<std::size_t>> m_RefsFrom;     // per record: observation targets
        std::vector<std::vector<std::size_t>> m_RefsTo;       // per record: who observes it
        std::vector<bool>                     m_Live;         // per record: an entity right now
        std::vector<DataLayerRuntimeState>    m_LayerStates;     // per world data layer (SetDataLayerState)
        std::vector<DataLayerRuntimeState>    m_UnitLayerStates; // per unit: DataLayerUnitStates of the above

        ActivationCostModel                            m_Cost;
        std::unordered_map<std::size_t, ActivationJob> m_Jobs; // per Activating unit
        std::uint64_t                                  m_Ticks              = 0;
        bool                                           m_PriorityActivation = false;
    };

    // THE ONE DECISION AT THE START OF PLAY: does this world stream? Only a scene whose file states a
    // WorldPartition block does (SceneFormat.hpp: "a world is partitioned or it is not, and this is never an
    // engine mode"). For any other scene this returns nullopt WITHOUT TOUCHING @p world — the scene plays with
    // every entity it had in Edit, exactly as before streaming existed.
    [[nodiscard]] Common::ResultStr<std::optional<ResidencyExecutor>>
    BeginWorldStreaming( const SceneSerialized& scene, const AssetBoundsSource& bounds,
                         const ResidencySettings& settings, std::span<const StreamingSource> sources,
                         ResidencyWorld& world, const ScriptReferenceSource& scripts = {} );
} // namespace Desert::Core::Rules
