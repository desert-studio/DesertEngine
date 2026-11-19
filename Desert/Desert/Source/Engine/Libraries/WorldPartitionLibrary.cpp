#include <Engine/Libraries/WorldPartitionLibrary.hpp>

#include <Common/Core/Logger.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingRules.hpp>
#include <Engine/Core/WorldStreamer.hpp>
#include <Engine/Libraries/ScriptWorld.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace Desert::Libraries
{
    namespace
    {
        using Reflection::FieldType;
        using Reflection::Value;
        namespace Rules = Core::Rules;

        constexpr const char* kLibrary = "WorldPartition";

        // ONE QUERY AS THE RULE TAKES IT: a source the residency never sees.
        struct StreamingQuery
        {
            Rules::StreamingSource Source;
            bool                   Spatial = true;
        };

        // A WAIT OF waitUntilStreamed, kept on the scene's registry context until its query completes.
        struct StreamingWait
        {
            std::vector<StreamingQuery>           Queries;
            Rules::Residency                      Target = Rules::Residency::Activated;
            Reflection::Callable                  Done;
            std::chrono::steady_clock::time_point Since;
        };
        struct StreamingWaits
        {
            std::vector<StreamingWait> Pending;
        };

        Common::ResultStr<Rules::Residency> TargetState( const std::string& state, std::string_view call )
        {
            if ( state == "Loaded" )
                return Common::MakeSuccess( Rules::Residency::Loaded );
            if ( state == "Activated" )
                return Common::MakeSuccess( Rules::Residency::Activated );
            return Common::MakeFormattedError<Rules::Residency>(
                 "{}.{}: the state \"{}\" is not \"Loaded\" or \"Activated\" (UE EWorldPartitionRuntimeCellState)",
                 kLibrary, call, state );
        }

        bool Reached( Rules::Residency now, Rules::Residency target )
        {
            if ( now == Rules::Residency::Failed || now == Rules::Residency::Activated )
                return true;
            return target == Rules::Residency::Loaded && now == Rules::Residency::Loaded;
        }

        const char* StateName( Rules::Residency state )
        {
            switch ( state )
            {
                case Rules::Residency::Unloaded:
                    return "Unloaded";
                case Rules::Residency::Loading:
                    return "Loading";
                case Rules::Residency::Loaded:
                    return "Loaded";
                case Rules::Residency::Activated:
                    return "Activated";
                case Rules::Residency::Failed:
                    return "Failed";
            }
            return "Unloaded";
        }

        // The streamer of the running script's world, or the error naming why there is none.
        Common::ResultStr<Core::WorldStreamer*> StreamerOf( std::string_view call,
                                                            Core::Scene**    sceneOut = nullptr )
        {
            auto world = ScriptWorld( kLibrary, std::string( call ).c_str() );
            if ( !world )
                return Common::MakeError<Core::WorldStreamer*>( world.GetError() );
            Core::Scene* scene = world.GetValue();
            if ( sceneOut != nullptr )
                *sceneOut = scene;
            const auto* ref = scene->GetRegistry().try_ctx<Core::WorldStreamerOfScene>();
            if ( ref == nullptr || ref->Streamer == nullptr )
            {
                return Common::MakeFormattedError<Core::WorldStreamer*>(
                     "{}.{}: this world is not partitioned (no streaming runs)", kLibrary, call );
            }
            return Common::MakeSuccess( ref->Streamer );
        }

        // ── Reading a query table ───────────────────────────────────────────────────────────────────
        Common::ResultStr<double> NumberOf( const Value& value, std::string_view where )
        {
            if ( value.Type() == FieldType::Double )
                return Common::MakeSuccess( *value.Get<double>() );
            if ( value.Type() == FieldType::Float )
                return Common::MakeSuccess( static_cast<double>( *value.Get<float>() ) );
            if ( value.Type() == FieldType::Int )
                return Common::MakeSuccess( static_cast<double>( *value.Get<std::int64_t>() ) );
            return Common::MakeFormattedError<double>( "{} is not a number", where );
        }

        Common::ResultStr<bool> BoolOf( const Value& value, std::string_view where )
        {
            if ( value.Type() != FieldType::Bool )
                return Common::MakeFormattedError<bool>( "{} is not a boolean", where );
            return Common::MakeSuccess( *value.Get<bool>() );
        }

        Common::ResultStr<glm::vec3> VectorOf( const Value& value, std::string_view where )
        {
            if ( value.Type() != FieldType::Vec3 )
                return Common::MakeFormattedError<glm::vec3>( "{} is not a vector", where );
            const Value::Float3& v = *value.Get<Value::Float3>();
            return Common::MakeSuccess( glm::vec3( v[0], v[1], v[2] ) );
        }

        // Degrees counter-clockwise from -Z seen from above, as a direction on the ground.
        glm::vec3 FacingOfYaw( double degrees )
        {
            const double radians = degrees * 3.14159265358979323846 / 180.0;
            return glm::vec3( static_cast<float>( -std::sin( radians ) ), 0.0f,
                              static_cast<float>( -std::cos( radians ) ) );
        }

        // Radius / UseGridLoadingRange of a query or a shape: the radius when one is stated (UE
        // bUseGridLoadingRange off), nullopt for the grid's range; both stated as a contradiction is refused.
        Common::ResultStr<std::optional<float>> RangeOf( const Value::Map& map, std::string_view where )
        {
            const Value*        radius = map.Find( "Radius" );
            const Value*        grid   = map.Find( "UseGridLoadingRange" );
            std::optional<bool> useGrid;
            if ( grid != nullptr )
            {
                auto flag = BoolOf( *grid, std::format( "{}.UseGridLoadingRange", where ) );
                if ( !flag )
                    return Common::MakeError<std::optional<float>>( flag.GetError() );
                useGrid = flag.GetValue();
            }
            if ( radius == nullptr )
            {
                if ( useGrid.has_value() && !*useGrid )
                {
                    return Common::MakeFormattedError<std::optional<float>>(
                         "{} turns UseGridLoadingRange off and states no Radius", where );
                }
                return Common::MakeSuccess( std::optional<float>() );
            }
            if ( useGrid.has_value() && *useGrid )
            {
                return Common::MakeFormattedError<std::optional<float>>(
                     "{} states a Radius and UseGridLoadingRange true: one range, not two", where );
            }
            auto value = NumberOf( *radius, std::format( "{}.Radius", where ) );
            if ( !value )
                return Common::MakeError<std::optional<float>>( value.GetError() );
            if ( !std::isfinite( value.GetValue() ) || value.GetValue() <= 0.0 )
            {
                return Common::MakeFormattedError<std::optional<float>>( "{}.Radius is {}; a range is > 0 cm",
                                                                         where, value.GetValue() );
            }
            return Common::MakeSuccess( std::optional<float>( static_cast<float>( value.GetValue() ) ) );
        }

        Common::ResultStr<Rules::StreamingSourceShape> ShapeOf( const Value& value, std::string_view where )
        {
            if ( value.Type() != FieldType::Map )
                return Common::MakeFormattedError<Rules::StreamingSourceShape>( "{} is not a table", where );
            const Value::Map&           map = *value.Get<Value::Map>();
            Rules::StreamingSourceShape shape;
            for ( const std::string& key : map.Keys )
            {
                if ( key != "Radius" && key != "UseGridLoadingRange" && key != "LoadingRangeScale" &&
                     key != "IsSector" && key != "SectorAngle" && key != "Location" && key != "Yaw" )
                {
                    return Common::MakeFormattedError<Rules::StreamingSourceShape>(
                         "{} has no field '{}' (Radius, UseGridLoadingRange, LoadingRangeScale, IsSector, "
                         "SectorAngle, Location, Yaw)",
                         where, key );
                }
            }
            auto range = RangeOf( map, where );
            if ( !range )
                return Common::MakeError<Rules::StreamingSourceShape>( range.GetError() );
            shape.UseGridLoadingRange = !range.GetValue().has_value();
            shape.Radius              = range.GetValue().value_or( 0.0f );
            const auto number         = [&]( const char* key, float& into ) -> Common::BoolResultStr
            {
                const Value* field = map.Find( key );
                if ( field == nullptr )
                    return Common::MakeSuccess( true );
                auto read = NumberOf( *field, std::format( "{}.{}", where, key ) );
                if ( !read )
                    return Common::MakeError<bool>( read.GetError() );
                into = static_cast<float>( read.GetValue() );
                return Common::MakeSuccess( true );
            };
            for ( const auto& [key, into] :
                  { std::pair<const char*, float*>{ "LoadingRangeScale", &shape.LoadingRangeScale },
                    std::pair<const char*, float*>{ "SectorAngle", &shape.SectorAngle },
                    std::pair<const char*, float*>{ "Yaw", &shape.Yaw } } )
                if ( auto read = number( key, *into ); !read )
                    return Common::MakeError<Rules::StreamingSourceShape>( read.GetError() );
            if ( const Value* sector = map.Find( "IsSector" ); sector != nullptr )
            {
                auto flag = BoolOf( *sector, std::format( "{}.IsSector", where ) );
                if ( !flag )
                    return Common::MakeError<Rules::StreamingSourceShape>( flag.GetError() );
                shape.IsSector = flag.GetValue();
            }
            if ( const Value* location = map.Find( "Location" ); location != nullptr )
            {
                auto at = VectorOf( *location, std::format( "{}.Location", where ) );
                if ( !at )
                    return Common::MakeError<Rules::StreamingSourceShape>( at.GetError() );
                shape.Location = at.GetValue();
            }
            return Common::MakeSuccess( shape );
        }

        Common::ResultStr<std::vector<std::string>> NamesOf( const Value& value, std::string_view where )
        {
            using Names = std::vector<std::string>;
            if ( value.Type() != FieldType::Array )
                return Common::MakeFormattedError<Names>( "{} is not a list of names", where );
            Names names;
            for ( const Value& item : *value.Get<Value::Array>() )
            {
                if ( item.Type() != FieldType::String )
                    return Common::MakeFormattedError<Names>( "{} holds something that is not a name", where );
                names.push_back( *item.Get<std::string>() );
            }
            return Common::MakeSuccess( std::move( names ) );
        }

        Common::ResultStr<StreamingQuery> QueryOf( const Value& value, std::string_view where,
                                                   const Core::WorldPartitionSerialized& partition )
        {
            if ( value.Type() != FieldType::Map )
                return Common::MakeFormattedError<StreamingQuery>( "{} is not a table", where );
            const Value::Map& map = *value.Get<Value::Map>();
            for ( const std::string& key : map.Keys )
            {
                if ( key == "DataLayers" || key == "DataLayersOnly" )
                {
                    return Common::MakeFormattedError<StreamingQuery>(
                         "{}.{}: a query by data layer needs runtime data layers, which this engine's World "
                         "Partition does not stream yet (WP-DATA-LAYERS-RUNTIME)",
                         where, key );
                }
                if ( key != "Location" && key != "Radius" && key != "UseGridLoadingRange" && key != "Yaw" &&
                     key != "Shapes" && key != "TargetGrids" && key != "SpatialQuery" )
                {
                    return Common::MakeFormattedError<StreamingQuery>(
                         "{} has no field '{}' (Location, Radius, UseGridLoadingRange, Yaw, Shapes, TargetGrids, "
                         "SpatialQuery)",
                         where, key );
                }
            }
            StreamingQuery query;
            if ( const Value* spatial = map.Find( "SpatialQuery" ); spatial != nullptr )
            {
                auto flag = BoolOf( *spatial, std::format( "{}.SpatialQuery", where ) );
                if ( !flag )
                    return Common::MakeError<StreamingQuery>( flag.GetError() );
                query.Spatial = flag.GetValue();
            }
            if ( const Value* grids = map.Find( "TargetGrids" ); grids != nullptr )
            {
                auto names = NamesOf( *grids, std::format( "{}.TargetGrids", where ) );
                if ( !names )
                    return Common::MakeError<StreamingQuery>( names.GetError() );
                for ( const std::string& name : names.GetValue() )
                {
                    bool known = false;
                    for ( const Core::RuntimePartitionSerialized& grid : partition.RuntimePartitions )
                        known = known || grid.Name == name;
                    if ( !known )
                    {
                        return Common::MakeFormattedError<StreamingQuery>(
                             "{}.TargetGrids names the grid '{}', which this world does not have", where, name );
                    }
                }
                query.Source.TargetGrids = names.ExtractValue();
            }
            if ( !query.Spatial )
            {
                for ( const char* spatialOnly : { "Location", "Radius", "UseGridLoadingRange", "Yaw", "Shapes" } )
                    if ( map.Find( spatialOnly ) != nullptr )
                    {
                        return Common::MakeFormattedError<StreamingQuery>(
                             "{} is not spatial (SpatialQuery false) and states {}", where, spatialOnly );
                    }
                return Common::MakeSuccess( std::move( query ) );
            }

            const Value* location = map.Find( "Location" );
            if ( location == nullptr )
                return Common::MakeFormattedError<StreamingQuery>( "{} states no Location", where );
            auto at = VectorOf( *location, std::format( "{}.Location", where ) );
            if ( !at )
                return Common::MakeError<StreamingQuery>( at.GetError() );
            query.Source.Position = at.GetValue();
            if ( const Value* yaw = map.Find( "Yaw" ); yaw != nullptr )
            {
                auto degrees = NumberOf( *yaw, std::format( "{}.Yaw", where ) );
                if ( !degrees )
                    return Common::MakeError<StreamingQuery>( degrees.GetError() );
                query.Source.Facing = FacingOfYaw( degrees.GetValue() );
            }
            if ( const Value* shapes = map.Find( "Shapes" ); shapes != nullptr )
            {
                if ( map.Find( "Radius" ) != nullptr || map.Find( "UseGridLoadingRange" ) != nullptr )
                {
                    return Common::MakeFormattedError<StreamingQuery>(
                         "{} states Shapes and a range of its own: each shape states its range", where );
                }
                if ( shapes->Type() != FieldType::Array )
                    return Common::MakeFormattedError<StreamingQuery>( "{}.Shapes is not a list", where );
                const Value::Array& list = *shapes->Get<Value::Array>();
                for ( std::size_t index = 0; index < list.size(); ++index )
                {
                    auto shape = ShapeOf( list[index], std::format( "{}.Shapes[{}]", where, index + 1 ) );
                    if ( !shape )
                        return Common::MakeError<StreamingQuery>( shape.GetError() );
                    query.Source.Shapes.push_back( shape.GetValue() );
                }
                return Common::MakeSuccess( std::move( query ) );
            }
            auto range = RangeOf( map, where );
            if ( !range )
                return Common::MakeError<StreamingQuery>( range.GetError() );
            query.Source.LoadingRange = range.GetValue();
            return Common::MakeSuccess( std::move( query ) );
        }

        // A table is one query, a list is several (UE's TArray<FWorldPartitionStreamingQuerySource>).
        Common::ResultStr<std::vector<StreamingQuery>> QueriesOf( const Value& queries, std::string_view call,
                                                                  const Core::WorldPartitionSerialized& partition )
        {
            using Queries = std::vector<StreamingQuery>;
            Queries out;
            if ( queries.Type() == FieldType::Map )
            {
                auto one = QueryOf( queries, std::format( "{}.{}: the query", kLibrary, call ), partition );
                if ( !one )
                    return Common::MakeError<Queries>( one.GetError() );
                out.push_back( one.ExtractValue() );
                return Common::MakeSuccess( std::move( out ) );
            }
            if ( queries.Type() != FieldType::Array || queries.Get<Value::Array>()->empty() )
            {
                return Common::MakeFormattedError<Queries>(
                     "{}.{}: the query is a table or a non-empty list of tables", kLibrary, call );
            }
            const Value::Array& list = *queries.Get<Value::Array>();
            for ( std::size_t index = 0; index < list.size(); ++index )
            {
                auto one = QueryOf( list[index], std::format( "{}.{}: query {}", kLibrary, call, index + 1 ),
                                    partition );
                if ( !one )
                    return Common::MakeError<Queries>( one.GetError() );
                out.push_back( one.ExtractValue() );
            }
            return Common::MakeSuccess( std::move( out ) );
        }

        // THE ANSWER: how many cells the queries want that have not reached @p target, on the residency as the
        // last streaming tick left it (before the first one, nothing has arrived).
        Common::ResultStr<int> PendingFor( const Core::WorldStreamer&      streamer,
                                           std::span<const StreamingQuery> queries, Rules::Residency target,
                                           std::string_view call )
        {
            const Rules::WorldPartitionPlan&      plan      = streamer.Plan();
            const Core::WorldPartitionSerialized& partition = streamer.Partition();
            const Rules::ResidencyState&          state     = streamer.Residency();

            std::vector<std::uint8_t>           wanted( plan.Cells.size(), 0 );
            std::vector<Rules::StreamingSource> spatial;
            for ( const StreamingQuery& query : queries )
            {
                if ( query.Spatial )
                {
                    spatial.push_back( query.Source );
                    continue;
                }
                for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
                {
                    const std::size_t grid = plan.Cells[cell].Grid;
                    if ( grid < partition.RuntimePartitions.size() &&
                         Rules::TargetsGrid( query.Source, partition.RuntimePartitions[grid] ) )
                        wanted[cell] = 1;
                }
            }
            if ( !spatial.empty() )
            {
                auto wish = Rules::QueryStreamingCells( plan, partition,
                                                        std::span<const Rules::StreamingSource>( spatial ) );
                if ( !wish )
                    return Common::MakeFormattedError<int>( "{}.{}: {}", kLibrary, call, wish.GetError() );
                for ( const Rules::WantedCell& cell : wish.GetValue().Cells )
                    wanted[cell.Cell] = 1;
            }
            int pending = 0;
            for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
            {
                if ( wanted[cell] == 0 )
                    continue;
                const std::size_t      unit = plan.AlwaysLoaded.size() + cell;
                const Rules::Residency now =
                     unit < state.Units.size() ? state.Units[unit].State : Rules::Residency::Unloaded;
                if ( !Reached( now, target ) )
                    ++pending;
            }
            return Common::MakeSuccess( pending );
        }
    } // namespace

    Common::ResultStr<int> WorldPartitionLibrary::StreamingPending( const Reflection::Value& queries,
                                                                    const std::string&       state )
    {
        constexpr std::string_view call   = "streamingPending";
        auto                       target = TargetState( state, call );
        if ( !target )
            return Common::MakeError<int>( target.GetError() );
        auto streamer = StreamerOf( call );
        if ( !streamer )
            return Common::MakeError<int>( streamer.GetError() );
        auto parsed = QueriesOf( queries, call, streamer.GetValue()->Partition() );
        if ( !parsed )
            return Common::MakeError<int>( parsed.GetError() );
        return PendingFor( *streamer.GetValue(), parsed.GetValue(), target.GetValue(), call );
    }

    Common::ResultStr<int> WorldPartitionLibrary::AllStreamingPending()
    {
        constexpr std::string_view call     = "allStreamingPending";
        Core::Scene*               scene    = nullptr;
        auto                       streamer = StreamerOf( call, &scene );
        if ( !streamer )
            return Common::MakeError<int>( streamer.GetError() );
        // The sources as they stand now (UE: the registered streaming sources), each weighed as a spatial query.
        std::vector<StreamingQuery> queries;
        for ( Rules::StreamingSource& source : Core::WorldStreamer::GatherSources( *scene, {} ) )
            queries.push_back( StreamingQuery{ std::move( source ), true } );
        return PendingFor( *streamer.GetValue(), queries, Rules::Residency::Activated, call );
    }

    Common::BoolResultStr WorldPartitionLibrary::WaitUntilStreamed( const Reflection::Value&    queries,
                                                                    const std::string&          state,
                                                                    const Reflection::Callable& done )
    {
        constexpr std::string_view call   = "waitUntilStreamed";
        auto                       target = TargetState( state, call );
        if ( !target )
            return Common::MakeError<bool>( target.GetError() );
        if ( !done.Alive() )
            return Common::MakeFormattedError<bool>( "{}.{}: no function to call when streamed", kLibrary, call );
        Core::Scene* scene    = nullptr;
        auto         streamer = StreamerOf( call, &scene );
        if ( !streamer )
            return Common::MakeError<bool>( streamer.GetError() );
        auto parsed = QueriesOf( queries, call, streamer.GetValue()->Partition() );
        if ( !parsed )
            return Common::MakeError<bool>( parsed.GetError() );
        // Refused now, not on a later frame, if the rule refuses the query.
        if ( auto valid = PendingFor( *streamer.GetValue(), parsed.GetValue(), target.GetValue(), call ); !valid )
            return Common::MakeError<bool>( valid.GetError() );

        entt::registry& registry = scene->GetRegistry();
        StreamingWait   wait{ parsed.ExtractValue(), target.GetValue(), done, std::chrono::steady_clock::now() };
        if ( auto* waits = registry.try_ctx<StreamingWaits>(); waits != nullptr )
            waits->Pending.push_back( std::move( wait ) );
        else
        {
            StreamingWaits fresh;
            fresh.Pending.push_back( std::move( wait ) );
            registry.set<StreamingWaits>( std::move( fresh ) );
        }
        return Common::MakeSuccess( true );
    }

    Common::ResultStr<std::vector<std::string>> WorldPartitionLibrary::CellStatesAt( const glm::vec3& location )
    {
        using Lines   = std::vector<std::string>;
        auto streamer = StreamerOf( "cellStatesAt" );
        if ( !streamer )
            return Common::MakeError<Lines>( streamer.GetError() );
        const Rules::WorldPartitionPlan& plan  = streamer.GetValue()->Plan();
        const Rules::ResidencyState&     state = streamer.GetValue()->Residency();
        Lines                            lines;
        for ( std::size_t cell = 0; cell < plan.Cells.size(); ++cell )
        {
            const auto& square = plan.Cells[cell].Square;
            if ( !( location.x >= square.MinX && location.x < square.MaxX && location.z >= square.MinZ &&
                    location.z < square.MaxZ ) )
                continue;
            const std::size_t      unit = plan.AlwaysLoaded.size() + cell;
            const Rules::Residency now =
                 unit < state.Units.size() ? state.Units[unit].State : Rules::Residency::Unloaded;
            lines.push_back(
                 std::format( "{}: {}", Rules::DescribeResidencyUnit( plan, unit ), StateName( now ) ) );
        }
        return Common::MakeSuccess( std::move( lines ) );
    }

    void WorldPartitionLibrary::FireCompletedWaits( Core::Scene& scene )
    {
        entt::registry& registry = scene.GetRegistry();
        auto*           waits    = registry.try_ctx<StreamingWaits>();
        if ( waits == nullptr || waits->Pending.empty() )
            return;
        // Taken whole: a callback may wait again (that wait lands in the list and is weighed next frame).
        std::vector<StreamingWait> pending = std::move( waits->Pending );
        waits->Pending.clear();
        const auto*                ref = registry.try_ctx<Core::WorldStreamerOfScene>();
        std::vector<StreamingWait> still;
        for ( StreamingWait& wait : pending )
        {
            if ( !wait.Done.Alive() )
                continue; // its script reloaded or its entity is gone: dropped unfired, as a timer
            if ( ref == nullptr || ref->Streamer == nullptr )
            {
                LOG_ERROR( "[WorldPartition] a waitUntilStreamed wait is dropped: the world stopped streaming" );
                continue;
            }
            auto left = PendingFor( *ref->Streamer, wait.Queries, wait.Target, "waitUntilStreamed" );
            if ( !left )
            {
                LOG_ERROR( "[WorldPartition] a waitUntilStreamed wait is dropped: {}", left.GetError() );
                continue;
            }
            if ( left.GetValue() > 0 )
            {
                still.push_back( std::move( wait ) );
                continue;
            }
            const double ms =
                 std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - wait.Since )
                      .count();
            LOG_INFO( "[WorldPartition] streaming completed for query in {:.0f} ms ({})", ms,
                      wait.Target == Rules::Residency::Activated ? "Activated" : "Loaded" );
            const Value arg = Value::Double( ms );
            if ( Common::BoolResultStr called = wait.Done.Call( &arg, 1 ); !called.IsSuccess() )
                LOG_ERROR( "[Lua] waitUntilStreamed callback error: {}", called.GetError() );
            // A callback may have stopped streaming; the next wait reads the context again.
            ref = registry.try_ctx<Core::WorldStreamerOfScene>();
        }
        if ( still.empty() )
            return;
        if ( auto* after = registry.try_ctx<StreamingWaits>(); after != nullptr )
            after->Pending.insert( after->Pending.begin(), std::make_move_iterator( still.begin() ),
                                   std::make_move_iterator( still.end() ) );
        else
        {
            StreamingWaits kept;
            kept.Pending = std::move( still );
            registry.set<StreamingWaits>( std::move( kept ) );
        }
    }
} // namespace Desert::Libraries
