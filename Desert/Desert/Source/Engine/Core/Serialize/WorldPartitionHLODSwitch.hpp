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
        [[nodiscard]] static Common::ResultStr<HLODRuntime> Make( const WorldPartitionPlan& plan,
                                                                  const HLODHierarchy&      hierarchy,
                                                                  const HLODResidency&      residency )
        {
            const std::size_t units     = ResidencyUnitCount( plan );
            const std::size_t hlodUnits = ResidencyUnitCount( residency.Plan );
            HLODRuntime       runtime;
            runtime.m_HLODOfUnit.assign( units, kNoRecord );
            runtime.m_HLODUnits = hlodUnits;
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
        [[nodiscard]] Common::ResultStr<std::size_t> Sync( const ResidencyState& state, const ResidencyState* hlodState,
                                                           HLODVisibilityWorld& world )
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

    // ONE FRAME OF A WORLD WHOSE HLODs STREAM (WP-FAR-8): the world's tick, the HLOD residency's tick, then the
    // switch on the states both left - so an HLOD cell that activates this frame has its HLODs shown or hidden
    // before the frame is drawn, and its parent hidden in the same frame.
    [[nodiscard]] inline Common::ResultStr<ResidencyTick>
    TickWithHLODs( ResidencyExecutor& executor, ResidencyExecutor& hlodExecutor, HLODRuntime& hlods,
                   std::span<const StreamingSource> sources, double nowSeconds, ResidencyWorld& world,
                   ResidencyWorld& hlodCells, HLODVisibilityWorld& hlodWorld )
    {
        auto tick = executor.Tick( sources, nowSeconds, world );
        if ( !tick.IsSuccess() )
            return tick;
        if ( auto hlodTick = hlodExecutor.Tick( sources, nowSeconds, hlodCells ); !hlodTick.IsSuccess() )
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
