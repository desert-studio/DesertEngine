#include <Engine/GameplayDebugger/GameplayDebuggerCategoryWorldPartition.hpp>

#if DESERT_DEV_INSTRUMENTS
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/WorldPartitionResidencyRules.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingRules.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Input/LocalPlayerInput.hpp>

#include <Common/Core/KeyCodes.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>
#include <Engine/GameplayDebugger/GameplayDebugger.hpp>

namespace Desert::Debug
{
    namespace
    {
        namespace Rules = Core::Rules;

        const glm::vec4 kWorldPartitionLabel( 0.65f, 0.65f, 0.65f, 1.0f );
        const glm::vec4 kWorldPartitionValue( 1.0f, 1.0f, 1.0f, 1.0f );
        const glm::vec4 kOnColor( 0.35f, 1.0f, 0.35f, 1.0f );
        const glm::vec4 kOffColor( 0.55f, 0.55f, 0.55f, 1.0f );
        const glm::vec4 kSourceColor( 1.0f, 0.55f, 0.1f, 1.0f );
        // A box no lower than this, cm: a flat cell (a landscape tile at one height) still reads as a box.
        constexpr float kMinBoxHalfHeight = 50.0f;
        // The 2D map: its side in pixels, its place from the view's top-right, the share of the widest loading
        // range it shows beyond it, the segments of a drawn circle.
        constexpr float kMapSide      = 360.0f;
        const glm::vec2 kMapOrigin    = glm::vec2( 20.0f, 60.0f );
        constexpr float kMapReach     = 1.5f;
        constexpr int   kArcSegments  = 32;

        // UE ULevelStreaming::GetLevelStreamingStatusColor, per residency.
        glm::vec3 StateColor( const Rules::Residency state )
        {
            switch ( state )
            {
                case Rules::Residency::Unloaded:
                    return { 0.45f, 0.45f, 0.45f };
                case Rules::Residency::Loading:
                    return { 1.0f, 1.0f, 0.0f };
                case Rules::Residency::Loaded:
                    return { 0.0f, 1.0f, 1.0f };
                case Rules::Residency::Activated:
                    return { 0.0f, 1.0f, 0.0f };
                case Rules::Residency::Failed:
                    return { 1.0f, 0.0f, 0.0f };
            }
            return { 1.0f, 1.0f, 1.0f };
        }

        const char* StateName( const Rules::Residency state )
        {
            switch ( state )
            {
                case Rules::Residency::Unloaded:
                    return "unloaded";
                case Rules::Residency::Loading:
                    return "loading";
                case Rules::Residency::Loaded:
                    return "loaded";
                case Rules::Residency::Activated:
                    return "activated";
                case Rules::Residency::Failed:
                    return "failed";
            }
            return "?";
        }

        const char* ColorModeName( const GameplayDebuggerCategoryWorldPartition::ColorMode mode )
        {
            switch ( mode )
            {
                case GameplayDebuggerCategoryWorldPartition::ColorMode::State:
                    return "state";
                case GameplayDebuggerCategoryWorldPartition::ColorMode::Grid:
                    return "grid";
                case GameplayDebuggerCategoryWorldPartition::ColorMode::DataLayer:
                    return "data layer";
            }
            return "?";
        }

        Rules::Residency UnitState( const Rules::ResidencyState& residency, const std::size_t unit )
        {
            return unit < residency.Units.size() ? residency.Units[unit].State : Rules::Residency::Unloaded;
        }

        glm::vec3 CellColor( const GameplayDebuggerCategoryWorldPartition::ColorMode mode,
                             const Rules::PlannedCell& cell, const Rules::Residency state,
                             const Core::WorldPartitionSerialized& partition )
        {
            switch ( mode )
            {
                case GameplayDebuggerCategoryWorldPartition::ColorMode::State:
                    return StateColor( state );
                case GameplayDebuggerCategoryWorldPartition::ColorMode::Grid:
                    return cell.Grid < partition.RuntimePartitions.size() ? partition.RuntimePartitions[cell.Grid].DebugColor
                                                                          : glm::vec3( 1.0f );
                case GameplayDebuggerCategoryWorldPartition::ColorMode::DataLayer:
                    if ( cell.DataLayers.empty() || !partition.DataLayers.has_value() ||
                         cell.DataLayers.front() >= partition.DataLayers->size() )
                        return { 0.45f, 0.45f, 0.45f }; // a cell of no runtime layer
                    {
                        const std::array<float, 3>& c = ( *partition.DataLayers )[cell.DataLayers.front()].DebugColor;
                        return glm::vec3( c[0], c[1], c[2] );
                    }
            }
            return { 1.0f, 1.0f, 1.0f };
        }

        std::string LayerNames( const Rules::PlannedCell& cell, const Core::WorldPartitionSerialized& partition )
        {
            std::string names;
            for ( const std::size_t layer : cell.DataLayers )
                if ( partition.DataLayers.has_value() && layer < partition.DataLayers->size() )
                    names += ( names.empty() ? "" : "+" ) + ( *partition.DataLayers )[layer].Name;
            return names;
        }

        std::string Millis( const double ms )
        {
            return ms < 0.0 ? std::string( "-" ) : std::format( "{:.1f} ms", ms );
        }

        bool Contains( const Core::Rules::CellBounds& square, const glm::vec3& point )
        {
            return point.x >= square.MinX && point.x < square.MaxX && point.z >= square.MinZ &&
                   point.z < square.MaxZ;
        }

        glm::vec4 WithAlpha( const glm::vec3& color, const float alpha )
        {
            return glm::vec4( color, alpha );
        }
    } // namespace

    void GameplayDebuggerCategoryWorldPartition::ReadInput( const Input::LocalPlayerInput& input )
    {
        if ( !input.IsKeyDown( Input::InputKey::Of( Common::KeyCode::LeftShift ) ) &&
             !input.IsKeyDown( Input::InputKey::Of( Common::KeyCode::RightShift ) ) )
            return;
        if ( input.WasKeyPressed( Input::InputKey::Of( Common::KeyCode::M ) ) )
            m_View.Hash2D = !m_View.Hash2D;
        if ( input.WasKeyPressed( Input::InputKey::Of( Common::KeyCode::B ) ) )
            m_View.Hash3D = !m_View.Hash3D;
        if ( input.WasKeyPressed( Input::InputKey::Of( Common::KeyCode::O ) ) )
            m_View.Sources = !m_View.Sources;
        if ( input.WasKeyPressed( Input::InputKey::Of( Common::KeyCode::I ) ) )
            m_View.CellDetails = !m_View.CellDetails;
        if ( input.WasKeyPressed( Input::InputKey::Of( Common::KeyCode::G ) ) )
            NextGridLevel();
        if ( input.WasKeyPressed( Input::InputKey::Of( Common::KeyCode::C ) ) )
            NextColorMode();
    }

    void GameplayDebuggerCategoryWorldPartition::NextGridLevel()
    {
        if ( !m_View.GridLevel )
            m_View.GridLevel = 0;
        else if ( *m_View.GridLevel + 1 < m_LevelCount )
            m_View.GridLevel = *m_View.GridLevel + 1;
        else
            m_View.GridLevel.reset();
    }

    void GameplayDebuggerCategoryWorldPartition::NextColorMode()
    {
        m_View.Coloring = m_View.Coloring == ColorMode::State  ? ColorMode::Grid
                          : m_View.Coloring == ColorMode::Grid ? ColorMode::DataLayer
                                                               : ColorMode::State;
    }

    void GameplayDebuggerCategoryWorldPartition::CollectData( const Core::Scene& scene, const ECS::Entity* target,
                                                              GameplayDebuggerCategoryData& out )
    {
        const auto onOff = []( const bool on ) { return on ? kOnColor : kOffColor; };
        out.AddTextLine(
             { { "Shift+M 2D  ", onOff( m_View.Hash2D ) },
               { "Shift+B 3D  ", onOff( m_View.Hash3D ) },
               { "Shift+O sources  ", onOff( m_View.Sources ) },
               { "Shift+I details  ", onOff( m_View.CellDetails ) },
               { std::format( "Shift+G level {}  ",
                              m_View.GridLevel ? std::to_string( *m_View.GridLevel ) : std::string( "all" ) ),
                 kWorldPartitionValue },
               { std::format( "Shift+C colour by {}", ColorModeName( m_View.Coloring ) ),
                 kWorldPartitionValue } } );

        const Core::WorldStreamer* streamer = scene.GetGameplayDebugger().GetWorldStreamer();
        if ( streamer == nullptr )
        {
            out.AddTextLine( { { "not streamed: World Partition streams in Play, a world that states a "
                                 "WorldPartition block",
                                 kWorldPartitionLabel } } );
            return;
        }
        const Rules::WorldPartitionPlan&      plan      = streamer->Plan();
        const Rules::ResidencyState&          residency = streamer->Residency();
        const Core::WorldPartitionSerialized& partition = streamer->Partition();
        const std::vector<Rules::StreamingSource>& sources = streamer->LastSources();
        const std::size_t                     firstCell = plan.AlwaysLoaded.size();
        m_LevelCount                                    = plan.LevelCount;
        const auto shown = [this]( const Rules::PlannedCell& cell )
        { return !m_View.GridLevel || cell.Level == *m_View.GridLevel; };

        std::size_t byState[5] = {};
        for ( std::size_t c = 0; c < plan.Cells.size(); ++c )
            ++byState[static_cast<std::size_t>( UnitState( residency, firstCell + c ) )];
        const Core::WorldStreamer::TickReport& tick = streamer->LastTick();
        out.AddTextLine(
             { { "Cells ", kWorldPartitionLabel },
               { std::format( "{} activated, {} loaded, {} loading, {} failed of {}",
                              byState[static_cast<std::size_t>( Rules::Residency::Activated )],
                              byState[static_cast<std::size_t>( Rules::Residency::Loaded )],
                              byState[static_cast<std::size_t>( Rules::Residency::Loading )],
                              byState[static_cast<std::size_t>( Rules::Residency::Failed )], plan.Cells.size() ),
                 kWorldPartitionValue },
               { std::format( "   records live {}, reads in flight {}, {} source(s)", tick.LiveRecords,
                              tick.LoadsInFlight, sources.size() ),
                 kWorldPartitionLabel } } );

        // 3D: every cell that is not Unloaded, a box over its square as high as its content.
        if ( m_View.Hash3D )
            for ( std::size_t c = 0; c < plan.Cells.size(); ++c )
            {
                const Rules::PlannedCell& cell  = plan.Cells[c];
                const Rules::Residency    state = UnitState( residency, firstCell + c );
                if ( state == Rules::Residency::Unloaded || !shown( cell ) )
                    continue;
                const glm::vec3 centre( ( cell.Square.MinX + cell.Square.MaxX ) * 0.5f,
                                        ( cell.Height.MinY + cell.Height.MaxY ) * 0.5f,
                                        ( cell.Square.MinZ + cell.Square.MaxZ ) * 0.5f );
                const glm::vec3 extent( ( cell.Square.MaxX - cell.Square.MinX ) * 0.5f,
                                        std::max( ( cell.Height.MaxY - cell.Height.MinY ) * 0.5f, kMinBoxHalfHeight ),
                                        ( cell.Square.MaxZ - cell.Square.MinZ ) * 0.5f );
                out.AddShape( GameplayDebuggerShape{
                     .Type   = GameplayDebuggerShape::Kind::Box,
                     .A      = centre,
                     .Extent = extent,
                     .Color  = WithAlpha( CellColor( m_View.Coloring, cell, state, partition ), 1.0f ) } );
            }

        // The sources' shapes per grid: a sphere of the loading range (measured in 3D, WP-SRC-ALTITUDE), a
        // sector's wedge on the ground at its altitude.
        if ( m_View.Sources )
            for ( std::size_t s = 0; s < sources.size(); ++s )
            {
                const Rules::StreamingSource& source = sources[s];
                out.AddShape( GameplayDebuggerShape{ .Type        = GameplayDebuggerShape::Kind::Point,
                                                     .A           = source.Position,
                                                     .Radius      = 6.0f,
                                                     .Color       = kSourceColor,
                                                     .Description = std::format( "source {}", s ) } );
                for ( const Core::RuntimePartitionSerialized& grid : partition.RuntimePartitions )
                    for ( const Rules::SourceSector& sector : Rules::SourceSectors( source, grid ) )
                    {
                        const glm::vec3 centre( static_cast<float>( sector.Centre.x ),
                                                static_cast<float>( sector.Altitude ),
                                                static_cast<float>( sector.Centre.y ) );
                        const float     radius = static_cast<float>( sector.Radius );
                        const glm::vec4 color  = WithAlpha( grid.DebugColor, 1.0f );
                        if ( sector.IsCircle() )
                        {
                            out.AddShape( GameplayDebuggerShape{ .Type   = GameplayDebuggerShape::Kind::Sphere,
                                                                 .A      = centre,
                                                                 .Radius = radius,
                                                                 .Color  = color } );
                            continue;
                        }
                        const double half = sector.Angle * 0.5;
                        const auto   edge = [&]( const double degrees )
                        {
                            const glm::dvec2 axis = Rules::Detail::TurnAboutUp( sector.Axis, degrees );
                            return centre + glm::vec3( static_cast<float>( axis.x ) * radius, 0.0f,
                                                       static_cast<float>( axis.y ) * radius );
                        };
                        out.AddShape( GameplayDebuggerShape{ .Type  = GameplayDebuggerShape::Kind::Segment,
                                                             .A     = centre,
                                                             .B     = edge( -half ),
                                                             .Color = color } );
                        out.AddShape( GameplayDebuggerShape{ .Type  = GameplayDebuggerShape::Kind::Segment,
                                                             .A     = centre,
                                                             .B     = edge( half ),
                                                             .Color = color } );
                        for ( int i = 0; i < kArcSegments; ++i )
                        {
                            const double a0 = -half + sector.Angle * i / kArcSegments;
                            const double a1 = -half + sector.Angle * ( i + 1 ) / kArcSegments;
                            out.AddShape( GameplayDebuggerShape{ .Type  = GameplayDebuggerShape::Kind::Segment,
                                                                 .A     = edge( a0 ),
                                                                 .B     = edge( a1 ),
                                                                 .Color = color } );
                        }
                    }
            }

        // Details: the cells under each source and under the debugger's target - the player and the cursor (UE
        // wp.Runtime.ToggleDrawRuntimeCellsDetails), in the panel and over the cell's box.
        struct Probe
        {
            std::string Name;
            glm::vec3   Position;
        };
        std::vector<Probe> probes;
        if ( m_View.CellDetails )
        {
            for ( std::size_t s = 0; s < sources.size(); ++s )
                probes.push_back( { std::format( "source {}", s ), sources[s].Position } );
            if ( target != nullptr )
                probes.push_back( { "target", glm::vec3( target->GetWorldTransform()[3] ) } );
        }
        // A cell under both the player and the target is listed for each, and labelled in the world once.
        std::vector<bool> labelled( plan.Cells.size(), false );
        for ( const Probe& probe : probes )
            for ( std::size_t c = 0; c < plan.Cells.size(); ++c )
            {
                const Rules::PlannedCell& cell = plan.Cells[c];
                if ( !shown( cell ) || !Contains( cell.Square, probe.Position ) )
                    continue;
                const std::size_t                      unit    = firstCell + c;
                const Rules::Residency                 state   = UnitState( residency, unit );
                const Core::WorldStreamer::UnitTiming* timing  = streamer->TimingOf( unit );
                const std::size_t                      records = Rules::ResidencyUnitRecords( plan, unit );
                const std::string                      layers  = LayerNames( cell, partition );
                const std::string                      name =
                     std::format( "G{} L{} {},{}{}", cell.Grid, cell.Level, cell.Cell.X, cell.Cell.Z,
                                  layers.empty() ? std::string() : std::format( " [{}]", layers ) );
                out.AddTextLine(
                     { { probe.Name + " in ", kWorldPartitionLabel },
                       { name, WithAlpha( CellColor( m_View.Coloring, cell, state, partition ), 1.0f ) },
                       { std::format( "  {} records, {}, loaded in {}, activated in {}, height {:.0f}..{:.0f} cm",
                                      records, StateName( state ), Millis( timing ? timing->LoadMs : -1.0 ),
                                      Millis( timing ? timing->ActivationMs : -1.0 ), cell.Height.MinY,
                                      cell.Height.MaxY ),
                         kWorldPartitionValue } } );
                if ( labelled[c] )
                    continue;
                labelled[c] = true;
                const glm::vec3 top( ( cell.Square.MinX + cell.Square.MaxX ) * 0.5f, cell.Height.MaxY,
                                     ( cell.Square.MinZ + cell.Square.MaxZ ) * 0.5f );
                out.AddShape( GameplayDebuggerShape{
                     .Type        = GameplayDebuggerShape::Kind::Point,
                     .A           = top,
                     .Radius      = 4.0f,
                     .Color       = kWorldPartitionValue,
                     .Description = std::format( "{} - {} records, loaded in {}", name, records,
                                                 Millis( timing ? timing->LoadMs : -1.0 ) ) } );
            }

        // 2D: the runtime hash around the first source (UE ToggleDrawRuntimeHash2D), top-right of the view.
        if ( !m_View.Hash2D )
            return;
        float reach = 0.0f;
        for ( const Core::RuntimePartitionSerialized& runtime : partition.RuntimePartitions )
            if ( const Core::RuntimePartitionLHGridSerialized* grid =
                      Rules::RuntimePartitionClass( runtime.Type ).CellGrid( runtime ) )
                reach = std::max( reach, grid->LoadingRange );
        reach *= kMapReach;
        const glm::vec3 middle = sources.empty() ? glm::vec3( 0.0f ) : sources.front().Position;
        if ( reach <= 0.0f )
            return;
        const float scale = kMapSide / ( 2.0f * reach );
        const auto  toMap = [&]( const float x, const float z )
        { return glm::vec2( ( x - ( middle.x - reach ) ) * scale, ( z - ( middle.z - reach ) ) * scale ); };

        Core::DebugDrawCanvas map{ .Origin = kMapOrigin,
                                   .Corner = Core::DebugDrawScreenCorner::TopRight,
                                   .Size   = glm::vec2( kMapSide ) };
        // Coarse levels first, so the small cells lie over the big ones.
        std::vector<std::size_t> order;
        for ( std::size_t c = 0; c < plan.Cells.size(); ++c )
        {
            const Rules::PlannedCell& cell = plan.Cells[c];
            if ( shown( cell ) && cell.Square.MaxX >= middle.x - reach && cell.Square.MinX <= middle.x + reach &&
                 cell.Square.MaxZ >= middle.z - reach && cell.Square.MinZ <= middle.z + reach )
                order.push_back( c );
        }
        std::stable_sort( order.begin(), order.end(), [&]( const std::size_t a, const std::size_t b )
                          { return plan.Cells[a].Level > plan.Cells[b].Level; } );
        for ( const std::size_t c : order )
        {
            const Rules::PlannedCell& cell  = plan.Cells[c];
            const Rules::Residency    state = UnitState( residency, firstCell + c );
            const glm::vec3           color = CellColor( m_View.Coloring, cell, state, partition );
            const glm::vec2           a     = toMap( cell.Square.MinX, cell.Square.MinZ );
            const glm::vec2           b     = toMap( cell.Square.MaxX, cell.Square.MaxZ );
            if ( state != Rules::Residency::Unloaded || m_View.Coloring != ColorMode::State )
                map.Shapes.push_back( { .Type = Core::DebugDrawCanvasShape::Kind::FilledRect,
                                        .A    = a,
                                        .B    = b,
                                        .Color = WithAlpha( color, state == Rules::Residency::Unloaded ? 0.15f
                                                                                                       : 0.4f ) } );
            map.Shapes.push_back( { .Type      = Core::DebugDrawCanvasShape::Kind::Rect,
                                    .A         = a,
                                    .B         = b,
                                    .Color     = WithAlpha( color, 0.9f ),
                                    .Thickness = 1.0f } );
        }
        for ( const Rules::StreamingSource& source : sources )
        {
            for ( const Core::RuntimePartitionSerialized& grid : partition.RuntimePartitions )
                for ( const Rules::SourceSector& sector : Rules::SourceSectors( source, grid ) )
                {
                    const glm::vec2 centre = toMap( static_cast<float>( sector.Centre.x ),
                                                    static_cast<float>( sector.Centre.y ) );
                    const float     radius = static_cast<float>( sector.Radius ) * scale;
                    const double    half   = sector.IsCircle() ? 180.0 : sector.Angle * 0.5;
                    const auto      at     = [&]( const double degrees )
                    {
                        const glm::dvec2 axis = Rules::Detail::TurnAboutUp( sector.Axis, degrees );
                        return centre + glm::vec2( static_cast<float>( axis.x ), static_cast<float>( axis.y ) ) *
                                             radius;
                    };
                    for ( int i = 0; i < kArcSegments; ++i )
                    {
                        const double    a0 = -half + 2.0 * half * i / kArcSegments;
                        const double    a1 = -half + 2.0 * half * ( i + 1 ) / kArcSegments;
                        const glm::vec2 p0 = at( a0 );
                        const glm::vec2 p1 = at( a1 );
                        if ( !sector.IsCircle() ) // the wedge, filled (UE's frustum / sector triangles)
                            map.Shapes.push_back( { .Type  = Core::DebugDrawCanvasShape::Kind::FilledTriangle,
                                                    .A     = centre,
                                                    .B     = p0,
                                                    .C     = p1,
                                                    .Color = WithAlpha( grid.DebugColor, 0.25f ) } );
                        map.Shapes.push_back( { .Type      = Core::DebugDrawCanvasShape::Kind::Line,
                                                .A         = p0,
                                                .B         = p1,
                                                .Color     = WithAlpha( grid.DebugColor, 1.0f ),
                                                .Thickness = 1.0f } );
                    }
                }
            const glm::vec2 dot = toMap( source.Position.x, source.Position.z );
            map.Shapes.push_back( { .Type  = Core::DebugDrawCanvasShape::Kind::FilledRect,
                                    .A     = dot - glm::vec2( 3.0f ),
                                    .B     = dot + glm::vec2( 3.0f ),
                                    .Color = kSourceColor } );
            if ( source.Facing )
            {
                const glm::vec2 facing( source.Facing->x, source.Facing->z );
                const float     length = std::sqrt( facing.x * facing.x + facing.y * facing.y );
                if ( length > 1e-4f )
                    map.Shapes.push_back( { .Type      = Core::DebugDrawCanvasShape::Kind::Line,
                                            .A         = dot,
                                            .B         = dot + facing / length * 18.0f,
                                            .Color     = kSourceColor,
                                            .Thickness = 2.0f } );
            }
        }
        map.Shapes.push_back(
             { .Type  = Core::DebugDrawCanvasShape::Kind::Text,
               .A     = glm::vec2( 6.0f, 4.0f ),
               .Color = kWorldPartitionValue,
               .Text  = std::format( "WP 2D  {:.0f} cm across  level {}  by {}", 2.0f * reach,
                                    m_View.GridLevel ? std::to_string( *m_View.GridLevel ) : std::string( "all" ),
                                     ColorModeName( m_View.Coloring ) ) } );
        out.Canvases.push_back( std::move( map ) );
    }
} // namespace Desert::Debug
#endif // DESERT_DEV_INSTRUMENTS
