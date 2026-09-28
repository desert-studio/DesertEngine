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
// A record the builder could not batch (EditorMesh, HiddenSubmeshes, SkinnedMesh, PrefabInstance, Unreadable)
// has no stand-in: while its cell is not visible it is simply absent from the distant picture. That is UE's
// policy too — its builder logs such a component and moves on — and nothing better exists without a new kind of
// proxy. What this adds is that the hole is never silent: DescribeHLODHoles names them by reason, and the
// streamer says it once when streaming begins. The rest of the cell's HLOD is drawn as usual.

#include <Engine/Core/Serialize/WorldPartitionHLODRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyExecutor.hpp>

#include <Common/Core/ResultStr.hpp>

#include <cstddef>
#include <cstdint>
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
                    return Common::MakeError<HLODRuntime>( "HLOD " + std::to_string( hlod ) +
                                                           " stands in for unit " + std::to_string( unit ) +
                                                           " of " + std::to_string( units ) );
                if ( unit < plan.AlwaysLoaded.size() )
                    return Common::MakeError<HLODRuntime>(
                         "HLOD " + std::to_string( hlod ) + " stands in for always-loaded unit " +
                         DescribeResidencyUnit( plan, unit ) + ", which is never hidden" );
                if ( runtime.m_HLODOfUnit[unit] != kNoRecord )
                    return Common::MakeError<HLODRuntime>(
                         "cell " + DescribeResidencyUnit( plan, unit ) + " has two HLODs (" +
                         std::to_string( runtime.m_HLODOfUnit[unit] ) + " and " + std::to_string( hlod ) + ")" );
                runtime.m_HLODOfUnit[unit] = hlod;
            }
            return Common::MakeSuccess( std::move( runtime ) );
        }

        // Brings every HLOD's visibility to !IsCellVisible( its cell ) in @p world, telling it only what changed
        // since the last call (the first call tells it everything). Returns how many HLODs were switched.
        [[nodiscard]] Common::ResultStr<std::size_t> Sync( const ResidencyState& state,
                                                           HLODVisibilityWorld&  world )
        {
            if ( state.Units.size() != m_HLODOfUnit.size() )
                return Common::MakeError<std::size_t>(
                     "the HLOD switch was made for " + std::to_string( m_HLODOfUnit.size() ) +
                     " unit(s), the residency has " + std::to_string( state.Units.size() ) );
            std::size_t switched = 0;
            for ( std::size_t hlod = 0; hlod < m_UnitOfHLOD.size(); ++hlod )
            {
                const std::uint8_t visible = IsCellVisible( state.Units[m_UnitOfHLOD[hlod]].State ) ? 0 : 1;
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

    private:
        friend class Common::ResultStr<HLODRuntime>;
        HLODRuntime() = default;

        static constexpr std::uint8_t kNotApplied = 2;

        std::vector<std::size_t>  m_HLODOfUnit; // per unit: its HLOD, or kNoRecord
        std::vector<std::size_t>  m_UnitOfHLOD; // per HLOD
        std::vector<std::uint8_t> m_Applied;    // per HLOD: 0 hidden, 1 visible, kNotApplied before the first Sync
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

    // The holes, counted by reason in the enum's own spelling and in its alphabetical order:
    // "1 PrefabInstance, 2 SkinnedMesh"; empty when there are none.
    [[nodiscard]] inline std::string DescribeHLODHoles( std::span<const HLODExclusion> reasons )
    {
        std::map<std::string, std::size_t> counts;
        for ( const HLODExclusion reason : reasons )
            ++counts[std::string( rfl::enum_to_string( reason ) )];
        std::string text;
        for ( const auto& [name, count] : counts )
            text += ( text.empty() ? "" : ", " ) + std::to_string( count ) + " " + name;
        return text;
    }
} // namespace Desert::Core::Rules
