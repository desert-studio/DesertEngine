#include <Engine/Core/WorldStreamer.hpp>

#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/HLODMeshBuilder.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/ECS/Entity.hpp>

#include <Common/Content/ContentChunks.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Common/Utilities/ProcessMemory.hpp>

#include <Common/Json/Json.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Desert::Core
{
    namespace
    {
        double MsSince( std::chrono::steady_clock::time_point started )
        {
            return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
        }

        // A cooked file's bytes through the VFS, so a pak and a loose directory read the same way. Called on
        // JobSystem workers; a pak read opens its own stream (PakFile.cpp), so it is safe there. A file the
        // packager shipped in an on-demand region chunk (WP23) mounts that chunk first — once, thread-safe; with
        // no chunks registered (editor, loose cooked files) the mount does nothing.
        using RegionChunks = std::shared_ptr<const std::map<std::string, std::string>>;
        WorldCells::FileReader VfsReader( std::string directory, RegionChunks regions )
        {
            return [directory = std::move( directory ), regions = std::move( regions )](
                        std::string_view name ) -> Common::ResultStr<std::vector<unsigned char>>
            {
                const std::string path = directory + std::string( name );
                if ( regions )
                    if ( const auto chunk = regions->find( std::string( name ) ); chunk != regions->end() )
                        if ( auto mounted = Common::Content::MountChunkOnDemand( chunk->second ); !mounted )
                            return Common::MakeError<std::vector<unsigned char>>( "cooked world file '" + path +
                                                                                  "' (chunk '" + chunk->second +
                                                                                  "'): " + mounted.GetError() );
                auto read = Common::Utils::FileSystem::ReadByteFileContent( path );
                if ( !read )
                    return Common::MakeError<std::vector<unsigned char>>( "cooked world file '" + path +
                                                                          "': " + read.GetError() );
                return Common::MakeSuccess( read.ExtractValue() );
            };
        }

        // A cooked world's cells, holding each loaded cell's region chunk while the cell is loaded (WP23): the
        // chunk unmounts when the last loaded cell of its region goes.
        class RegionChunkCellSource final : public Rules::WorldCellSource
        {
        public:
            RegionChunkCellSource( std::shared_ptr<const WorldCells::WorldIndex> index, std::string directory,
                                   RegionChunks regions )
                 : m_Index( std::move( index ) ), m_Regions( regions ),
                   m_Cooked( *m_Index, VfsReader( std::move( directory ), std::move( regions ) ) )
            {
            }

            [[nodiscard]] Common::ResultStr<std::vector<Assets::EntityData>>
            UnitRecords( std::size_t unit ) const override
            {
                return m_Cooked.UnitRecords( unit );
            }
            void HoldUnit( std::size_t unit ) const override
            {
                if ( const std::string* chunk = ChunkOf( unit ) )
                    if ( auto held = Common::Content::HoldChunkOnDemand( *chunk ); !held )
                        LOG_ERROR( "[WorldPartition] unit {}: {}", unit, held.GetError() );
            }
            void ReleaseUnit( std::size_t unit ) const override
            {
                if ( const std::string* chunk = ChunkOf( unit ) )
                    if ( auto released = Common::Content::ReleaseChunkOnDemand( *chunk ); !released )
                        LOG_ERROR( "[WorldPartition] unit {}: {}", unit, released.GetError() );
            }

        private:
            const std::string* ChunkOf( std::size_t unit ) const
            {
                if ( !m_Regions || unit >= m_Index->Units.size() )
                    return nullptr;
                const auto chunk = m_Regions->find( m_Index->Units[unit].File );
                return chunk != m_Regions->end() ? &chunk->second : nullptr;
            }

            std::shared_ptr<const WorldCells::WorldIndex> m_Index;
            RegionChunks                                  m_Regions;
            WorldCells::CookedCellSource                  m_Cooked;
        };
    } // namespace

    Common::ResultStr<std::optional<CookedWorldStart>> ReadCookedWorld( const std::string& scenePath )
    {
        using Result = std::optional<CookedWorldStart>;
        CookedWorldStart world;
        world.Directory             = WorldCells::CookedWorldDirectory( scenePath );
        const std::string indexPath = world.Directory + std::string( WorldCells::kIndexFileName );
        if ( !Common::Utils::FileSystem::Exists( indexPath ) )
            return Common::MakeSuccess( Result() );

        // The index ships in the world's Shared chunk, mounted at start: read before any region is known.
        auto bytes = VfsReader( world.Directory, nullptr )( WorldCells::kIndexFileName );
        if ( !bytes )
            return Common::MakeError<Result>( bytes.GetError() );
        auto index = WorldCells::ReadWorldIndex( indexPath, bytes.GetValue() );
        if ( !index )
            return Common::MakeError<Result>( index.GetError() );
        world.Index = std::make_shared<WorldCells::WorldIndex>( index.ExtractValue() );

        auto layout = WorldCells::ChunkLayoutOf( *world.Index, world.Directory );
        if ( !layout )
            return Common::MakeError<Result>( indexPath + ": " + layout.GetError() );
        if ( layout.GetValue().has_value() )
        {
            auto regions = std::make_shared<std::map<std::string, std::string>>();
            for ( const auto& [file, chunk] : layout.GetValue()->FileChunk )
                if ( chunk != layout.GetValue()->Shared )
                    regions->emplace( file, chunk );
            world.RegionChunkOfFile = std::move( regions );
        }
        const WorldCells::FileReader reader = VfsReader( world.Directory, world.RegionChunkOfFile );

        auto indexed = WorldCells::PlanFromIndex( *world.Index );
        if ( !indexed )
            return Common::MakeError<Result>( indexPath + ": " + indexed.GetError() );
        world.Indexed = indexed.ExtractValue();

        auto alwaysLoaded = WorldCells::AssembleAlwaysLoaded( *world.Index, reader );
        if ( !alwaysLoaded )
            return Common::MakeError<Result>( alwaysLoaded.GetError() );
        world.AlwaysLoadedJson = Common::Json::Write( alwaysLoaded.GetValue() );
        return Common::MakeSuccess( Result( std::move( world ) ) );
    }

    WorldStreamer::WorldStreamer( Scene& scene, Assets::AssetManager& assets, std::string sceneName )
         : m_Scene( &scene ), m_Assets( &assets ), m_SceneName( std::move( sceneName ) )
    {
    }

    Common::ResultStr<std::unique_ptr<WorldStreamer>>
    WorldStreamer::Begin( Scene& scene, Assets::AssetManager& assets, const std::string& snapshotJson,
                          const std::filesystem::path&            scenePath,
                          std::span<const Rules::StreamingSource> instruments )
    {
        using Result       = std::unique_ptr<WorldStreamer>;
        const auto started = std::chrono::steady_clock::now();
        auto       parsed  = ParseLoadableScene( "<Play snapshot>", snapshotJson );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );

        auto   snapshot = std::make_shared<const SceneSerialized>( std::move( parsed.ExtractValue().Scene ) );
        Result streamer( new WorldStreamer( scene, assets, snapshot->SceneName ) );
        std::vector<Rules::StreamingSource> sources = GatherSources( scene, instruments );
        // BEFORE the executor begins: its Begin destroys every unwanted record, and Destroy finds each by id.
        // A record without one is refused by that Begin; the null id here is never looked up.
        streamer->m_RecordIds.reserve( snapshot->Entities.size() );
        for ( const Assets::EntityData& record : snapshot->Entities )
            streamer->m_RecordIds.push_back( record.id.value_or( Common::UUID( 0 ) ) );
        auto begun = Rules::BeginWorldStreaming( *snapshot, RegistryMeshBounds(), streamer->m_Settings, sources,
                                                 *streamer );
        if ( !begun )
            return Common::MakeError<Result>( "world streaming: " + begun.GetError() );
        if ( !begun.GetValue().has_value() )
            return Common::MakeSuccess( Result() );

        streamer->m_Executor = begun.ExtractValue();
        // A streaming begin is what a WorldPartition block produces; a begin without one is a broken invariant.
        const std::optional<WorldPartitionSerialized>& partition = snapshot->WorldPartition;
        if ( !partition.has_value() )
            return Common::MakeError<Result>( "world streaming of '" + snapshot->SceneName +
                                              "': streaming began but the snapshot states no WorldPartition" );
        streamer->m_Partition = *partition;
        streamer->UseSources( std::move( sources ) );
        streamer->m_Snapshot   = snapshot;
        streamer->m_Loader     = std::make_unique<WorldCellLoader>(
             std::make_shared<Rules::MemoryCellSource>( streamer->Executor().Plan(), snapshot->Entities ) );
        {
            // THE SAVED HLODs (WP-FAR-6B D1; UE: PIE shows what Build HLODs saved): Play reads them and never
            // builds them. Missing or stale is a logged error naming the scene and the step; Play goes on without
            // the HLODs concerned.
            const Rules::WorldPartitionPlan&              plan = streamer->Executor().Plan();
            std::unordered_map<Common::UUID, std::size_t> byId;
            for ( std::size_t record = 0; record < snapshot->Entities.size(); ++record )
                byId.emplace( streamer->m_RecordIds[record], record );
            std::vector<CellHLOD>             hlods;
            std::vector<Rules::HLODExclusion> holes;
            if ( auto layers = Rules::ValidateHLODLayers( *partition ); !layers )
                return Common::MakeError<Result>( "world streaming of '" + snapshot->SceneName +
                                                  "': " + layers.GetError() );
            if ( Rules::AnyGridHLODLayer( *partition ) )
            {
                auto stored = scenePath.empty()
                                   ? Common::MakeError<WorldCells::StoredHLODs>(
                                          "'" + snapshot->SceneName + "' is not saved, so it has no built HLODs" )
                                   : WorldCells::ReadBuiltHLODs( scenePath );
                if ( !stored )
                {
                    LOG_ERROR( "[WorldPartition] Play of '{0}' runs WITHOUT HLODs: {1}. Run Build > Build HLODs.",
                               snapshot->SceneName, stored.GetError() );
                }
                else
                {
                    // Shared with every HLOD read below: BeginHLODs runs them after this block has ended.
                    const auto held = std::make_shared<const WorldCells::StoredHLODs>( stored.ExtractValue() );
                    const auto registries = std::span( &Assets::ContentRegistry::Get(), 1 );
                    const std::vector<Rules::EntityDescriptor> descriptors =
                         Rules::DescribeEntities( snapshot->Entities );
                    auto stale = WorldCells::StaleHLODCells( held->Index, *snapshot, descriptors, registries );
                    if ( !stale )
                        return Common::MakeError<Result>( "world streaming of '" + snapshot->SceneName +
                                                          "': " + stale.GetError() );
                    const std::set<std::string> staleUnits( stale.GetValue().begin(), stale.GetValue().end() );
                    std::unordered_map<std::string, const WorldCells::BuiltHLODCell*> rows;
                    for ( const WorldCells::BuiltHLODCell& row : held->Index.Cells )
                        rows.emplace( row.Unit, &row );
                    for ( std::size_t unit = plan.AlwaysLoaded.size(); unit < Rules::ResidencyUnitCount( plan );
                          ++unit )
                    {
                        const std::string name = Rules::DescribeResidencyUnit( plan, unit );
                        const auto        row  = rows.find( name );
                        if ( staleUnits.contains( name ) || row == rows.end() )
                        {
                            LOG_ERROR(
                                 "[WorldPartition] Play of '{0}': the saved HLOD of {1} is missing or stale, "
                                 "so that cell has none. Run Build > Build HLODs.",
                                 snapshot->SceneName, name );
                            continue;
                        }
                        for ( const WorldCells::IndexNotInstanced& missing : row->second->NotInstanced )
                            holes.push_back( missing.Reason );
                        // A row without a file, or whose file lists no record, stands in for nothing.
                        if ( !row->second->File.has_value() || row->second->Ids.empty() )
                            continue;
                        // Read when BeginHLODs makes it, not now (CellHLOD): the read shares `held` (this block
                        // ends before that call); `byId` and `snapshot` outlive it.
                        const WorldCells::BuiltHLODCell* cell = row->second;
                        hlods.push_back( CellHLOD{
                             unit,
                             [held, &byId, &snapshot, cell,
                              name]() -> Common::ResultStr<std::vector<Assets::EntityData>>
                             {
                                 using Records = std::vector<Assets::EntityData>;
                                 auto records  = WorldCells::StoredCellRecords( *held, *cell );
                                 if ( !records )
                                     return Common::MakeError<Records>( "world streaming of '" +
                                                                        snapshot->SceneName + "', " + name + ": " +
                                                                        records.GetError() );
                                 for ( const Assets::EntityData& record : records.GetValue() )
                                     if ( byId.contains( *record.id ) )
                                         return Common::MakeError<Records>(
                                              "world streaming of '" + snapshot->SceneName +
                                              "': an HLOD record of " + name +
                                              " has a record's id; rename the world" );
                                 return records;
                             } } );
                    }
                    LOG_INFO( "[WorldPartition] Play of '{0}': {1} saved HLOD(s) from {2}, {3} stale cell(s)",
                              snapshot->SceneName, hlods.size(),
                              WorldCells::BuiltHLODDirectory( scenePath ).string(), staleUnits.size() );
                }
            }
            if ( auto begun = streamer->BeginHLODs( std::move( hlods ), holes ); !begun )
                return Common::MakeError<Result>( begun.GetError() );
        }
        streamer->m_MostResident = streamer->Executor().LiveRecords();
        LOG_INFO( "[WorldPartition] '{0}': Play streams {1} record(s); {2} resident at the start around {3} "
                  "streaming source(s), the rest destroyed until their cell is wanted ({4:.1f} ms to parse, plan "
                  "and destroy).",
                  streamer->m_SceneName, snapshot->Entities.size(), streamer->m_MostResident,
                  streamer->m_LastSources.size(), MsSince( started ) );
        // THE PLAY START IS THE LARGEST DEPARTURE OF ALL. The editor had the whole world open; the executor has
        // just destroyed every record outside the resident cells, and what those records drew - built meshes,
        // materials, textures - stayed resident until the first cell happened to leave. Measured (WP14 flight):
        // six corpus textures, 21.3 MB, resident from frame 0 in a world whose first resident cells name none.
        if ( streamer->m_MostResident < snapshot->Entities.size() )
        {
            Assets::AssetEvictionSchedule::Request(
                 "world '" + streamer->m_SceneName + "': Play began, " +
                 std::to_string( snapshot->Entities.size() - streamer->m_MostResident ) +
                 " record(s) outside the resident cells destroyed" );
        }
        return Common::MakeSuccess( std::move( streamer ) );
    }

    Common::ResultStr<std::unique_ptr<WorldStreamer>>
    WorldStreamer::BeginCooked( Scene& scene, Assets::AssetManager& assets, CookedWorldStart world )
    {
        using Result = std::unique_ptr<WorldStreamer>;
        Result streamer( new WorldStreamer( scene, assets, world.Index->SceneName ) );
        // Every read of `world` is resolved before the plan is moved out of it: an argument list is not
        // sequenced (clang evaluates it left to right, MSVC right to left).
        const auto  recordCount  = world.Indexed.RecordIds.size();
        const auto& partition    = world.Index->WorldPartition;
        auto&       observations = world.Indexed.Observations;
        auto        plan         = std::move( world.Indexed.Plan );
        auto        begun        = Rules::ResidencyExecutor::BeginFromAlwaysLoaded(
             std::move( plan ), partition, streamer->m_Settings, recordCount, observations );
        if ( !begun )
            return Common::MakeError<Result>( "world streaming of '" + streamer->m_SceneName +
                                              "': " + begun.GetError() );
        streamer->m_Executor = begun.ExtractValue();
        streamer->m_RecordIds.reserve( world.Indexed.RecordIds.size() );
        for ( const std::uint64_t id : world.Indexed.RecordIds )
            streamer->m_RecordIds.emplace_back( id );
        streamer->m_Partition = partition;
        streamer->m_Index     = world.Index;
        streamer->m_Loader    = std::make_unique<WorldCellLoader>(
             std::make_shared<RegionChunkCellSource>( world.Index, world.Directory, world.RegionChunkOfFile ) );
        {
            // Every HLOD file is read now, on this thread: HLODs stand in for the whole world from the first
            // frame (their own streaming, UE's HLOD grid, is not this round's).
            const WorldCells::FileReader      reader = VfsReader( world.Directory, world.RegionChunkOfFile );
            std::vector<CellHLOD>             hlods;
            std::vector<Rules::HLODExclusion> holes;
            for ( std::size_t hlod = 0; hlod < world.Index->HLODs.size(); ++hlod )
            {
                const WorldCells::IndexHLOD& row = world.Index->HLODs[hlod];
                for ( const WorldCells::IndexNotInstanced& missing : row.NotInstanced )
                    holes.push_back( missing.Reason );
                if ( !row.File.has_value() )
                    continue;
                // Read when BeginHLODs makes it (CellHLOD); `world` and `reader` outlive that call, made below.
                hlods.push_back( CellHLOD{
                     row.Unit,
                     [&world, &reader, hlod, &streamer]() -> Common::ResultStr<std::vector<Assets::EntityData>>
                     {
                         auto records = WorldCells::HLODRecords( *world.Index, reader, hlod );
                         if ( !records )
                             return Common::MakeError<std::vector<Assets::EntityData>>(
                                  "world streaming of '" + streamer->m_SceneName + "': " + records.GetError() );
                         return records;
                     } } );
            }
            if ( auto begun = streamer->BeginHLODs( std::move( hlods ), holes ); !begun )
                return Common::MakeError<Result>( begun.GetError() );
        }
        streamer->m_MostResident = streamer->Executor().LiveRecords();
        LOG_INFO( "[WorldPartition] '{0}': a cooked world of {1} record(s) in {2} unit(s) from '{3}'; {4} "
                  "always-loaded record(s) are entities, every cell is read on a worker when first wanted.",
                  streamer->m_SceneName, world.Indexed.RecordIds.size(), world.Index->Units.size(),
                  world.Directory, streamer->m_MostResident );
        return Common::MakeSuccess( std::move( streamer ) );
    }

    std::vector<Rules::StreamingSource>
    WorldStreamer::GatherSources( Scene& scene, std::span<const Rules::StreamingSource> instruments,
                                  std::vector<std::uint32_t>* entities, std::vector<std::uint32_t>* withoutCamera )
    {
        std::vector<Rules::StreamingSource> sources;
        entt::registry&                     registry = scene.GetRegistry();
        for ( const auto entity : registry.view<ECS::StreamingSourceComponent>() )
        {
            const ECS::StreamingSourceData& data = registry.get<ECS::StreamingSourceComponent>( entity ).Data;
            if ( !data.Enabled )
                continue;
            const glm::mat4        world = ECS::Entity( entity, registry ).GetWorldTransform();
            Rules::StreamingSource source;
            source.Position = glm::vec3( world[3] );
            // The entity's forward is its local −Z (the camera's convention), in world axes.
            source.Facing                = -glm::vec3( world[2] );
            source.Priority              = data.Priority;
            source.UseVelocityForSorting = data.UseVelocityForSorting;
            source.TargetGrids           = data.TargetGrids;
            source.LookAheadTime         = data.LookAheadTime;
            source.MaxLookAhead          = data.MaxLookAhead;
            // One shape, always (UE's FStreamingSourceShape on the component): with the defaults it is exactly
            // the circle of the range a source with no shapes loads in.
            Rules::StreamingSourceShape shape;
            shape.UseGridLoadingRange = !data.OverrideLoadingRange;
            shape.Radius              = data.LoadingRange;
            shape.IsSector            = data.IsSector;
            shape.SectorAngle         = data.SectorAngle;
            if ( data.IsSector && data.SectorAngleFromCamera )
            {
                const auto* camera = registry.try_get<ECS::CameraComponent>( entity );
                if ( camera == nullptr )
                {
                    if ( withoutCamera != nullptr )
                        withoutCamera->push_back( static_cast<std::uint32_t>( entity ) );
                }
                else
                {
                    // The component's FOV is what gameplay zooms; the width comes from the viewport it was last
                    // drawn into. Before its first frame nothing has drawn through it, it has no width yet, and
                    // its vertical angle stands for one frame.
                    const float aspect =
                         camera->Camera ? camera->Camera->GetViewportAspect().value_or( 1.0f ) : 1.0f;
                    shape.SectorAngle = CameraViewAngle( camera->Data.FOV, aspect );
                }
            }
            shape.Location            = data.ShapeOffset;
            shape.Yaw                 = data.ShapeYaw;
            source.Shapes.push_back( shape );
            sources.push_back( std::move( source ) );
            if ( entities != nullptr )
                entities->push_back( static_cast<std::uint32_t>( entity ) );
        }
        // THE PLAYER (UE's APlayerController, a streaming source provider at GetPlayerViewPoint): with a pawn
        // the pawn's own component above stands for it; with none - a level without a Default Pawn, played from
        // here or through an Auto Activate camera - the player is a source where it looks from.
        if ( scene.GetPlayerPawn() == entt::null )
            if ( const std::optional<glm::mat4> view = scene.PlayerViewPoint() )
            {
                Rules::StreamingSource player;
                player.Position = glm::vec3( ( *view )[3] );
                player.Facing   = -glm::vec3( ( *view )[2] );
                sources.push_back( std::move( player ) );
            }
        sources.insert( sources.end(), instruments.begin(), instruments.end() );
        return sources;
    }

    float WorldStreamer::CameraViewAngle( const float verticalFovDegrees, const float aspect )
    {
        const double halfVertical =
             static_cast<double>( verticalFovDegrees ) * 0.5 * 3.14159265358979323846 / 180.0;
        const double horizontal = 2.0 * std::atan( std::tan( halfVertical ) * static_cast<double>( aspect ) );
        return static_cast<float>( std::min( 360.0, horizontal * 180.0 / 3.14159265358979323846 ) );
    }

    void WorldStreamer::StampVelocities( std::vector<Rules::StreamingSource>& sources,
                                         std::span<const std::uint32_t> entities, double nowSeconds )
    {
        // UE's UWorldPartitionSubsystem::UpdateStreamingSourcesVelocity: the distance an entity's source moved
        // since the last Tick, over the time between them. A source new this Tick has none yet.
        const double                                 elapsed = nowSeconds - m_SourcesAt;
        std::unordered_map<std::uint32_t, glm::vec3> now;
        for ( std::size_t index = 0; index < entities.size(); ++index )
        {
            now[entities[index]] = sources[index].Position;
            const auto before    = m_SourcePositions.find( entities[index] );
            if ( before != m_SourcePositions.end() && elapsed > 0.0 )
                sources[index].Velocity =
                     ( sources[index].Position - before->second ) / static_cast<float>( elapsed );
        }
        m_SourcePositions = std::move( now );
        m_SourcesAt       = nowSeconds;
    }

    void WorldStreamer::UseSources( std::vector<Rules::StreamingSource> sources )
    {
        if ( sources.empty() && !m_SaidNoSource )
        {
            m_SaidNoSource = true;
            LOG_ERROR( "[WorldPartition] '{0}': Play has no streaming source — no entity has an enabled Streaming "
                       "Source, the level spawned no pawn and the player has no view — so only the always-loaded "
                       "part of the world is resident. Give the level a Default Pawn or put a Streaming Source on "
                       "an entity.",
                       m_SceneName );
        }
        m_LastSources = std::move( sources );
    }

    Common::BoolResultStr WorldStreamer::Tick( double                                  nowSeconds,
                                               std::span<const Rules::StreamingSource> instruments )
    {
        std::vector<std::uint32_t>          entities;
        std::vector<std::uint32_t>          withoutCamera;
        std::vector<Rules::StreamingSource> gathered =
             GatherSources( *m_Scene, instruments, &entities, &withoutCamera );
        for ( const std::uint32_t entity : withoutCamera )
            if ( m_SaidWithoutCamera.insert( entity ).second )
                LOG_ERROR( "[WorldPartition] '{0}': entity {1}'s Streaming Source takes its sector's angle from "
                           "its camera (Angle From Camera) but the entity has no Camera; it streams its authored "
                           "Sector Angle of {2} degrees until it has one.",
                           m_SceneName, entity,
                           m_Scene->GetRegistry()
                                .get<ECS::StreamingSourceComponent>( static_cast<entt::entity>( entity ) )
                                .Data.SectorAngle );
        StampVelocities( gathered, entities, nowSeconds );
        UseSources( std::move( gathered ) );
        const std::span<const Rules::StreamingSource> sources = m_LastSources;

        m_LastTick = TickReport{};
        // The HLODs switch on the state this very tick leaves, before the frame is drawn.
        auto tick = Rules::TickWithHLODs( Executor(), *m_HLODs, // NOLINT(bugprone-unchecked-optional-access)
                                          sources, nowSeconds, *this, *this );
        if ( !tick )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': " + tick.GetError() );

        const Rules::ResidencyTick& done = tick.GetValue();
        m_LastTick.Tick                  = done;
        m_LastTick.LiveRecords           = Executor().LiveRecords();
        m_LastTick.LoadsInFlight         = m_Loader->InFlight();
        m_MostResident                   = std::max( m_MostResident, m_LastTick.LiveRecords );
        m_LastTick.Streaming =
             Rules::AssessStreaming( Executor().Plan(), m_Partition, m_Settings, Executor().State(), sources );
        const bool wasWaiting = m_FramesWaiting > 0;
        m_FramesWaiting       = m_LastTick.Streaming.Blocks() ? m_FramesWaiting + 1 : 0;
        if ( m_FramesWaiting == 1 )
        {
            LOG_INFO( "[WorldPartition] '{0}': the cell under a streaming source ({1}) is not resident; play "
                      "waits for it",
                      m_SceneName,
                      Rules::DescribeResidencyUnit( Executor().Plan(), m_LastTick.Streaming.UnderSource ) );
        }
        else if ( wasWaiting && m_FramesWaiting == 0 )
        {
            LOG_INFO( "[WorldPartition] '{0}': the cell under the streaming source is resident; play resumes",
                      m_SceneName );
        }
        if ( Rules::ReleasesCellAssets( done ) )
        {
            Assets::AssetEvictionSchedule::Request(
                 "world '" + m_SceneName + "': " + std::to_string( done.UnitsDeactivated ) +
                 " cell(s) deactivated, " + std::to_string( done.UnitsUnloaded ) + " unloaded" );
        }
        WorldStreamingWait wait{ m_LastTick.Streaming, m_FramesWaiting, {} };
        // The entity sources lead the gathered list, in the order of entities.
        for ( std::size_t index = 0; index < entities.size(); ++index )
        {
            auto completion = Rules::AssessSourceCompletion( Executor().Plan(), m_Partition, Executor().State(),
                                                             m_LastSources[index] );
            if ( !completion )
                return Common::MakeError( "world streaming of '" + m_SceneName + "': " + completion.GetError() );
            wait.Sources.emplace( entities[index], completion.GetValue() );
        }
        m_Scene->GetRegistry().set<WorldStreamingWait>( std::move( wait ) );
        // A reference across a cell boundary is legal and its reader handles the absence — but it is SAID.
        if ( done.DeferredReferences > 0 || done.UnboundReferences > 0 )
        {
            LOG_INFO( "[WorldPartition] '{0}': {1} reference(s) from records just activated name an entity that "
                      "is not resident (they resolve when it is), and {2} reference(s) from resident records "
                      "named an entity that just left (they read null until it returns).",
                      m_SceneName, done.DeferredReferences, done.UnboundReferences );
        }
        return BOOLSUCCESS;
    }

#if DESERT_DEV_INSTRUMENTS
    void WorldStreamer::SetDebugLoadDelayTicks( std::uint32_t ticks )
    {
        m_Loader->SetDebugDelayTicks( ticks );
    }
#endif

    void WorldStreamer::StartLoad( std::size_t unit, std::uint64_t ticket )
    {
        m_Loader->Start( unit, ticket );
    }

    void WorldStreamer::CancelLoad( std::size_t unit, std::uint64_t ticket )
    {
        m_Loader->Cancel( unit, ticket );
    }

    void WorldStreamer::Unload( std::size_t unit )
    {
        m_Loader->Unload( unit );
    }

    std::vector<Rules::LoadOutcome> WorldStreamer::TakeLoadOutcomes()
    {
        std::vector<Rules::LoadOutcome> finished = m_Loader->TakeFinished();
        for ( const Rules::LoadOutcome& outcome : finished )
            if ( !outcome.Ok )
                LOG_ERROR( "[WorldPartition] '{0}': reading {1} failed, it is retried: {2}", m_SceneName,
                           Rules::DescribeResidencyUnit( Executor().Plan(), outcome.Unit ), outcome.Reason );
        return finished;
    }

    Common::BoolResultStr WorldStreamer::Activate( std::size_t unit, std::span<const std::size_t> records )
    {
        const auto start = std::chrono::steady_clock::now();

        // StepResidency activates only a Loaded unit, and a unit is Loaded only once its read was reported Ok.
        const std::vector<Assets::EntityData>* unitRecords = m_Loader->Records( unit );
        if ( unitRecords == nullptr )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': unit " + std::to_string( unit ) +
                                      " is activated with no finished read of its records" );
        if ( unitRecords->size() != records.size() )
        {
            return Common::MakeError( "world streaming of '" + m_SceneName + "': unit " + std::to_string( unit ) +
                                      " came back with " + std::to_string( unitRecords->size() ) +
                                      " record(s), the plan holds " + std::to_string( records.size() ) );
        }
        auto made = SceneSerializer( m_Scene, m_Assets ).InstantiateRecords( *unitRecords, m_SceneName, nullptr );

        const double ms = MsSince( start );
        ++m_Activations;
        m_RecordsActivated += records.size();
        m_ActivationMs += ms;
        m_WorstUnitMs = std::max( m_WorstUnitMs, ms );
        m_LastTick.ActivationMs += ms;
        m_LastTick.ActivatedUnits += ( m_LastTick.ActivatedUnits.empty() ? "" : " " ) +
                                     Rules::DescribeResidencyUnit( Executor().Plan(), unit );
        return made;
    }

    void WorldStreamer::Destroy( std::span<const std::size_t> records )
    {
        for ( const std::size_t record : records )
        {
            // A child already went with its parent's tree; a prefab instance's inner entities go with its root.
            const auto entity = m_Scene->FindEntityByID( m_RecordIds.at( record ) );
            if ( entity.has_value() )
                m_Scene->DestroyEntity( entity->get() );
        }
    }

    Common::BoolResultStr WorldStreamer::BeginHLODs( std::vector<CellHLOD>                 hlods,
                                                     std::span<const Rules::HLODExclusion> holes )
    {
        std::vector<std::size_t> units;
        units.reserve( hlods.size() );
        for ( const CellHLOD& hlod : hlods )
            units.push_back( hlod.Unit );
        auto runtime = Rules::HLODRuntime::Make( Executor().Plan(), units );
        if ( !runtime )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': " + runtime.GetError() );
        m_HLODs = runtime.ExtractValue();
        // ONE HLOD'S RECORDS ALIVE AT A TIME (CellHLOD): read, made entities, dropped before the next read.
        const Common::Utils::ProcessFootprint before = Common::Utils::ReadProcessFootprint();
        m_HLODIds.clear();
        std::size_t recordCount = 0;
        for ( CellHLOD& hlod : hlods )
        {
            std::vector<Common::UUID>& ids     = m_HLODIds.emplace_back();
            auto                       records = hlod.Read();
            if ( !records )
                return Common::MakeError( records.GetError() );
            for ( const Assets::EntityData& record : records.GetValue() )
            {
                if ( !record.id.has_value() || record.id->IsNull() )
                    return Common::MakeError( "world streaming of '" + m_SceneName +
                                              "': a record of the HLOD of " +
                                              Rules::DescribeResidencyUnit( Executor().Plan(), hlod.Unit ) +
                                              " has no id, so it could not be shown or hidden" );
                ids.push_back( *record.id );
            }
            recordCount += records.GetValue().size();
            if ( auto made = SceneSerializer( m_Scene, m_Assets )
                                  .InstantiateRecords( records.GetValue(), m_SceneName, nullptr );
                 !made )
                return Common::MakeError( "world streaming of '" + m_SceneName + "': making the HLOD of " +
                                          Rules::DescribeResidencyUnit( Executor().Plan(), hlod.Unit ) +
                                          " failed: " + made.GetError() );
            hlod.Read = nullptr; // what the read holds goes with the records
        }
        // The census the Play start is judged by: the process before and after every HLOD became entities.
        LOG_INFO( "[WorldPartition] '{0}': {1} HLOD(s), {2} record(s) made one HLOD at a time; process before: "
                  "{3}; after: {4}",
                  m_SceneName, hlods.size(), recordCount, before.Describe(),
                  Common::Utils::ReadProcessFootprint().Describe() );
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): assigned just above
        if ( auto synced = m_HLODs->Sync( Executor().State(), *this ); !synced )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': " + synced.GetError() );
        // From here on every switch is an event a flight reads (WP22c): the first Sync above set them all.
        m_HLODSwitchesSaid = true;
        // THE HOLES: a drawing record the builder could not batch has no stand-in (WorldPartitionHLODSwitch.hpp).
        const std::string holesText =
             holes.empty() ? std::string( "every drawing record has its stand-in" )
                           : std::to_string( holes.size() ) +
                                  " drawing record(s) have none and leave a hole while their cell is not drawn: " +
                                  Rules::DescribeHLODHoles( holes );
        // A hole is something the player sees vanish (WP-FAR-1: a whole landscape beyond the loading range), so
        // it is a warning, not a line among the info.
        if ( holes.empty() )
        {
            LOG_INFO( "[WorldPartition] '{0}': {1} HLOD(s), {2} record(s), stand in for the cells not drawn; {3}.",
                      m_SceneName, units.size(), recordCount, holesText );
        }
        else
        {
            LOG_WARN( "[WorldPartition] '{0}': {1} HLOD(s), {2} record(s), stand in for the cells not drawn; {3}.",
                      m_SceneName, units.size(), recordCount, holesText );
        }
        return BOOLSUCCESS;
    }

    void WorldStreamer::SetHLODVisible( std::size_t hlod, bool visible )
    {
        for ( const Common::UUID& id : m_HLODIds.at( hlod ) )
        {
            // An HLOD entity gameplay destroyed has nothing left to show or hide.
            const auto entity = m_Scene->FindEntityByID( id );
            if ( entity.has_value() )
                m_Scene->SetVisibleRecursive( entity->get(), visible );
        }
        if ( !m_HLODSwitchesSaid || !m_HLODs.has_value() )
            return;
        // A grid's stand-in replaces only that grid's cells (UE UWorldPartitionRuntimeHashSet: each runtime
        // partition owns its HLOD setup), so the switch is said with the grid it belongs to.
        const Rules::WorldPartitionPlan& plan = Executor().Plan();
        const std::size_t                unit = m_HLODs->UnitOf( hlod );
        const std::size_t                grid =
             unit < plan.AlwaysLoaded.size() ? 0 : plan.Cells.at( unit - plan.AlwaysLoaded.size() ).Grid;
        const std::string gridName =
             grid < m_Partition.Grids.size() ? m_Partition.Grids[grid].Name : std::string( "?" );
        LOG_INFO( "[WorldPartition] '{0}': grid '{1}' HLOD of {2} {3}.", m_SceneName, gridName,
                  Rules::DescribeResidencyUnit( plan, unit ), visible ? "shown" : "hidden" );
    }

    WorldStreamer::~WorldStreamer()
    {
        // The scene outlives its streamer (hosts reset the streamer before clearing the scene); what the
        // streamer said about it must not outlive the streamer.
        if ( m_Scene != nullptr )
            m_Scene->GetRegistry().unset<WorldStreamingWait>();
        if ( !m_Executor.has_value() )
            return;
        LOG_INFO( "[WorldPartition] '{0}': streamed {1} activation(s), {2} record(s) in {3:.2f} ms on the main "
                  "thread ({4:.4f} ms per record, worst unit {5:.2f} ms), their reads {6:.2f} ms on workers; at "
                  "most {7} of {8} record(s) resident at once.",
                  m_SceneName, m_Activations, m_RecordsActivated, m_ActivationMs,
                  m_RecordsActivated > 0 ? m_ActivationMs / static_cast<double>( m_RecordsActivated ) : 0.0,
                  m_WorstUnitMs, m_Loader->WorkerMs(), m_MostResident, m_RecordIds.size() );
    }
} // namespace Desert::Core
