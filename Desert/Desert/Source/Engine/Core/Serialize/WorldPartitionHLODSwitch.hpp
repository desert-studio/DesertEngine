#pragma once
// Ported from UE 5.8 Engine/Source/Runtime/Engine/Private/WorldPartition/HLOD/HLODRuntimeSubsystem.cpp:623-660
// (UWorldPartitionHLODRuntimeSubsystem::OnCellShown / OnCellHidden over the per-cell FCellData), adapted: a
// cell's visibility is READ from the residency state after each step instead of pushed by a level-streaming
// callback, an HLOD object is an index the world maps to its entities, and there is no warmup (the HLOD draws
// the very mesh assets its cell draws, so neither side waits for the other's resources).
//
// WHICH ONE IS DRAWN, A CELL OR ITS HLOD (WP11).
//
// The cook writes, beside every cell that draws something, an Instancing HLOD (WorldPartitionHLODRules.hpp):
// the cell's meshes as InstancedStaticMesh records. In Play those records are ordinary entities for the whole
// session, drawn by the ordinary instancing path; what changes is only their Visibility. UE's rule, verbatim:
// an HLOD is visible exactly while its cell is NOT visible. A cell is visible when its records are entities —
// Residency::Activated. Unloaded, Loading, Loaded (read but not yet made) and Failed all draw the HLOD.
//
// ── THE SAME FRAME, BOTH WAYS ───────────────────────────────────────────────────────────────────────
//
// Activation and destruction happen inside ResidencyExecutor::Tick, on the main thread, before the frame is
// drawn. The switch runs right after that Tick on the state it left (TickWithHLODs), so in the frame a cell's
// records become entities its HLOD is already hidden, and in the frame they are destroyed it is already shown:
// no frame with a hole, no frame with both. Doing it one frame late in either direction is exactly the defect
// the suite (WorldPartitionStreamer, HLOD switch) is there to catch.
//
// ── THE SWAP, BY RESIDENCY (WP-FAR-8, a streamed hierarchy) ─────────────────────────────────────────
//
// UE's rule (a parent HLOD is hidden exactly while its child cell is visible) leaves a band: a world cell that
// activates while the HLOD cell above it is still loading is drawn under the parent standing in for that HLOD
// cell - the same buildings twice, z-fighting. Here the order is enforced instead (ResidencyGate): a world cell
// waits, Loaded, until every HLOD cell above it is Activated (WorldGate), an HLOD cell waits for the HLOD cell
// its stand-in lives in, and an HLOD cell is not released while a world cell beneath it is Activated or a child
// HLOD cell is not Unloaded (HLODGate). So a parent hides only once its children are entities and shows again
// in the frame its last child goes: at any distance exactly one of {cell, its HLOD, a parent} is drawn. Waiting
// costs detail, never a hole: the held cell is drawn by the parent meanwhile.
//
// ── READ FROM THE STATE, NOT FROM EVENTS ────────────────────────────────────────────────────────────
//
// UE flips on OnCellShown/OnCellHidden. Here the executor's actions are the only place a cell's records come or
// go, and the state after them is the whole truth; comparing it with what was last applied gives the same two
// events with no path by which one could be missed (a Begin that activates without an action, a failed load
// that never activated). The comparison is per cell that has an HLOD, a byte each, once a frame.
//
// ── THE HOLES (the policy for WP10's NotInstanced) ──────────────────────────────────────────────────
//
// A record the builder could not batch (EditorMesh, HiddenSubmeshes, SkinnedMesh, Unreadable, ...)
// has no stand-in: while its cell is not visible it is simply absent from the distant picture. That is UE's
// policy too — its builder logs such a component and moves on — and nothing better exists without a new kind of
// proxy. What this adds is that the hole is never silent: DescribeHLODHoles names them by reason, and the
// streamer says it once when streaming begins. The rest of the cell's HLOD is drawn as usual.

#include <Engine/Core/Serialize/WorldPartitionHLODHierarchy.hpp>
#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>
#include <Engine/Core/TextTemplate.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace Desert::Core::Rules
{
    // What the switch needs from the place HLOD entities live: show or hide every entity of one HLOD.
    class HLODVisibilityWorld
    {
    public:
        virtual ~HLODVisibilityWorld()                                = default;
        virtual void SetHLODVisible( std::size_t hlod, bool visible ) = 0;
    };

    // UE's bIsCellVisible: the cell's own records are drawn.
    [[nodiscard]] constexpr bool IsCellVisible( Residency state )
    {
        return state == Residency::Activated;
    }

    class HLODRuntime
    {
    public:
        // @p unitOfHLOD: per HLOD, the residency unit (a CELL of @p plan) it stands in for; at most one HLOD per
        // cell, as the cook writes them. Refused: an always-loaded unit (it is never hidden, so its HLOD would
        // double it forever), a unit the plan does not have, a cell named twice.
        [[nodiscard]] static Common::ResultStr<HLODRuntime> Make( const WorldPartitionPlan&    plan,
                                                                  std::span<const std::size_t> unitOfHLOD )
        {
            const std::size_t units = ResidencyUnitCount( plan );
            HLODRuntime       runtime;
            runtime.m_HLODOfUnit.assign( units, kNoRecord );
            runtime.m_UnitOfHLOD.assign( unitOfHLOD.begin(), unitOfHLOD.end() );
            runtime.m_Applied.assign( unitOfHLOD.size(), kNotApplied );
            for ( std::size_t hlod = 0; hlod < unitOfHLOD.size(); ++hlod )
            {
                const std::size_t unit = unitOfHLOD[hlod];
                if ( unit >= units )
                    return Common::MakeError<HLODRuntime>(
                         std::format( "HLOD {} stands in for unit {} of {}", hlod, unit, units ) );
                if ( unit < plan.AlwaysLoaded.size() )
                    return Common::MakeError<HLODRuntime>(
                         std::format( "HLOD {} stands in for always-loaded unit {}, which is never hidden", hlod,
                                      DescribeResidencyUnit( plan, unit ) ) );
                if ( runtime.m_HLODOfUnit[unit] != kNoRecord )
                    return Common::MakeError<HLODRuntime>( std::format( "cell {} has two HLODs ({} and {})",
                                                                        DescribeResidencyUnit( plan, unit ),
                                                                        runtime.m_HLODOfUnit[unit], hlod ) );
                runtime.m_HLODOfUnit[unit] = hlod;
            }
            return Common::MakeSuccess( std::move( runtime ) );
        }

        // THE HIERARCHY STREAMED (WP-FAR-8, WorldPartitionHLODHierarchy.hpp): one HLOD per node of @p hierarchy,
        // in node order, over the world's @p plan and the HLOD residency @p residency made from that hierarchy. A
        // node standing in for an HLOD cell reads that cell's state in the HLOD residency; a node living in an
        // HLOD cell has entities only while that cell is Activated.
        [[nodiscard]] static Common::ResultStr<HLODRuntime>
        Make( const WorldPartitionPlan& plan, const HLODHierarchy& hierarchy, const HLODResidency& residency )
        {
            const std::size_t units     = ResidencyUnitCount( plan );
            const std::size_t hlodUnits = ResidencyUnitCount( residency.Plan );
            HLODRuntime       runtime;
            runtime.m_HLODOfUnit.assign( units, kNoRecord );
            runtime.m_HLODUnits = hlodUnits;
            runtime.m_WorldWaitsFor.assign( units, {} );
            runtime.m_CellWaitsFor.assign( hlodUnits, {} );
            runtime.m_CellHeldByWorld.assign( hlodUnits, {} );
            runtime.m_CellHeldByCells.assign( hlodUnits, {} );
            runtime.m_Applied.assign( hierarchy.Nodes.size(), kNotApplied );
            for ( std::size_t hlod = 0; hlod < hierarchy.Nodes.size(); ++hlod )
            {
                const HLODNode& node = hierarchy.Nodes[hlod];
                if ( node.ForHLODCell )
                {
                    const std::size_t unit = residency.Plan.AlwaysLoaded.size() + node.StandsFor;
                    if ( node.StandsFor >= hierarchy.Cells.size() || unit >= hlodUnits )
                        return Common::MakeError<HLODRuntime>(
                             std::format( "HLOD '{}' stands in for HLOD cell {} of {}", node.Name, node.StandsFor,
                                          hierarchy.Cells.size() ) );
                    runtime.m_UnitOfHLOD.push_back( unit );
                    // THE SWAP'S ORDER (see THE SWAP, BY RESIDENCY): every world cell this parent is built from
                    // waits for the HLOD cell it stands in for, and that cell is held while any of them is drawn.
                    for ( const std::size_t source : node.SourceUnits )
                    {
                        if ( source >= units )
                            return Common::MakeError<HLODRuntime>( std::format(
                                 "HLOD '{}' is built from unit {} of {}", node.Name, source, units ) );
                        runtime.m_WorldWaitsFor[source].push_back( unit );
                        runtime.m_CellHeldByWorld[unit].push_back( source );
                    }
                }
                else
                {
                    if ( node.StandsFor >= units || node.StandsFor < plan.AlwaysLoaded.size() )
                        return Common::MakeError<HLODRuntime>(
                             std::format( "HLOD '{}' stands in for unit {} of {}, which is no cell", node.Name,
                                          node.StandsFor, units ) );
                    if ( runtime.m_HLODOfUnit[node.StandsFor] != kNoRecord )
                        return Common::MakeError<HLODRuntime>( "cell " + node.Name + " has two HLODs" );
                    runtime.m_HLODOfUnit[node.StandsFor] = hlod;
                    runtime.m_UnitOfHLOD.push_back( node.StandsFor );
                }
                runtime.m_ForHLODUnit.push_back( node.ForHLODCell ? 1 : 0 );
                const std::size_t residesIn = residency.UnitOfNode.at( hlod );
                if ( residesIn >= hlodUnits )
                    return Common::MakeError<HLODRuntime>(
                         std::format( "HLOD '{}' lives in HLOD unit {} of {}", node.Name, residesIn, hlodUnits ) );
                runtime.m_ResidesIn.push_back( residesIn );
                // A parent living in an HLOD cell: the cell it stands in for waits for that one, and holds it.
                if ( node.ForHLODCell && residesIn >= residency.Plan.AlwaysLoaded.size() )
                {
                    const std::size_t child = runtime.m_UnitOfHLOD.back();
                    runtime.m_CellWaitsFor[child].push_back( residesIn );
                    runtime.m_CellHeldByCells[residesIn].push_back( child );
                }
            }
            return Common::MakeSuccess( std::move( runtime ) );
        }

        // Brings every HLOD's visibility to !IsCellVisible( its cell ) in @p world, telling it only what changed
        // since the last call (the first call tells it everything). Returns how many HLODs were switched.
        [[nodiscard]] Common::ResultStr<std::size_t> Sync( const ResidencyState& state,
                                                           HLODVisibilityWorld&  world )
        {
            return Sync( state, nullptr, world );
        }

        // The same over a streamed hierarchy: @p hlodState is the HLOD residency's (null for a runtime made from
        // a unit list, whose HLODs are resident the whole session). An HLOD whose own cell is not Activated has
        // no entities: nothing is told, and it is told again the frame its cell activates.
        [[nodiscard]] Common::ResultStr<std::size_t>
        Sync( const ResidencyState& state, const ResidencyState* hlodState, HLODVisibilityWorld& world )
        {
            if ( state.Units.size() != m_HLODOfUnit.size() )
                return Common::MakeError<std::size_t>(
                     std::format( "the HLOD switch was made for {} unit(s), the residency has {}",
                                  m_HLODOfUnit.size(), state.Units.size() ) );
            const bool streamed = !m_ResidesIn.empty();
            if ( streamed && ( hlodState == nullptr || hlodState->Units.size() != m_HLODUnits ) )
                return Common::MakeError<std::size_t>(
                     std::format( "the HLOD switch was made for {} HLOD unit(s), the HLOD residency has {}",
                                  m_HLODUnits, hlodState == nullptr ? 0 : hlodState->Units.size() ) );
            std::size_t switched = 0;
            for ( std::size_t hlod = 0; hlod < m_UnitOfHLOD.size(); ++hlod )
            {
                if ( streamed && !IsCellVisible( hlodState->Units[m_ResidesIn[hlod]].State ) )
                {
                    m_Applied[hlod] = kNotApplied;
                    continue;
                }
                const ResidencyState& of      = streamed && m_ForHLODUnit[hlod] == 1 ? *hlodState : state;
                const std::uint8_t    visible = IsCellVisible( of.Units[m_UnitOfHLOD[hlod]].State ) ? 0 : 1;
                if ( m_Applied[hlod] == visible )
                    continue;
                world.SetHLODVisible( hlod, visible == 1 );
                m_Applied[hlod] = visible;
                ++switched;
            }
            return Common::MakeSuccess( switched );
        }

        // THE WORLD'S GATE (streamed hierarchy only; empty otherwise): a world cell may not activate while an
        // HLOD cell standing above it is not Activated, because the parent standing in for that HLOD cell is
        // drawn and would be drawn over the cell.
        [[nodiscard]] Common::ResultStr<ResidencyGate> WorldGate( const ResidencyState& hlodState ) const
        {
            ResidencyGate gate;
            if ( m_ResidesIn.empty() )
                return Common::MakeSuccess( std::move( gate ) );
            if ( hlodState.Units.size() != m_HLODUnits )
                return Common::MakeError<ResidencyGate>(
                     std::format( "the HLOD gate was made for {} HLOD unit(s), the HLOD residency has {}",
                                  m_HLODUnits, hlodState.Units.size() ) );
            gate.HoldActivation.assign( m_WorldWaitsFor.size(), 0 );
            for ( std::size_t unit = 0; unit < m_WorldWaitsFor.size(); ++unit )
                for ( const std::size_t cell : m_WorldWaitsFor[unit] )
                    if ( !IsCellVisible( hlodState.Units[cell].State ) )
                        gate.HoldActivation[unit] = 1;
            return Common::MakeSuccess( std::move( gate ) );
        }

        // THE HLOD CELLS' GATE, on the world's state AFTER its tick this frame: an HLOD cell may not activate
        // while the HLOD cell its own stand-in lives in is not Activated (that stand-in's parent is drawn over
        // it), and may not be released while a world cell it stands above is Activated, or while a child HLOD
        // cell whose stand-in lives in it is anything but Unloaded (it could activate in this very step).
        [[nodiscard]] Common::ResultStr<ResidencyGate> HLODGate( const ResidencyState& state,
                                                                 const ResidencyState& hlodState ) const
        {
            ResidencyGate gate;
            if ( m_ResidesIn.empty() )
                return Common::MakeSuccess( std::move( gate ) );
            if ( state.Units.size() != m_WorldWaitsFor.size() || hlodState.Units.size() != m_HLODUnits )
                return Common::MakeError<ResidencyGate>( std::format(
                     "the HLOD gate was made for {} unit(s) and {} HLOD unit(s), the residencies have {} and {}",
                     m_WorldWaitsFor.size(), m_HLODUnits, state.Units.size(), hlodState.Units.size() ) );
            gate.HoldActivation.assign( m_HLODUnits, 0 );
            gate.HoldRelease.assign( m_HLODUnits, 0 );
            for ( std::size_t cell = 0; cell < m_HLODUnits; ++cell )
            {
                for ( const std::size_t above : m_CellWaitsFor[cell] )
                    if ( !IsCellVisible( hlodState.Units[above].State ) )
                        gate.HoldActivation[cell] = 1;
                for ( const std::size_t unit : m_CellHeldByWorld[cell] )
                    if ( IsCellVisible( state.Units[unit].State ) )
                        gate.HoldRelease[cell] = 1;
                for ( const std::size_t child : m_CellHeldByCells[cell] )
                    if ( hlodState.Units[child].State != Residency::Unloaded )
                        gate.HoldRelease[cell] = 1;
            }
            return Common::MakeSuccess( std::move( gate ) );
        }

        // The HLOD cells that must be Activated before the first frame when @p state already has cells Activated
        // (Edit -> Play keeps the neighbourhood): every HLOD cell some Activated world cell waits for, and every
        // HLOD cell those wait for, in HLOD unit order. Empty for a runtime made from a unit list.
        [[nodiscard]] std::vector<std::size_t> CellsWantedAtBegin( const ResidencyState& state ) const
        {
            std::vector<std::uint8_t> wanted( m_HLODUnits, 0 );
            std::vector<std::size_t>  open;
            for ( std::size_t unit = 0; unit < m_WorldWaitsFor.size() && unit < state.Units.size(); ++unit )
                if ( IsCellVisible( state.Units[unit].State ) )
                    open.insert( open.end(), m_WorldWaitsFor[unit].begin(), m_WorldWaitsFor[unit].end() );
            while ( !open.empty() )
            {
                const std::size_t cell = open.back();
                open.pop_back();
                if ( wanted[cell] != 0 )
                    continue;
                wanted[cell] = 1;
                open.insert( open.end(), m_CellWaitsFor[cell].begin(), m_CellWaitsFor[cell].end() );
            }
            std::vector<std::size_t> cells;
            for ( std::size_t cell = 0; cell < m_HLODUnits; ++cell )
                if ( wanted[cell] != 0 )
                    cells.push_back( cell );
            return cells;
        }

        // What the last Sync applied; false before the first.
        [[nodiscard]] bool IsVisible( std::size_t hlod ) const
        {
            return m_Applied.at( hlod ) == 1;
        }

        [[nodiscard]] std::size_t Count() const
        {
            return m_UnitOfHLOD.size();
        }

        [[nodiscard]] std::size_t UnitOf( std::size_t hlod ) const
        {
            return m_UnitOfHLOD.at( hlod );
        }

        // Whether UnitOf( @p hlod ) is a unit of the HLOD residency (a parent) rather than of the world's plan.
        [[nodiscard]] bool StandsForHLODUnit( std::size_t hlod ) const
        {
            return !m_ForHLODUnit.empty() && m_ForHLODUnit.at( hlod ) == 1;
        }

    private:
        friend class Common::ResultStr<HLODRuntime>;
        HLODRuntime() = default;

        static constexpr std::uint8_t kNotApplied = 2;

        std::vector<std::size_t>  m_HLODOfUnit; // per unit: its HLOD, or kNoRecord
        std::vector<std::size_t>  m_UnitOfHLOD; // per HLOD
        std::vector<std::uint8_t> m_Applied;    // per HLOD: 0 hidden, 1 visible, kNotApplied before the first Sync
        // A streamed hierarchy only (empty otherwise): per HLOD, 1 when UnitOf is an HLOD residency unit, and the
        // HLOD residency unit it lives in; and how many units that residency has.
        std::vector<std::uint8_t> m_ForHLODUnit;
        std::vector<std::size_t>  m_ResidesIn;
        std::size_t               m_HLODUnits = 0;
        // The swap's order (WorldGate / HLODGate): per world unit, the HLOD units it waits for; per HLOD unit,
        // the HLOD units it waits for, and the world units and HLOD units that hold it.
        std::vector<std::vector<std::size_t>> m_WorldWaitsFor;
        std::vector<std::vector<std::size_t>> m_CellWaitsFor;
        std::vector<std::vector<std::size_t>> m_CellHeldByWorld;
        std::vector<std::vector<std::size_t>> m_CellHeldByCells;
    };

    // ONE FRAME OF A PARTITIONED WORLD WITH HLODs: the residency tick, then the switch on the state it left —
    // in that order and in the same frame, which is the whole of "no hole, no double" (see the top of the file).
    [[nodiscard]] inline Common::ResultStr<ResidencyTick>
    TickWithHLODs( ResidencyExecutor& executor, HLODRuntime& hlods, std::span<const StreamingSource> sources,
                   double nowSeconds, ResidencyWorld& world, HLODVisibilityWorld& hlodWorld )
    {
        auto tick = executor.Tick( sources, nowSeconds, world );
        if ( !tick.IsSuccess() )
            return tick;
        if ( auto synced = hlods.Sync( executor.State(), hlodWorld ); !synced.IsSuccess() )
            return Common::MakeError<ResidencyTick>( synced.GetError() );
        return tick;
    }

    // ONE FRAME OF A WORLD WHOSE HLODs STREAM (WP-FAR-8): the world's tick under the gate of the HLOD cells as
    // the last frame left them, the HLOD residency's tick under the gate of the world as this tick left it, then
    // the switch on the states both left - so an HLOD cell that activates this frame has its HLODs shown or
    // hidden before the frame is drawn, and its parent hidden in the same frame (THE SWAP, BY RESIDENCY).
    // The HLOD cells stream for the sources that stream the world grids beneath them (HLODSources, @p residency
    // the plan @p hlodExecutor runs).
    [[nodiscard]] inline Common::ResultStr<ResidencyTick>
    TickWithHLODs( ResidencyExecutor& executor, ResidencyExecutor& hlodExecutor, const HLODResidency& residency,
                   HLODRuntime& hlods, std::span<const StreamingSource> sources, double nowSeconds,
                   ResidencyWorld& world, ResidencyWorld& hlodCells, HLODVisibilityWorld& hlodWorld )
    {
        auto worldGate = hlods.WorldGate( hlodExecutor.State() );
        if ( !worldGate.IsSuccess() )
            return Common::MakeError<ResidencyTick>( worldGate.GetError() );
        auto tick = executor.Tick( sources, nowSeconds, world, &worldGate.GetValue() );
        if ( !tick.IsSuccess() )
            return tick;
        auto hlodGate = hlods.HLODGate( executor.State(), hlodExecutor.State() );
        if ( !hlodGate.IsSuccess() )
            return Common::MakeError<ResidencyTick>( hlodGate.GetError() );
        const std::vector<StreamingSource> hlodSources = HLODSources( residency, sources );
        if ( auto hlodTick = hlodExecutor.Tick( hlodSources, nowSeconds, hlodCells, &hlodGate.GetValue() );
             !hlodTick.IsSuccess() )
            return Common::MakeError<ResidencyTick>( "the HLOD cells: " + hlodTick.GetError() );
        if ( auto synced = hlods.Sync( executor.State(), &hlodExecutor.State(), hlodWorld ); !synced.IsSuccess() )
            return Common::MakeError<ResidencyTick>( synced.GetError() );
        return tick;
    }

    // The holes, counted by reason in the enum's own spelling and in its alphabetical order:
    // "1 EditorMesh, 2 SkinnedMesh"; empty when there are none.
    [[nodiscard]] inline std::string DescribeHLODHoles( std::span<const HLODExclusion> reasons )
    {
        std::map<std::string, std::size_t> counts;
        for ( const HLODExclusion reason : reasons )
            ++counts[std::string( rfl::enum_to_string( reason ) )];
        static const TextTemplate kHoles(
             "HLODHoles",
             "{% for h in holes %}{% if not loop.first %}, {% endif %}{{ h.count }} {{ h.name }}{% endfor %}" );
        Common::Json::Value::Array holes;
        for ( const auto& [name, count] : counts )
            holes.emplace_back( Common::Json::ObjectBuilder().Set( "name", name ).Set( "count", count ).Build() );
        return kHoles.RenderOrLog( Common::Json::Value(
             Common::Json::ObjectBuilder().Set( "holes", Common::Json::Value( std::move( holes ) ) ).Build() ) );
    }
} // namespace Desert::Core::Rules
