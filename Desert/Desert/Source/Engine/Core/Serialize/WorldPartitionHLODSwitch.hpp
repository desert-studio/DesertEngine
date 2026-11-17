#pragma once
// Ported from UE 5.8 Engine/Source/Runtime/Engine/Private/WorldPartition/HLOD/HLODRuntimeSubsystem.cpp:623-660
// (UWorldPartitionHLODRuntimeSubsystem::OnCellShown / OnCellHidden over the per-cell FCellData), adapted: a
// cell's visibility is READ from the residency state after each step instead of pushed by a level-streaming
// callback, and an HLOD object is an index the world maps to its entities. The warmup is UE's too
// (UWorldPartitionHLODRuntimeSubsystem::CanMakeVisible / CanMakeInvisible / PrepareToWarmup / OnBeginRenderViews),
// gated by residency instead of a frame count: see THE WARMUP below.
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
// ── THE WARMUP (WP-HLOD-WARMUP) ─────────────────────────────────────────────────────────────────────
//
// A cell and its HLOD do NOT draw the same assets: a world cell draws its own meshes and their materials, the
// HLOD draws what the builder made (Instancing: the cell's meshes; MeshMerge / MeshSimplify / Approximate /
// Landscape: a built mesh with a baked material and its textures). Whichever side is about to be shown has
// been drawn by nobody: its meshes may not be built, and its textures sit at their streaming tail (the mips
// are streamed by what the frame draws, TextureService::UpdateStreaming) - so the first frames after a swap
// would show a hole or a soft picture. UE asks for the resources before the swap and holds the swap meanwhile
// (wp.Runtime.HLOD.WarmupEnabled, WarmupNumFrames frames). Here the same two questions are asked by the
// residency step at the moment of the change (ResidencyGate::MayActivate / MayRelease):
//  - A Loaded world cell about to activate: the world requests everything its read records name (WarmCell:
//    the records' dependency closure read on workers, meshes built, textures asked for at the screen size every
//    view measures the cell at) and the cell stays Loaded - its HLOD drawn - until that is resident.
//  - An Activated world cell about to be released: the world requests its HLOD's own assets the same way
//    (WarmHLOD) and the cell stays drawn until they are resident; then the HLOD is shown sharp.
// The gate is the FACT of residency, not a frame count: a warmup ends the step its assets are resident, and
// kWarmupMaxFrames only bounds it (a read that never lands, a pool too small to hold the asked mips), after
// which the swap goes ahead and the overrun is reported (TakeWarmupOverruns). A cell with no HLOD is never
// held: nothing stands in for it, so waiting would only leave its place empty longer.
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
#include <utility>
#include <vector>

namespace Desert::Core::Rules
{
    // What the switch needs from the place HLOD entities live: show or hide every entity of one HLOD.
    class HLODVisibilityWorld
    {
    public:
        virtual ~HLODVisibilityWorld()                                = default;
        virtual void SetHLODVisible( std::size_t hlod, bool visible ) = 0;

        // THE WARMUP PORT (see THE WARMUP). Each Warm call asks again, every step of the warmup, and answers
        // whether everything the side about to be shown draws is resident now. The defaults are a world whose
        // assets are resident the moment they are named (the suites' worlds): every warmup is done at once. The
        // engine's streamer overrides all four.
        // World cell @p unit (Loaded: its records are read) is about to be shown.
        [[nodiscard]] virtual bool WarmCell( std::size_t /*unit*/ )
        {
            return true;
        }
        // HLOD @p hlod is about to be shown in place of its cell.
        [[nodiscard]] virtual bool WarmHLOD( std::size_t /*hlod*/ )
        {
            return true;
        }
        // The warmup of that side ended (it was shown, or the change is no longer coming): its requests go.
        virtual void EndCellWarmup( std::size_t /*unit*/ )
        {
        }
        virtual void EndHLODWarmup( std::size_t /*hlod*/ )
        {
        }
    };

    // A warmup that reached kWarmupMaxFrames before its assets were resident: the swap went ahead anyway.
    struct WarmupOverrun
    {
        std::size_t Unit = 0;
        bool        Showing = false; // true: the cell was shown; false: its HLOD was
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
            runtime.m_Warmup.assign( units, {} );
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
            runtime.m_Warmup.assign( units, {} );
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

        // THE WARMUP'S BOUND, in residency steps. UE's wp.Runtime.HLOD.WarmupNumFrames (5) is the whole wait there:
        // UE waits that many frames and swaps. Here the wait ends when the assets are resident and this only
        // bounds it: one second at 60 Hz, longer than a cooked cell's reads, cooks and mip uploads take on a warm
        // machine, short enough that a pool which cannot hold the asked mips delays the swap by no more.
        static constexpr std::uint32_t kWarmupMaxFrames = 60;

        // Binds the warmup's two questions about the world's cells (see THE WARMUP) into @p gate, for the one
        // step @p world is asked from; the gate must not outlive this runtime or @p world.
        void AskWarmup( ResidencyGate& gate, HLODVisibilityWorld& world )
        {
            gate.MayActivate = [this, &world]( std::size_t unit ) { return WarmToShow( unit, world ); };
            gate.MayRelease  = [this, &world]( std::size_t unit ) { return WarmToHide( unit, world ); };
        }

        // After the step: ends the warmups it finished (UE OnCellShown / OnCellHidden drop the cell from
        // ObjectsToWarmup) - a cell warming to be shown that is no longer Loaded (shown, or gone), a cell warming
        // its HLOD that is no longer Activated (released) or that the step did not ask about (back inside its
        // band, so the swap is no longer coming).
        void EndWarmups( const ResidencyState& state, HLODVisibilityWorld& world )
        {
            for ( std::size_t unit = 0; unit < m_Warmup.size(); ++unit )
            {
                Warmup&         warm = m_Warmup[unit];
                const Residency now  = unit < state.Units.size() ? state.Units[unit].State : Residency::Unloaded;
                const bool      showEnds = warm.Phase == WarmupPhase::Showing && now != Residency::Loaded;
                const bool      hideEnds =
                     warm.Phase == WarmupPhase::Hiding && ( now != Residency::Activated || !warm.Asked );
                if ( showEnds )
                    world.EndCellWarmup( unit );
                if ( hideEnds )
                    world.EndHLODWarmup( m_HLODOfUnit[unit] );
                if ( showEnds || hideEnds )
                    warm = Warmup{};
                warm.Asked = false;
            }
        }

        // The warmups that ran out of steps since the last call, in the order they did.
        [[nodiscard]] std::vector<WarmupOverrun> TakeWarmupOverruns()
        {
            return std::exchange( m_Overruns, {} );
        }

        // The HLOD standing in for world unit @p unit, or kNoRecord.
        [[nodiscard]] std::size_t HLODOf( std::size_t unit ) const
        {
            return m_HLODOfUnit.at( unit );
        }

        // A streamed hierarchy only: the HLOD residency unit HLOD @p hlod lives in (its entities are that unit's).
        [[nodiscard]] std::size_t ResidesIn( std::size_t hlod ) const
        {
            return m_ResidesIn.at( hlod );
        }

        [[nodiscard]] bool Streamed() const
        {
            return !m_ResidesIn.empty();
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

        enum class WarmupPhase : std::uint8_t
        {
            Idle,
            Showing, // the cell is warming to be shown (MayActivate)
            Hiding,  // its HLOD is warming to be shown in its place (MayRelease)
        };
        struct Warmup
        {
            WarmupPhase   Phase  = WarmupPhase::Idle;
            std::uint32_t Frames = 0;     // steps asked so far
            bool          Ready  = false; // resident, or out of steps: the answer stays yes until it ends
            bool          Asked  = false; // asked in the current step
        };

        // One question of the step about one world unit; @p showing picks the side. The answer is yes for a unit
        // no HLOD stands in for, and from the step its assets are resident or its steps ran out.
        bool Warm( std::size_t unit, HLODVisibilityWorld& world, bool showing )
        {
            if ( unit >= m_HLODOfUnit.size() || m_HLODOfUnit[unit] == kNoRecord )
                return true;
            Warmup& warm = m_Warmup[unit];
            warm.Phase   = showing ? WarmupPhase::Showing : WarmupPhase::Hiding;
            warm.Asked   = true;
            if ( warm.Ready )
                return true;
            ++warm.Frames;
            const bool resident = showing ? world.WarmCell( unit ) : world.WarmHLOD( m_HLODOfUnit[unit] );
            if ( !resident && warm.Frames < kWarmupMaxFrames )
                return false;
            if ( !resident )
                m_Overruns.push_back( { unit, showing } );
            warm.Ready = true;
            return true;
        }
        bool WarmToShow( std::size_t unit, HLODVisibilityWorld& world )
        {
            return Warm( unit, world, true );
        }
        bool WarmToHide( std::size_t unit, HLODVisibilityWorld& world )
        {
            return Warm( unit, world, false );
        }

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
        // THE WARMUP: per world unit, the swap it is warming for; the overruns not yet taken.
        std::vector<Warmup>        m_Warmup;
        std::vector<WarmupOverrun> m_Overruns;
    };

    // ONE FRAME OF A PARTITIONED WORLD WITH HLODs: the residency tick (its swaps asked of the warmup), then the
    // switch on the state it left — in that order and in the same frame, which is the whole of "no hole, no
    // double" (see the top of the file).
    [[nodiscard]] inline Common::ResultStr<ResidencyTick>
    TickWithHLODs( ResidencyExecutor& executor, HLODRuntime& hlods, std::span<const StreamingSource> sources,
                   double nowSeconds, ResidencyWorld& world, HLODVisibilityWorld& hlodWorld )
    {
        ResidencyGate gate;
        hlods.AskWarmup( gate, hlodWorld );
        auto tick = executor.Tick( sources, nowSeconds, world, &gate );
        if ( !tick.IsSuccess() )
            return tick;
        hlods.EndWarmups( executor.State(), hlodWorld );
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
        ResidencyGate gate = worldGate.ExtractValue();
        hlods.AskWarmup( gate, hlodWorld );
        auto tick = executor.Tick( sources, nowSeconds, world, &gate );
        if ( !tick.IsSuccess() )
            return tick;
        hlods.EndWarmups( executor.State(), hlodWorld );
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
