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
// by design, and every observation field in `kEntityReferences` is an id that its reader resolves by lookup when
// it runs — AttachmentSystem, the projectile owner, the landscape tile's root — and treats "not found" as null.
// So nothing has to be rewritten when a target comes or goes: a reference to a record that is not resident
// resolves the frame its target activates (DEFERRED), and one whose target is destroyed reads null from the
// next lookup (UNBOUND). What the executor adds is that neither happens silently: each tick counts both, from
// the register, and the world logs them. References by NAME (`kEntityReferencesByName`, UI overlays) are not
// counted here: their targets are UI trees under a screen-space canvas, which is always-loaded.

#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
        std::size_t StaleOutcomes     = 0;
    };

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
            std::unordered_map<Common::UUID, std::size_t> byId;
            for ( std::size_t record = 0; record < records.size(); ++record )
            {
                if ( !records[record].id.has_value() || records[record].id->IsNull() )
                {
                    return Common::MakeError<ResidencyExecutor>(
                         "ResidencyExecutor: record " + std::to_string( record ) + " ('" +
                         records[record].Tag.value_or( "" ) +
                         "') has no id, so its entity could not be found again to be destroyed" );
                }
                byId.emplace( *records[record].id, record );
            }
            // The observation register, resolved to record indices once. A target the file does not contain is
            // dangling, not a streaming matter — the planner already names those.
            std::vector<std::pair<std::size_t, std::size_t>> observations;
            Common::Json::Issues                             issues;
            for ( std::size_t record = 0; record < records.size(); ++record )
                for ( const EntityReferenceRow& row : kEntityReferences )
                {
                    if ( row.Kind != ReferenceKind::Observation )
                        continue;
                    const auto   block = Detail::BlockOf( records[record], row.ComponentKey, issues );
                    Common::UUID target;
                    if ( !block.has_value() || !Detail::ReadReference( *block, row.Field, target, issues ) ||
                         target.IsNull() )
                        continue;
                    const auto found = byId.find( target );
                    if ( found != byId.end() )
                        observations.emplace_back( record, found->second );
                }
            // An observation field of the wrong type observes nothing; it is named, not skipped in silence.
            Common::Json::ReportIssues( issues, "world partition observation references" );
            auto made = Make( std::move( plan ), std::move( partition ), settings, records.size(), observations );
            if ( !made.IsSuccess() )
                return made;
            ResidencyExecutor executor = made.ExtractValue();
            for ( std::size_t record = 0; record < records.size(); ++record )
                if ( executor.m_UnitOfRecord[record] == kNoRecord )
                {
                    return Common::MakeError<ResidencyExecutor>(
                         "ResidencyExecutor: record " + std::to_string( record ) + " ('" +
                         records[record].Tag.value_or( "" ) + "', id " +
                         std::to_string( static_cast<std::uint64_t>( *records[record].id ) ) +
                         ") belongs to no unit of the plan; the plan was not made from these records" );
                }
            // The units resident from the start: what the first step would want.
            auto wished = QueryStreamingCells( executor.m_Plan, executor.m_Partition, sources );
            if ( !wished.IsSuccess() )
                return Common::MakeError<ResidencyExecutor>( "ResidencyExecutor: " + wished.GetError() );
            for ( const std::size_t unit : Detail::UnitsInOrder( executor.m_Plan, wished.GetValue() ) )
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
        [[nodiscard]] static Common::ResultStr<ResidencyExecutor>
        BeginFromAlwaysLoaded( WorldPartitionPlan plan, WorldPartitionSerialized partition,
                               const ResidencySettings& settings, std::size_t recordCount,
                               std::span<const std::pair<std::size_t, std::size_t>> observations )
        {
            auto made = Make( std::move( plan ), std::move( partition ), settings, recordCount, observations );
            if ( !made.IsSuccess() )
                return made;
            ResidencyExecutor executor = made.ExtractValue();
            for ( std::size_t unit = 0; unit < executor.m_Plan.AlwaysLoaded.size(); ++unit )
                executor.MarkActivated( unit );
            for ( std::size_t record = 0; record < recordCount; ++record )
                if ( executor.m_UnitOfRecord[record] == kNoRecord )
                    return Common::MakeError<ResidencyExecutor>(
                         "ResidencyExecutor: record " + std::to_string( record ) + " of " +
                         std::to_string( recordCount ) + " belongs to no unit of the plan" );
            return Common::MakeSuccess( std::move( executor ) );
        }

        // ONE FRAME: step the residency with the loads the world finished since the last one, then perform its
        // actions in the order it gives them.
        [[nodiscard]] Common::ResultStr<ResidencyTick> Tick( std::span<const StreamingSource> sources,
                                                             double nowSeconds, ResidencyWorld& world )
        {
            const std::vector<LoadOutcome> finished = world.TakeLoadOutcomes();
            auto stepped = StepResidency( m_Plan, m_Partition, sources, m_Settings, std::move( m_State ), finished,
                                          nowSeconds );
            if ( !stepped.IsSuccess() )
                return Common::MakeError<ResidencyTick>( stepped.GetError() );
            ResidencyStep step = stepped.ExtractValue();
            m_State            = std::move( step.State );

            ResidencyTick            tick;
            std::vector<std::size_t> activated;
            std::vector<std::size_t> destroyed;
            tick.WaitingForBudget = step.WaitingForBudget;
            tick.StaleOutcomes    = step.StaleOutcomes;
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
                                 "ResidencyExecutor: activating unit " + std::to_string( action.Unit ) + " (" +
                                 std::to_string( records.size() ) + " record(s)) failed: " + made.GetError() );
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
                        break;
                }
            }
            for ( const std::size_t record : activated )
                for ( const std::size_t target : m_RefsFrom[record] )
                    tick.DeferredReferences += m_Live[target] ? 0 : 1;
            for ( const std::size_t record : destroyed )
                if ( !m_Live[record] )
                    for ( const std::size_t referrer : m_RefsTo[record] )
                        tick.UnboundReferences += m_Live[referrer] ? 1 : 0;
            return Common::MakeSuccess( tick );
        }

        [[nodiscard]] const ResidencyState& State() const
        {
            return m_State;
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
            for ( std::size_t unit = 0; unit < unitCount; ++unit )
            {
                executor.m_UnitRecords[unit] = ResidencyUnitMembers( executor.m_Plan, unit );
                for ( const std::size_t record : executor.m_UnitRecords[unit] )
                {
                    if ( record >= recordCount )
                        return Common::MakeError<ResidencyExecutor>(
                             "ResidencyExecutor: unit " + std::to_string( unit ) + " names record " +
                             std::to_string( record ) + " of " + std::to_string( recordCount ) +
                             "; the plan was not made from these records" );
                    executor.m_UnitOfRecord[record] = unit;
                }
            }
            executor.m_RefsFrom.resize( recordCount );
            executor.m_RefsTo.resize( recordCount );
            for ( const auto& [from, to] : observations )
            {
                if ( from >= recordCount || to >= recordCount )
                    return Common::MakeError<ResidencyExecutor>( "ResidencyExecutor: a reference names record " +
                                                                 std::to_string( std::max( from, to ) ) + " of " +
                                                                 std::to_string( recordCount ) );
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
    };

    // THE ONE DECISION AT THE START OF PLAY: does this world stream? Only a scene whose file states a
    // WorldPartition block does (SceneFormat.hpp: "a world is partitioned or it is not, and this is never an
    // engine mode"). For any other scene this returns nullopt WITHOUT TOUCHING @p world — the scene plays with
    // every entity it had in Edit, exactly as before streaming existed.
    [[nodiscard]] inline Common::ResultStr<std::optional<ResidencyExecutor>>
    BeginWorldStreaming( const SceneSerialized& scene, const AssetBoundsSource& bounds,
                         const ResidencySettings& settings, std::span<const StreamingSource> sources,
                         ResidencyWorld& world )
    {
        if ( !scene.WorldPartition.has_value() )
            return Common::MakeSuccess( std::optional<ResidencyExecutor>() );
        auto begun = ResidencyExecutor::Begin( PlanWorldPartition( scene.Entities, *scene.WorldPartition, bounds ),
                                               *scene.WorldPartition, scene.Entities, settings, sources, world );
        if ( !begun.IsSuccess() )
            return Common::MakeError<std::optional<ResidencyExecutor>>( "'" + scene.SceneName +
                                                                        "': " + begun.GetError() );
        return Common::MakeSuccess( std::optional<ResidencyExecutor>( begun.ExtractValue() ) );
    }
} // namespace Desert::Core::Rules
