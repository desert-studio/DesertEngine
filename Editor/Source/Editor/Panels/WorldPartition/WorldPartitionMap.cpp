// Ported from UE 5.8
// Engine/Source/Editor/WorldPartitionEditor/Private/WorldPartition/SWorldPartitionEditorGrid2D.cpp and
// Engine/Source/Runtime/Engine/Private/LevelStreaming.cpp; the lines and what was adapted are in the header.

#include "WorldPartitionMap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace Desert::Editor::WorldPartitionMap
{
    namespace
    {
        bool Overlaps( const Core::Rules::CellBounds& a, glm::dvec2 lo, glm::dvec2 hi )
        {
            return a.MinX < hi.x && a.MaxX > lo.x && a.MinZ < hi.y && a.MaxZ > lo.y;
        }

        bool Shown( int cellLevel, int level )
        {
            return level < 0 || cellLevel == level;
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

    void Focus( View& view, glm::dvec2 screenSize, const Core::Rules::CellBounds& box )
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

    std::optional<Core::Rules::CellBounds> PlanBounds( const Core::Rules::WorldPartitionPlan& plan )
    {
        if ( plan.Cells.empty() )
            return std::nullopt;
        Core::Rules::CellBounds bounds = plan.Cells.front().Square;
        for ( const Core::Rules::PlannedCell& cell : plan.Cells )
        {
            bounds.MinX = std::min( bounds.MinX, cell.Square.MinX );
            bounds.MinZ = std::min( bounds.MinZ, cell.Square.MinZ );
            bounds.MaxX = std::max( bounds.MaxX, cell.Square.MaxX );
            bounds.MaxZ = std::max( bounds.MaxZ, cell.Square.MaxZ );
        }
        return bounds;
    }

    std::vector<std::size_t> VisibleCells( const Core::Rules::WorldPartitionPlan& plan, const View& view,
                                           glm::dvec2 screenSize, int level )
    {
        const glm::dvec2         lo = ScreenToWorld( view, screenSize, { 0.0, 0.0 } );
        const glm::dvec2         hi = ScreenToWorld( view, screenSize, screenSize );
        std::vector<std::size_t> visible;
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
            if ( Shown( plan.Cells[cell].Level, level ) && Overlaps( plan.Cells[cell].Square, lo, hi ) )
                visible.push_back( cell );
        std::stable_sort( visible.begin(), visible.end(), [&plan]( std::size_t a, std::size_t b )
                          { return plan.Cells[a].Level > plan.Cells[b].Level; } );
        return visible;
    }

    std::optional<std::size_t> CellAt( const Core::Rules::WorldPartitionPlan& plan, glm::dvec2 world, int level )
    {
        std::optional<std::size_t> found;
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            const Core::Rules::PlannedCell& candidate = plan.Cells[cell];
            const bool inside = world.x >= candidate.Square.MinX && world.x < candidate.Square.MaxX &&
                                world.y >= candidate.Square.MinZ && world.y < candidate.Square.MaxZ;
            if ( inside && Shown( candidate.Level, level ) &&
                 ( !found.has_value() || candidate.Level < plan.Cells[*found].Level ) )
                found = cell;
        }
        return found;
    }

    int GridLineLevel( const View& view, float cellSize, double minPixels )
    {
        // 62: the last level whose 2^L is exact in a double; a view that needs more is a view of nothing.
        for ( int level = 0; level < 62; ++level )
            if ( Core::Rules::LevelCellSize( cellSize, level ) * view.Scale >= minPixels )
                return level;
        return 62;
    }

    std::string LevelLabel( float cellSize, int level )
    {
        const double metres = Core::Rules::LevelCellSize( cellSize, level ) / 100.0;
        char         text[64];
        if ( std::floor( metres ) == metres )
            std::snprintf( text, sizeof( text ), "L%d · %.0f m", level, metres );
        else
            std::snprintf( text, sizeof( text ), "L%d · %g m", level, metres );
        return text;
    }

    CellState StateOf( const Core::Rules::WorldPartitionPlan& plan, const Core::Rules::ResidencyState* residency,
                       std::size_t cell )
    {
        if ( residency == nullptr )
            return CellState::Unstreamed;
        const std::size_t unit = plan.AlwaysLoaded.size() + cell;
        // Before the first step the state vector is empty (ResidencyState): nothing has been loaded yet.
        if ( unit >= residency->Units.size() )
            return CellState::Unloaded;
        switch ( residency->Units[unit].State )
        {
            case Core::Rules::Residency::Unloaded:
                return CellState::Unloaded;
            case Core::Rules::Residency::Loading:
                return CellState::Loading;
            case Core::Rules::Residency::Loaded:
                return CellState::Loaded;
            case Core::Rules::Residency::Activated:
                return CellState::Resident;
            case Core::Rules::Residency::Failed:
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
            case CellState::Resident:
                return { 0.0f, 1.0f, 0.0f, kTile };
            case CellState::Failed:
                return { 142.0f / 255.0f, 35.0f / 255.0f, 35.0f / 255.0f, kTile };
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
            case CellState::Resident:
                return "Resident";
            case CellState::Failed:
                return "Failed to load";
        }
        return "Failed to load";
    }

    std::span<const CellState> LegendStates( bool streaming )
    {
        static constexpr std::array kEdit      = { CellState::Unstreamed };
        static constexpr std::array kStreaming = { CellState::Unloaded, CellState::Loading, CellState::Loaded,
                                                   CellState::Resident, CellState::Failed };
        if ( streaming )
            return kStreaming;
        return kEdit;
    }

    std::vector<LegendRow> Legend( const Core::Rules::WorldPartitionPlan& plan,
                                   const Core::Rules::ResidencyState* residency, int level )
    {
        std::vector<LegendRow> rows;
        for ( const CellState state : LegendStates( residency != nullptr ) )
            rows.push_back( LegendRow{ state, 0 } );
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            if ( !Shown( plan.Cells[cell].Level, level ) )
                continue;
            // A state without a row is left uncounted, and then the rows no longer sum to the cell count:
            // that sum is what the suite asserts for every residency.
            const CellState state = StateOf( plan, residency, cell );
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
} // namespace Desert::Editor::WorldPartitionMap
