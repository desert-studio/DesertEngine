#include <Engine/Core/WorldStreamer.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>

#include <Common/Utilities/FileSystem.hpp>

#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <chrono>
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
        // JobSystem workers; a pak read opens its own stream (PakFile.cpp), so it is safe there.
        WorldCells::FileReader VfsReader( std::string directory )
        {
            return [directory = std::move( directory )](
                        std::string_view name ) -> Common::ResultStr<std::vector<unsigned char>>
            {
                const std::string path = directory + std::string( name );
                auto              read = Common::Utils::FileSystem::ReadByteFileContent( path );
                if ( !read )
                    return Common::MakeError<std::vector<unsigned char>>( "cooked world file '" + path +
                                                                          "': " + read.GetError() );
                return Common::MakeSuccess( read.ExtractValue() );
            };
        }
    } // namespace

    Common::ResultStr<std::optional<CookedWorldStart>> ReadCookedWorld( const std::string& scenePath )
    {
        using Result = std::optional<CookedWorldStart>;
        CookedWorldStart world;
        world.Directory             = WorldCells::CookedWorldDirectory( scenePath );
        const std::string indexPath = world.Directory + std::string( WorldCells::kIndexFileName );
        if ( !Common::Utils::FileSystem::Exists( indexPath ) )
            return Common::MakeSuccess( Result() );

        const WorldCells::FileReader reader = VfsReader( world.Directory );
        auto                         bytes  = reader( WorldCells::kIndexFileName );
        if ( !bytes )
            return Common::MakeError<Result>( bytes.GetError() );
        auto index = WorldCells::ReadWorldIndex( indexPath, bytes.GetValue() );
        if ( !index )
            return Common::MakeError<Result>( index.GetError() );
        world.Index = std::make_shared<WorldCells::WorldIndex>( index.ExtractValue() );

        auto indexed = WorldCells::PlanFromIndex( *world.Index );
        if ( !indexed )
            return Common::MakeError<Result>( indexPath + ": " + indexed.GetError() );
        world.Indexed = indexed.ExtractValue();

        auto alwaysLoaded = WorldCells::AssembleAlwaysLoaded( *world.Index, reader );
        if ( !alwaysLoaded )
            return Common::MakeError<Result>( alwaysLoaded.GetError() );
        world.AlwaysLoadedJson = rfl::json::write( alwaysLoaded.GetValue() );
        return Common::MakeSuccess( Result( std::move( world ) ) );
    }

    WorldStreamer::WorldStreamer( Scene& scene, Assets::AssetManager& assets, std::string sceneName )
         : m_Scene( &scene ), m_Assets( &assets ), m_SceneName( std::move( sceneName ) )
    {
    }

    Common::ResultStr<std::unique_ptr<WorldStreamer>>
    WorldStreamer::Begin( Scene& scene, Assets::AssetManager& assets, const std::string& snapshotJson )
    {
        using Result       = std::unique_ptr<WorldStreamer>;
        const auto started = std::chrono::steady_clock::now();
        auto       parsed  = ParseLoadableScene( "<Play snapshot>", snapshotJson );
        if ( !parsed )
            return Common::MakeError<Result>( parsed.GetError() );

        auto   snapshot = std::make_shared<const SceneSerialized>( parsed.ExtractValue() );
        Result streamer( new WorldStreamer( scene, assets, snapshot->SceneName ) );
        auto   where = streamer->Source();
        if ( !where )
            return Common::MakeError<Result>( where.GetError() );
        const Rules::StreamingSource source = where.GetValue();
        // BEFORE the executor begins: its Begin destroys every unwanted record, and Destroy finds each by id.
        // A record without one is refused by that Begin; the null id here is never looked up.
        streamer->m_RecordIds.reserve( snapshot->Entities.size() );
        for ( const Assets::EntityData& record : snapshot->Entities )
            streamer->m_RecordIds.push_back( record.id.value_or( Common::UUID( 0 ) ) );
        auto begun = Rules::BeginWorldStreaming( *snapshot, RegistryMeshBounds(), Rules::ResidencySettings{},
                                                 std::span( &source, 1 ), *streamer );
        if ( !begun )
            return Common::MakeError<Result>( "world streaming: " + begun.GetError() );
        if ( !begun.GetValue().has_value() )
            return Common::MakeSuccess( Result() );

        streamer->m_Executor = begun.ExtractValue();
        streamer->m_Snapshot = snapshot;
        streamer->m_Loader   = std::make_unique<WorldCellLoader>(
             std::make_shared<Rules::MemoryCellSource>( streamer->m_Executor->Plan(), snapshot->Entities ) );
        streamer->m_MostResident = streamer->m_Executor->LiveRecords();
        LOG_INFO( "[WorldPartition] '{0}': Play streams {1} record(s); {2} resident at the start around ({3:.0f}, "
                  "{4:.0f}) cm, the rest destroyed until their cell is wanted ({5:.1f} ms to parse, plan and "
                  "destroy).",
                  streamer->m_SceneName, snapshot->Entities.size(), streamer->m_MostResident, source.Position.x,
                  source.Position.z, MsSince( started ) );
        return Common::MakeSuccess( std::move( streamer ) );
    }

    Common::ResultStr<std::unique_ptr<WorldStreamer>>
    WorldStreamer::BeginCooked( Scene& scene, Assets::AssetManager& assets, CookedWorldStart world )
    {
        using Result = std::unique_ptr<WorldStreamer>;
        Result streamer( new WorldStreamer( scene, assets, world.Index->SceneName ) );
        auto   begun = Rules::ResidencyExecutor::BeginFromAlwaysLoaded(
             std::move( world.Indexed.Plan ), world.Index->WorldPartition, Rules::ResidencySettings{},
             world.Indexed.RecordIds.size(), world.Indexed.Observations );
        if ( !begun )
            return Common::MakeError<Result>( "world streaming of '" + streamer->m_SceneName +
                                              "': " + begun.GetError() );
        streamer->m_Executor = begun.ExtractValue();
        streamer->m_RecordIds.reserve( world.Indexed.RecordIds.size() );
        for ( const std::uint64_t id : world.Indexed.RecordIds )
            streamer->m_RecordIds.emplace_back( id );
        streamer->m_Index  = world.Index;
        streamer->m_Loader = std::make_unique<WorldCellLoader>(
             std::make_shared<WorldCells::CookedCellSource>( *world.Index, VfsReader( world.Directory ) ) );
        streamer->m_MostResident = streamer->m_Executor->LiveRecords();
        LOG_INFO( "[WorldPartition] '{0}': a cooked world of {1} record(s) in {2} unit(s) from '{3}'; {4} "
                  "always-loaded record(s) are entities, every cell is read on a worker when first wanted.",
                  streamer->m_SceneName, world.Indexed.RecordIds.size(), world.Index->Units.size(),
                  world.Directory, streamer->m_MostResident );
        return Common::MakeSuccess( std::move( streamer ) );
    }

    Common::ResultStr<Rules::StreamingSource> WorldStreamer::Source() const
    {
        // The origin is not a stand-in for a missing camera: it would stream the wrong neighbourhood in silence.
        const std::shared_ptr<Camera> camera = m_Scene->GetActiveCamera();
        if ( !camera )
            return Common::MakeError<Rules::StreamingSource>(
                 "world streaming of '" + m_SceneName + "': the scene has no active camera to stream around" );
        return Common::MakeSuccess( Rules::StreamingSource{ camera->GetPosition() } );
    }

    Common::BoolResultStr WorldStreamer::Tick( double nowSeconds )
    {
        auto where = Source();
        if ( !where )
            return Common::MakeError( where.GetError() );
        const Rules::StreamingSource source = where.GetValue();

        m_LastTick = TickReport{};
        auto tick  = m_Executor->Tick( std::span( &source, 1 ), nowSeconds, *this );
        if ( !tick )
            return Common::MakeError( "world streaming of '" + m_SceneName + "': " + tick.GetError() );

        const Rules::ResidencyTick& done = tick.GetValue();
        m_LastTick.Tick                  = done;
        m_LastTick.LiveRecords           = m_Executor->LiveRecords();
        m_LastTick.LoadsInFlight         = m_Loader->InFlight();
        m_MostResident                   = std::max( m_MostResident, m_LastTick.LiveRecords );
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
                           Rules::DescribeResidencyUnit( m_Executor->Plan(), outcome.Unit ), outcome.Reason );
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
                                     Rules::DescribeResidencyUnit( m_Executor->Plan(), unit );
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

    WorldStreamer::~WorldStreamer()
    {
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
