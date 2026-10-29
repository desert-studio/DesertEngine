#include "Editor/LevelEditor/AssetCompiling.hpp"

#include "Editor/Import/ImportManager.hpp"
#include "Editor/Panels/FileExplorer/FileExplorerPanel.hpp"

#include <Engine/Animation/AnimationLibrary.hpp>
#include <Engine/Assets/AssetEviction.hpp>
#include <Engine/Assets/BootContent.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Desert.hpp>
#include <Engine/ECS/Components.hpp>

namespace Desert::Editor
{
    std::size_t AssetCompiling::RemainingAssets() const
    {
        return MeshSourceCooksRunning();
    }

    void AssetCompiling::TickHotReload( const Common::Timestep& ts, Desert::Core::Scene* activeScene )
    {
        if ( m_AssetManager )
            m_AssetHotReload.Tick( ts, *m_AssetManager, activeScene );
    }

    void AssetCompiling::RebuildCookedAssets( Desert::Core::Scene* activeScene, FileExplorerPanel* fileExplorer )
    {
        // Idle first: re-registering rebuilds GPU textures/materials.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        if ( m_ImportManager )
        {
            // The user's command: an import (it rewrites the sources' assets), not the cook that only reads them.
            m_ImportManager->ImportAllFromDirectory( Common::Constants::Path::MESH_PATH, ImportPass::Import,
                                                     /*force=*/true );
            m_ImportManager->ImportAllFromDirectory( Common::Constants::Path::COLLECTIONS_PATH, ImportPass::Import,
                                                     /*force=*/true );
            // AND THE TEXTURES DIRECTORY, WHICH THIS COMMAND DID NOT REACH. Loose textures are cooked
            // only as a mesh's dependency or by a drag-and-drop, so `Assets/Textures/` — the checker
            // floor's texture and the sky panoramas — was the one place "Rebuild
            // Cooked Assets" could not rebuild. Found the day the container's version moved: the menu
            // entry whose whole job is "the cooked form is stale, make it again" left `T_Checker.tex`
            // stale, and the only remedy left was to drag the file back into the editor.
            (void)m_ImportManager->ImportLooseTextures();
        }

        // The clip rows may have changed with the re-cook; everything else is re-read on demand.
        if ( m_AnimationLibrary )
            Assets::IndexAnimationClips( *m_AssetManager, *m_AnimationLibrary );

        // Drop cached per-entity material instances so MeshECSSystem rebuilds them from the freshly
        // re-registered runtime materials (which now reference the reloaded texture images).
        if ( activeScene != nullptr )
        {
            auto& reg = activeScene->GetRegistry();
            reg.view<ECS::StaticMeshComponent>().each( []( auto, ECS::StaticMeshComponent& c )
                                                       { c.RuntimeMaterialInstances().clear(); } );
            reg.view<ECS::SkinnedMeshComponent>().each( []( auto, ECS::SkinnedMeshComponent& c )
                                                        { c.RuntimeMaterialInstances().clear(); } );
        }

        if ( fileExplorer != nullptr )
            fileExplorer->QueueRefresh();

        LOG_INFO( "[Editor] Rebuilt cooked assets" );
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
