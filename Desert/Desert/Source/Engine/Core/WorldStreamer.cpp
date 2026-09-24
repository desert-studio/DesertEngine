#include <Engine/Core/WorldStreamer.hpp>

#include <Engine/Core/Scene.hpp>
#include <Engine/Core/Serialize/SceneSerializer.hpp>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace Desert::Core
{
    WorldStreamer::WorldStreamer( Scene& scene, Assets::AssetManager& assets, SceneSerialized records )
         : m_Scene( &scene ), m_Assets( &assets ), m_Records( std::move( records ) )
    {
    }

    Common::ResultStr<std::unique_ptr<WorldStreamer>>
    WorldStreamer::Begin( Scene& scene, Assets::AssetManager& assets, const std::string& snapshotJson )
    {
        const auto started = std::chrono::steady_clock::now();
        auto       parsed  = ParseLoadableScene( "<Play snapshot>", snapshotJson );
        if ( !parsed )
            return Common::MakeError<std::unique_ptr<WorldStreamer>>( parsed.GetError() );

        std::unique_ptr<WorldStreamer> streamer( new WorldStreamer( scene, assets, parsed.ExtractValue() ) );
        auto                           where = streamer->Source();
        if ( !where )
            return Common::MakeError<std::unique_ptr<WorldStreamer>>( where.GetError() );
        const Rules::StreamingSource source = where.GetValue();
        auto                         begun = Rules::BeginWorldStreaming( streamer->m_Records, RegistryMeshBounds(),
                                                                         Rules::ResidencySettings{}, std::span( &source, 1 ), *streamer );
        if ( !begun )
            return Common::MakeError<std::unique_ptr<WorldStreamer>>( "world streaming: " + begun.GetError() );
        if ( !begun.GetValue().has_value() )
            return Common::MakeSuccess( std::unique_ptr<WorldStreamer>() );

        streamer->m_Executor     = begun.ExtractValue();
        streamer->m_Source       = std::make_unique<Rules::MemoryCellSource>( streamer->m_Executor->Plan(),
                                                                              streamer->m_Records.Entities );
        streamer->m_MostResident = streamer->m_Executor->LiveRecords();
        const double ms =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - started ).count();
        LOG_INFO( "[WorldPartition] '{0}': Play streams {1} record(s); {2} resident at the start around ({3:.0f}, "
                  "{4:.0f}) cm, the rest destroyed until their cell is wanted ({5:.1f} ms to parse, plan and "
                  "destroy).",
                  streamer->m_Records.SceneName, streamer->m_Records.Entities.size(), streamer->m_MostResident,
                  source.Position.x, source.Position.z, ms );
        return Common::MakeSuccess( std::move( streamer ) );
    }

    Common::ResultStr<Rules::StreamingSource> WorldStreamer::Source() const
    {
        // The origin is not a stand-in for a missing camera: it would stream the wrong neighbourhood in silence.
        const std::shared_ptr<Camera> camera = m_Scene->GetActiveCamera();
        if ( !camera )
            return Common::MakeError<Rules::StreamingSource>(
                 "world streaming of '" + m_Records.SceneName +
                 "': the scene has no active camera to stream around" );
        return Common::MakeSuccess( Rules::StreamingSource{ camera->GetPosition() } );
    }

    Common::BoolResultStr WorldStreamer::Tick( double nowSeconds )
    {
        auto where = Source();
        if ( !where )
            return Common::MakeError( where.GetError() );
        const Rules::StreamingSource source = where.GetValue();
        m_LastTick                          = TickReport{};
        auto                         tick   = m_Executor->Tick( std::span( &source, 1 ), nowSeconds, *this );
        if ( !tick )
            return Common::MakeError( "world streaming of '" + m_Records.SceneName + "': " + tick.GetError() );

        const Rules::ResidencyTick& done = tick.GetValue();
        m_LastTick.Tick                  = done;
        m_LastTick.LiveRecords           = m_Executor->LiveRecords();
        m_MostResident                   = std::max( m_MostResident, m_LastTick.LiveRecords );
        // A reference across a cell boundary is legal and its reader handles the absence — but it is SAID.
        if ( done.DeferredReferences > 0 || done.UnboundReferences > 0 )
        {
            LOG_INFO( "[WorldPartition] '{0}': {1} reference(s) from records just activated name an entity that "
                      "is not resident (they resolve when it is), and {2} reference(s) from resident records "
                      "named an entity that just left (they read null until it returns).",
                      m_Records.SceneName, done.DeferredReferences, done.UnboundReferences );
        }
        return BOOLSUCCESS;
    }

    Common::BoolResultStr WorldStreamer::Activate( std::size_t unit, std::span<const std::size_t> records )
    {
        const auto start = std::chrono::steady_clock::now();

        // Begin activates nothing (it keeps what is there), so every call reaching here has a source.
        auto read = m_Source->UnitRecords( unit );
        if ( !read )
            return Common::MakeError( "world streaming of '" + m_Records.SceneName + "': " + read.GetError() );
        const std::vector<Assets::EntityData>& unitRecords = read.GetValue();
        if ( unitRecords.size() != records.size() )
        {
            return Common::MakeError( "world streaming of '" + m_Records.SceneName + "': unit " +
                                      std::to_string( unit ) + " came back with " +
                                      std::to_string( unitRecords.size() ) + " record(s), the plan holds " +
                                      std::to_string( records.size() ) );
        }
        auto made =
             SceneSerializer( m_Scene, m_Assets ).InstantiateRecords( unitRecords, m_Records.SceneName, nullptr );

        const double ms =
             std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - start ).count();
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
            const auto entity = m_Scene->FindEntityByID( *m_Records.Entities.at( record ).id );
            if ( entity.has_value() )
                m_Scene->DestroyEntity( entity->get() );
        }
    }

    WorldStreamer::~WorldStreamer()
    {
        if ( !m_Executor.has_value() )
            return;
        LOG_INFO(
             "[WorldPartition] '{0}': streamed {1} activation(s), {2} record(s) in {3:.2f} ms ({4:.4f} ms per "
             "record, worst unit {5:.2f} ms); at most {6} of {7} record(s) resident at once.",
             m_Records.SceneName, m_Activations, m_RecordsActivated, m_ActivationMs,
             m_RecordsActivated > 0 ? m_ActivationMs / static_cast<double>( m_RecordsActivated ) : 0.0,
             m_WorstUnitMs, m_MostResident, m_Records.Entities.size() );
    }
} // namespace Desert::Core
