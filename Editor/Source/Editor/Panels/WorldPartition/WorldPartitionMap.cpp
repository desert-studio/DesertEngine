// Ported from UE 5.8
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/SWorldPartitionEditorGrid2D.cpp and
// Engine/Source/Runtime/Engine/Private/LevelStreaming.cpp; the lines and what was adapted are in the header.

#include "WorldPartitionMap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Desert::Editor::WorldPartitionMap
{
    namespace
    {
        bool Overlaps( const ::Desert::Core::Rules::CellBounds& a, glm::dvec2 lo, glm::dvec2 hi )
        {
            return a.MinX < hi.x && a.MaxX > lo.x && a.MinZ < hi.y && a.MaxZ > lo.y;
        }

        bool Shown( int cellLevel, int level )
        {
            return level < 0 || cellLevel == level;
        }

        bool OnGrid( const ::Desert::Core::Rules::PlannedCell& cell, std::optional<std::size_t> grid )
        {
            return !grid.has_value() || cell.Grid == *grid;
        }
    } // namespace

    glm::dvec2 WorldToScreen( const View& view, glm::dvec2 screenSize, glm::dvec2 world )
    {
        return ( world + view.Trans ) * view.Scale + screenSize * 0.5;
    }

    glm::dvec2 ScreenToWorld( const View& view, glm::dvec2 screenSize, glm::dvec2 screen )
    {
        return ( screen - screenSize * 0.5 ) / view.Scale - view.Trans;
    }

    void Zoom( View& view, glm::dvec2 screenSize, glm::dvec2 mouse, float wheelDelta )
    {
        // UE OnMouseWheel: the cursor's offset from the centre, in world units, before and after the scale
        // changes; the difference goes into Trans, which is what pins the world point under the cursor.
        const glm::dvec2 local  = mouse - screenSize * 0.5;
        const glm::dvec2 before = local / view.Scale;
        const double     delta  = 1.0 + std::abs( static_cast<double>( wheelDelta ) / 8.0 );
        view.Scale = std::clamp( view.Scale * ( wheelDelta > 0.0f ? delta : 1.0 / delta ), kMinScale, kMaxScale );
        view.Trans += local / view.Scale - before;
    }

    void Pan( View& view, glm::dvec2 screenDelta )
    {
        view.Trans += screenDelta / view.Scale;
    }

    void Focus( View& view, glm::dvec2 screenSize, const ::Desert::Core::Rules::CellBounds& box )
    {
        const glm::dvec2 lo( box.MinX, box.MinZ );
        const glm::dvec2 hi( box.MaxX, box.MaxZ );
        view.Trans = -( lo + hi ) * 0.5;
        // UE takes the smaller of the two axis ratios; an axis the box has no extent on cannot constrain it.
        const glm::dvec2 half = ( hi - lo ) * 0.5;
        double           fit  = std::numeric_limits<double>::infinity();
        if ( half.x > 0.0 )
            fit = std::min( fit, screenSize.x * 0.5 / half.x );
        if ( half.y > 0.0 )
            fit = std::min( fit, screenSize.y * 0.5 / half.y );
        if ( std::isfinite( fit ) )
            view.Scale = std::clamp( fit * kFocusFill, kMinScale, kMaxScale );
    }

    View Follow( glm::dvec2 screenSize, glm::dvec2 sourceXZ )
    {
        View view;
        view.Trans = -sourceXZ;
        view.Scale = std::min( screenSize.x, screenSize.y ) * 0.5 / kFollowExtentCm;
        return view;
    }

    std::optional<::Desert::Core::Rules::CellBounds>
    PlanBounds( const ::Desert::Core::Rules::WorldPartitionPlan& plan, std::optional<std::size_t> grid )
    {
        std::optional<::Desert::Core::Rules::CellBounds> found;
        for ( const ::Desert::Core::Rules::PlannedCell& cell : plan.Cells )
        {
            if ( !OnGrid( cell, grid ) )
                continue;
            if ( !found.has_value() )
            {
                found = cell.Square;
                continue;
            }
            ::Desert::Core::Rules::CellBounds& bounds = *found;
            bounds.MinX = std::min( bounds.MinX, cell.Square.MinX );
            bounds.MinZ = std::min( bounds.MinZ, cell.Square.MinZ );
            bounds.MaxX = std::max( bounds.MaxX, cell.Square.MaxX );
            bounds.MaxZ = std::max( bounds.MaxZ, cell.Square.MaxZ );
        }
        return found;
    }

    std::vector<std::size_t> VisibleCells( const ::Desert::Core::Rules::WorldPartitionPlan& plan, const View& view,
                                           glm::dvec2 screenSize, int level, std::optional<std::size_t> grid )
    {
        const glm::dvec2         lo = ScreenToWorld( view, screenSize, { 0.0, 0.0 } );
        const glm::dvec2         hi = ScreenToWorld( view, screenSize, screenSize );
        std::vector<std::size_t> visible;
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
            if ( OnGrid( plan.Cells[cell], grid ) && Shown( plan.Cells[cell].Level, level ) &&
                 Overlaps( plan.Cells[cell].Square, lo, hi ) )
                visible.push_back( cell );
        std::stable_sort( visible.begin(), visible.end(), [&plan]( std::size_t a, std::size_t b )
                          { return plan.Cells[a].Level > plan.Cells[b].Level; } );
        return visible;
    }

    std::optional<std::size_t> CellAt( const ::Desert::Core::Rules::WorldPartitionPlan& plan, glm::dvec2 world,
                                       int level, std::optional<std::size_t> grid )
    {
        std::optional<std::size_t> found;
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            const ::Desert::Core::Rules::PlannedCell& candidate = plan.Cells[cell];
            const bool inside = world.x >= candidate.Square.MinX && world.x < candidate.Square.MaxX &&
                                world.y >= candidate.Square.MinZ && world.y < candidate.Square.MaxZ;
            if ( inside && OnGrid( candidate, grid ) && Shown( candidate.Level, level ) &&
                 ( !found.has_value() || candidate.Level < plan.Cells[*found].Level ) )
                found = cell;
        }
        return found;
    }

    int GridLineLevel( const View& view, float cellSize, double minPixels )
    {
        // 62: the last level whose 2^L is exact in a double; a view that needs more is a view of nothing.
        for ( int level = 0; level < 62; ++level )
            if ( ::Desert::Core::Rules::LevelCellSize( cellSize, level ) * view.Scale >= minPixels )
                return level;
        return 62;
    }

    std::string LevelLabel( float cellSize, int level )
    {
        const double metres = ::Desert::Core::Rules::LevelCellSize( cellSize, level ) / 100.0;
        char         text[64];
        if ( std::floor( metres ) == metres )
            std::snprintf( text, sizeof( text ), "L%d · %.0f m", level, metres );
        else
            std::snprintf( text, sizeof( text ), "L%d · %g m", level, metres );
        return text;
    }

    std::vector<CellState> EditorCellStates( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                             std::span<const std::uint64_t>                   rowIds,
                                             const std::function<bool( std::uint64_t )>&      held )
    {
        std::vector<CellState> states;
        states.reserve( plan.Cells.size() );
        for ( const ::Desert::Core::Rules::PlannedCell& cell : plan.Cells )
        {
            std::size_t total = 0;
            std::size_t in    = 0;
            for ( const std::size_t c : cell.Composites )
            {
                if ( c >= plan.Composites.size() )
                    continue;
                for ( const std::size_t member : plan.Composites[c].Members )
                {
                    if ( member >= rowIds.size() )
                        continue;
                    ++total;
                    if ( held( rowIds[member] ) )
                        ++in;
                }
            }
            if ( total > 0 && in == total )
                states.push_back( CellState::EditorLoaded );
            else if ( in > 0 )
                states.push_back( CellState::EditorPartial );
            else
                states.push_back( CellState::EditorUnloaded );
        }
        return states;
    }

    CellState StateOf( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                       const ::Desert::Core::Rules::ResidencyState* residency, std::size_t cell,
                       std::span<const CellState> editor )
    {
        if ( residency == nullptr )
            return cell < editor.size() ? editor[cell] : CellState::Unstreamed;
        const std::size_t unit = plan.AlwaysLoaded.size() + cell;
        // Before the first step the state vector is empty (ResidencyState): nothing has been loaded yet.
        if ( unit >= residency->Units.size() )
            return CellState::Unloaded;
        switch ( residency->Units[unit].State )
        {
            case ::Desert::Core::Rules::Residency::Unloaded:
                return CellState::Unloaded;
            case ::Desert::Core::Rules::Residency::Loading:
                return CellState::Loading;
            case ::Desert::Core::Rules::Residency::Loaded:
                return CellState::Loaded;
            case ::Desert::Core::Rules::Residency::Activating:
                return CellState::MakingVisible;
            case ::Desert::Core::Rules::Residency::Activated:
                return CellState::Resident;
            case ::Desert::Core::Rules::Residency::Failed:
                return CellState::Failed;
        }
        return CellState::Failed;
    }

    glm::vec4 ColorOf( CellState state )
    {
        // UE GetLevelStreamingStatusColor, at the 0.25 opacity Draw2D gives a cell tile. Loaded = UE's
        // LEVEL_Loaded (read, not visible), Resident = LEVEL_Visible, Failed = LEVEL_FailedToLoad (Maroon),
        // Unstreamed = UE's default (White).
        constexpr float kTile = 0.25f;
        switch ( state )
        {
            case CellState::Unstreamed:
                return { 1.0f, 1.0f, 1.0f, kTile };
            case CellState::Unloaded:
                return { 1.0f, 0.0f, 0.0f, kTile };
            case CellState::Loading:
                return { 1.0f, 1.0f, 0.0f, kTile };
            case CellState::Loaded:
                return { 0.0f, 1.0f, 1.0f, kTile };
            case CellState::MakingVisible: // UE LEVEL_MakingVisible: FColor::Blue
                return { 0.0f, 0.0f, 1.0f, kTile };
            case CellState::Resident:
                return { 0.0f, 1.0f, 0.0f, kTile };
            case CellState::Failed:
                return { 142.0f / 255.0f, 35.0f / 255.0f, 35.0f / 255.0f, kTile };
            // UE's World Partition editor: a loaded region is drawn lit, the rest of the world dim.
            case CellState::EditorLoaded:
                return { 0.0f, 1.0f, 0.0f, kTile };
            case CellState::EditorPartial:
                return { 1.0f, 1.0f, 0.0f, kTile };
            case CellState::EditorUnloaded:
                return { 0.5f, 0.5f, 0.5f, kTile };
        }
        return { 1.0f, 1.0f, 1.0f, kTile };
    }

    const char* NameOf( CellState state )
    {
        switch ( state )
        {
            case CellState::Unstreamed:
                return "Not streaming";
            case CellState::Unloaded:
                return "Unloaded";
            case CellState::Loading:
                return "Loading";
            case CellState::Loaded:
                return "Loaded, waiting to activate";
            case CellState::MakingVisible:
                return "Activating, hidden until whole";
            case CellState::Resident:
                return "Resident";
            case CellState::Failed:
                return "Failed to load";
            case CellState::EditorLoaded:
                return "Loaded in the editor";
            case CellState::EditorPartial:
                return "Partly loaded in the editor";
            case CellState::EditorUnloaded:
                return "On disk";
        }
        return "Failed to load";
    }

    std::span<const CellState> LegendStates( bool streaming, bool editor )
    {
        static constexpr std::array kEdit      = { CellState::Unstreamed };
        static constexpr std::array kEditor    = { CellState::EditorUnloaded, CellState::EditorPartial,
                                                   CellState::EditorLoaded };
        static constexpr std::array kStreaming = { CellState::Unloaded, CellState::Loading,
                                                   CellState::Loaded,   CellState::MakingVisible,
                                                   CellState::Resident, CellState::Failed };
        if ( streaming )
            return kStreaming;
        if ( editor )
            return kEditor;
        return kEdit;
    }

    std::vector<LegendRow> Legend( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                   const ::Desert::Core::Rules::ResidencyState* residency, int level,
                                   std::span<const CellState> editor, std::optional<std::size_t> grid )
    {
        std::vector<LegendRow> rows;
        for ( const CellState state : LegendStates( residency != nullptr, !editor.empty() ) )
            rows.push_back( LegendRow{ state, 0 } );
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            if ( !OnGrid( plan.Cells[cell], grid ) || !Shown( plan.Cells[cell].Level, level ) )
                continue;
            // A state without a row is left uncounted, and then the rows no longer sum to the cell count:
            // that sum is what the suite asserts for every residency.
            const CellState state = StateOf( plan, residency, cell, editor );
            const auto      row   = std::ranges::find( rows, state, &LegendRow::State );
            if ( row != rows.end() )
                ++row->Count;
        }
        return rows;
    }

    double RadiusPixels( const View& view, double radiusCm )
    {
        return radiusCm * view.Scale;
    }

    ::Desert::Core::Rules::CellBounds RegionBetween( glm::dvec2 from, glm::dvec2 to )
    {
        ::Desert::Core::Rules::CellBounds region;
        region.MinX = static_cast<float>( std::min( from.x, to.x ) );
        region.MinZ = static_cast<float>( std::min( from.y, to.y ) );
        region.MaxX = static_cast<float>( std::max( from.x, to.x ) );
        region.MaxZ = static_cast<float>( std::max( from.y, to.y ) );
        return region;
    }
    std::vector<FootprintOnScreen> VisibleFootprints( const ::Desert::Core::Rules::WorldPartitionPlan& plan,
                                                      const View& view, glm::dvec2 screenSize,
                                                      const std::function<bool( std::size_t )>& held )
    {
        std::vector<FootprintOnScreen> shown;
        for ( std::size_t c = 0; c < plan.Composites.size(); ++c )
        {
            const ::Desert::Core::Rules::PlannedComposite& composite = plan.Composites[c];
            if ( !composite.Footprint || composite.Anchor == ::Desert::Core::Rules::kNoRecord ||
                 !held( composite.Anchor ) )
                continue;
            const ::Desert::Core::Rules::CellBounds& box = *composite.Footprint;
            const glm::dvec2 a  = WorldToScreen( view, screenSize, { box.MinX, box.MinZ } );
            const glm::dvec2 b  = WorldToScreen( view, screenSize, { box.MaxX, box.MaxZ } );
            const glm::dvec2 lo = glm::min( a, b );
            const glm::dvec2 hi = glm::max( a, b );
            if ( hi.x < 0.0 || hi.y < 0.0 || lo.x > screenSize.x || lo.y > screenSize.y )
                continue;
            // UE culls by the box's on-screen extent and fades it in above the cull.
            const double extent = std::max( hi.x - lo.x, hi.y - lo.y );
            if ( extent < kMinFootprintPixels )
                continue;
            const double fade = std::clamp( ( extent - kMinFootprintPixels ) / kFootprintFadePixels, 0.0, 1.0 );
            shown.push_back( FootprintOnScreen{ c, lo, hi, static_cast<float>( fade ) } );
        }
        return shown;
    }

    bool LabelFits( const FootprintOnScreen& footprint, glm::dvec2 labelSize )
    {
        const glm::dvec2 room = footprint.Max - footprint.Min - glm::dvec2( 2.0 * kLabelPaddingPixels );
        return labelSize.x <= room.x && labelSize.y <= room.y;
    }

    std::vector<HLODState> HLODCellStates( const ::Desert::Core::Rules::WorldPartitionPlan&   plan,
                                           const ::Desert::Core::WorldPartitionSerialized&    partition,
                                           const ::Desert::Core::WorldCells::HLODWorldStatus& status )
    {
        namespace WC = ::Desert::Core::WorldCells;
        std::vector<HLODState> states;
        states.reserve( plan.Cells.size() );
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            // A cell's unit follows the always-loaded composites (WorldPartitionResidencyRules.hpp, UNITS).
            const WC::HLODNodeStatus* node = status.Find(
                 ::Desert::Core::Rules::HLODRowName( partition, plan, plan.AlwaysLoaded.size() + cell ) );
            if ( node == nullptr )
            {
                states.push_back( HLODState::None );
                continue;
            }
            switch ( node->State )
            {
                case WC::HLODNodeState::Current:
                    states.push_back( HLODState::Built );
                    break;
                case WC::HLODNodeState::Missing:
                case WC::HLODNodeState::Stale:
                    states.push_back( HLODState::Stale );
                    break;
                case WC::HLODNodeState::NoSource:
                    states.push_back( HLODState::None );
                    break;
            }
        }
        return states;
    }

    glm::vec4 ColorOf( HLODState state )
    {
        switch ( state )
        {
            case HLODState::None:
                return { 0.0f, 0.0f, 0.0f, 0.0f };
            case HLODState::Built:
                return { 0.35f, 0.65f, 1.0f, 0.9f };
            // Apart from every residency colour (red, yellow, cyan, green, maroon): a stale cell reads at a glance.
            case HLODState::Stale:
                return { 1.0f, 0.3f, 0.85f, 1.0f };
        }
        return { 0.0f, 0.0f, 0.0f, 0.0f };
    }

    const char* NameOf( HLODState state )
    {
        switch ( state )
        {
            case HLODState::None:
                return "No HLOD";
            case HLODState::Built:
                return "HLOD built";
            case HLODState::Stale:
                return "HLOD stale (Build HLODs again)";
        }
        return "No HLOD";
    }
} // namespace Desert::Editor::WorldPartitionMap
