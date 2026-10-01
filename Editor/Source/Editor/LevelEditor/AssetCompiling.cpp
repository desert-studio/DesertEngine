#include "Editor/LevelEditor/AssetCompiling.hpp"

#include "Editor/Import/CookPaths.hpp"
#include "Editor/Import/ImportManager.hpp"
#include "Editor/Import/MeshDnD.hpp"

#include <Common/Core/JobSystem.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Desert.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Runtime/Services/Mesh/MeshService.hpp>

#include <array>

namespace Desert::Editor
{
    void AssetCompiling::StartBackgroundCook()
    {
        m_BackgroundCook = std::make_unique<BackgroundCookQueue>(
             []( const std::filesystem::path& source )
             {
                 // One cooker per worker thread: Assimp importers are not reentrant.
                 thread_local ImportManager s_ThreadImporter;
                 return s_ThreadImporter.Import( source );
             },
             []( std::function<void()> job ) { Common::JobSystem::Get().Submit( std::move( job ) ); } );
        m_BackgroundCookStart = std::chrono::steady_clock::now();

        const std::array<std::filesystem::path, 2> roots{ Common::Constants::Path::MESH_PATH,
                                                          Common::Constants::Path::COLLECTIONS_PATH };
        for ( const std::filesystem::path& root : roots )
            for ( const std::filesystem::path& source : ImportManager::MeshSources( root ) )
                m_BackgroundCook->Enqueue( source );
        LOG_INFO( "[BackgroundCook] {} mesh source(s) queued on the JobSystem after the reveal; a source whose "
                  "cache entry is missing or stale stays Pending until its cook lands",
                  m_BackgroundCook->Total() );
    }

    void AssetCompiling::ReloadRecookedMesh( const std::filesystem::path& source )
    {
        // THE PENDING ASSET AND THE COOKED ONE MUST BE THE SAME HANDLE: the scene already names the Pending one,
        // and nothing rewrites the scene when the cook lands. The handle comes from the asset's path (or a
        // header stated in the file at that path), never from the envelope the cook minted, so it holds — and a
        // drift would be a scene pointing at a mesh that never arrives, so it is checked, not assumed.
        const std::filesystem::path staticPath = CookPaths::MeshAsset( source );
        std::optional<uint64_t>     pendingHandle;
        if ( const auto pending = m_AssetManager->FindByPath<Assets::MeshAsset>( staticPath.generic_string() ) )
        {
            pendingHandle = static_cast<uint64_t>( pending->GetMetadata().Handle );
            // The failed load is dropped with the built GPU mesh; the shell stays, so the next draw reads the
            // fresh entry through the path a first use takes.
            if ( const auto unloaded = pending->Unload(); !unloaded )
                LOG_ERROR( "[BackgroundCook] '{}' was cooked but its Pending asset could not be reset: {}",
                           staticPath.string(), unloaded.GetError() );
            if ( auto* service = Runtime::ResourceRegistry::GetMeshService() )
                (void)service->EvictBuilt( pending->GetMetadata().Handle );
        }
        const auto resolved = MeshDnD::ResolveOrImportMesh( *m_AssetManager, source.string() );
        if ( resolved.Handle.IsNull() )
        {
            LOG_ERROR( "[BackgroundCook] '{}' cooked but its asset did not resolve; it stays Pending",
                       source.string() );
            return;
        }
        if ( pendingHandle && *pendingHandle != static_cast<uint64_t>( resolved.Handle ) )
            LOG_ERROR( "[BackgroundCook] '{}' was Pending as handle {} and resolved as {} after its cook; the "
                       "scene's reference no longer reaches it",
                       source.string(), *pendingHandle, static_cast<uint64_t>( resolved.Handle ) );
        LOG_INFO( "[BackgroundCook] '{}' cooked; its asset now resolves{}", source.string(),
                  pendingHandle ? " under the handle it was Pending as" : "" );
    }

    void AssetCompiling::DrainBackgroundCook()
    {
        if ( !m_BackgroundCook )
            return;
        for ( const BackgroundCookQueue::Completed& done : m_BackgroundCook->Drain() )
        {
            switch ( DecideCookCompletion( done.Verdict ) )
            {
                case CookCompletionAction::Nothing:
                    break;
                case CookCompletionAction::ReportFailure:
                    ++m_BackgroundCookFailed;
                    LOG_ERROR( "[BackgroundCook] '{}' did not cook; its asset stays Pending (not drawn)",
                               done.Source.string() );
                    break;
                case CookCompletionAction::Reload:
                    ++m_BackgroundCookChanged;
                    ReloadRecookedMesh( done.Source );
                    break;
            }
        }
        if ( !m_BackgroundCookReported && m_BackgroundCook->AllSettled() )
        {
            m_BackgroundCookReported = true;
            LOG_INFO( "[BackgroundCook] {} mesh source(s) checked after the reveal in {} ms: {} cooked, {} failed",
                      m_BackgroundCook->Total(),
                      std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() -
                                                                             m_BackgroundCookStart )
                           .count(),
                      m_BackgroundCookChanged, m_BackgroundCookFailed );
        }
    }

    void AssetCompiling::TickHotReload( const Common::Timestep& ts, Desert::Core::Scene* activeScene )
    {
        if ( m_AssetManager )
            m_AssetHotReload.Tick( ts, *m_AssetManager, activeScene );
    }

    void AssetCompiling::AppendActionCommands( std::vector<PaletteCommand>& commands )
    {
        // Actions.
        //
        // RELEASING UNUSED ASSETS BY NAME. The sweep runs by itself on every scene change, which is the
        // policy (Engine/Assets/AssetEviction.hpp) — this is the same request under a name, so that a
        // person profiling a level can ask for it without changing scene, and so that the control channel
        // can. A capability reachable only as a side effect of something else is missing from THE
        // DICTIONARY, and the dictionary is this editor's claim that anything a person can do an agent can
        // do. It goes through the schedule rather than calling Run directly, so a manual sweep lands at the
        // same safe point in the frame as an automatic one.
        commands.push_back( { "Action", "Release unused assets", []
                              {
                                  Assets::AssetEvictionSchedule::Request( "asked for from the command "
                                                                          "palette" );
                                  return PaletteCommandDone();
                              } } );

        // REBUILD CONTENT REGISTRY — the remedy every refusal in this subsystem names, reachable
        // without restarting.
        //
        // Since T2.4 neither host scans the content roots at boot: `Cooked/AssetRegistry.dreg` is the
        // list of what the project has. Content authored IN this editor enters it the moment
        // `AssetManager::CreateAsset` sees the file, and content the cook writes enters it at the
        // write — but a file that arrived on disk with nobody looking (a `git pull`, a drop into the
        // folder while the editor was closed) has no row until something walks. The boot deliberately
        // does not walk; this is what does, on demand.
        //
        // IT IS IN THE DICTIONARY AND NOT ONLY IN A MENU, for the reason "Release unused assets" is:
        // a capability reachable only as a side effect of something else is missing from the palette,
        // and the palette is this editor's claim that anything a person can do an agent can do. The
        // packager refuses to build against a stale registry and its message names this command; a
        // named remedy that cannot be run is worse than no message.
        commands.push_back( { "Action", "Rebuild Content Registry", [this]
                              {
                                  const auto cooked = Assets::ContentRegistry::Refresh( *m_AssetManager );
                                  if ( !cooked )
                                      return Common::MakeFormattedError( "the content registry: {}",
                                                                         cooked.GetError() );
                                  LOG_INFO( "[ContentRegistry] {}", cooked.GetValue().Describe() );
                                  return PaletteCommandDone();
                              } } );
    }

} // namespace Desert::Editor
