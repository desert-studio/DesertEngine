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

#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Core::Rules
{
    // What the executor needs from the place entities live. Record indices are into the span given to
    // ResidencyExecutor::Begin.
    class ResidencyWorld
    {
    public:
        virtual ~ResidencyWorld() = default;

        // Makes entities of unit @p unit, whole: every record of a unit in one call, so a parent and its child
        // always meet in the same stitch. @p records are ResidencyUnitMembers( plan, unit ) — the unit is named
        // as well because a world that reads its records from somewhere (a cell source) asks by unit.
        [[nodiscard]] virtual Common::BoolResultStr Activate( std::size_t                  unit,
                                                              std::span<const std::size_t> records ) = 0;

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
        std::size_t WaitingForBudget  = 0;
        std::size_t WaitingForGate    = 0; // wanted Loaded units the step's ResidencyGate held back
        std::size_t HeldByDataLayers  = 0; // wanted Loaded units their data layers keep Loaded (WP-DATA-LAYERS-RUNTIME)
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

        // ONE FRAME: step the residency with the loads the world finished since the last one, then perform its
        // actions in the order it gives them. @p gate: what another residency holds back this step (null = none).
        [[nodiscard]] Common::ResultStr<ResidencyTick> Tick( std::span<const StreamingSource> sources,
                                                             double nowSeconds, ResidencyWorld& world,
                                                             const ResidencyGate* gate = nullptr )
        {
            const std::vector<LoadOutcome> finished = world.TakeLoadOutcomes();
            auto stepped = StepResidency( m_Plan, m_Partition, sources, m_Settings, std::move( m_State ), finished,
                                          nowSeconds, gate, m_UnitLayerStates );
            if ( !stepped.IsSuccess() )
                return Common::MakeError<ResidencyTick>( stepped.GetError() );
            ResidencyStep step = stepped.ExtractValue();
            m_State            = std::move( step.State );

            ResidencyTick            tick;
            std::vector<std::size_t> activated;
            std::vector<std::size_t> destroyed;
            tick.WaitingForBudget = step.WaitingForBudget;
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
                        if ( auto made = world.Activate( action.Unit, records ); !made.IsSuccess() )
                        {
                            return Common::MakeError<ResidencyTick>(
                                 std::format( "ResidencyExecutor: activating unit {} ({} record(s)) failed: {}",
                                              action.Unit, records.size(), made.GetError() ) );
                        }
                        for ( const std::size_t record : records )
                            m_Live[record] = true;
                        activated.insert( activated.end(), records.begin(), records.end() );
                        ++tick.UnitsActivated;
                        tick.RecordsActivated += records.size();
                        break;
                    case ResidencyActionKind::Deactivate:
                        world.Destroy( records );
                        for ( const std::size_t record : records )
                            m_Live[record] = false;
                        destroyed.insert( destroyed.end(), records.begin(), records.end() );
                        ++tick.UnitsDeactivated;
                        tick.RecordsDestroyed += records.size();
                        break;
                    case ResidencyActionKind::Unload:
                        world.Unload( action.Unit );
                        ++tick.UnitsUnloaded;
                        break;
                }
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
    };

    // THE ONE DECISION AT THE START OF PLAY: does this world stream? Only a scene whose file states a
    // WorldPartition block does (SceneFormat.hpp: "a world is partitioned or it is not, and this is never an
    // engine mode"). For any other scene this returns nullopt WITHOUT TOUCHING @p world — the scene plays with
    // every entity it had in Edit, exactly as before streaming existed.
    [[nodiscard]] inline Common::ResultStr<std::optional<ResidencyExecutor>>
    BeginWorldStreaming( const SceneSerialized& scene, const AssetBoundsSource& bounds,
                         const ResidencySettings& settings, std::span<const StreamingSource> sources,
                         ResidencyWorld& world, const ScriptReferenceSource& scripts = {} )
    {
        if ( !scene.WorldPartition.has_value() )
            return Common::MakeSuccess( std::optional<ResidencyExecutor>() );
        WorldPartitionPlan plan = PlanWorldPartition( scene.Entities, *scene.WorldPartition, bounds, scripts );
        // Play names what the planner refused exactly as the cook does (ReportPlanIssues).
        ReportPlanIssues( plan, scene.SceneName );
        auto begun = ResidencyExecutor::Begin( std::move( plan ), *scene.WorldPartition, scene.Entities, settings,
                                               sources, world );
        if ( !begun.IsSuccess() )
            return Common::MakeError<std::optional<ResidencyExecutor>>( "'" + scene.SceneName +
                                                                        "': " + begun.GetError() );
        return Common::MakeSuccess( std::optional<ResidencyExecutor>( begun.ExtractValue() ) );
    }
} // namespace Desert::Core::Rules
