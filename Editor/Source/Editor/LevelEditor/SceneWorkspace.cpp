#include "Editor/LevelEditor/SceneWorkspace.hpp"

#include "Editor/Core/Commands/SceneCommands.hpp"
#include "Editor/Core/PanelRegistry.hpp"
#include "Editor/Panels/ViewportPanel/ViewportCameraPreset.hpp"
#include "Editor/Panels/ViewportPanel/ViewportPanel.hpp"
#include <Editor/Core/Selection/SelectionManager.hpp>

#include <Engine/Core/Camera.hpp>
#include <Engine/Core/Scene.hpp>
#include <Engine/Core/SceneRenderCollectors.hpp>
#include <Engine/ECS/System/AnimationECSSystem.hpp>
#include <Engine/ECS/System/AttachmentSystem.hpp>
#include <Engine/ECS/System/AudioECSSystem.hpp>
#include <Engine/ECS/System/LevelSequenceSystem.hpp>
#include <Engine/ECS/System/LocomotionSystem.hpp>
#include <Engine/ECS/System/PhysicsECSSystem.hpp>
#include <Engine/ECS/System/ScriptSystem.hpp>
#include <Engine/ECS/System/GameModeSystem.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/VFX/VFXSystemLookup.hpp>

#include <array>
#include <format>

namespace Desert::Editor
{
    namespace
    {
        // A scene window's ImGui title: the name a person reads, then the "###<kind><id>" identity that keeps
        // two windows from merging and a closed window's imgui.ini entry from being inherited.
        std::string SceneWindowTitle( std::string_view name, std::string_view kind, std::uint64_t id )
        {
            return std::format( "{}###{}{}", name, kind, id );
        }
    } // namespace

    SceneWorkspace::SceneWorkspace( PanelRegistry& panels, const std::shared_ptr<Assets::AssetManager>& assets,
                                    const std::unique_ptr<Animation::AnimationLibrary>& animations )
         : m_Panels( panels ), m_Assets( assets ), m_Animations( animations )
    {
    }

    void SceneWorkspace::CreatePrimaryScene()
    {
        // The viewport panel is laid out on the first frame, after Scene::Init builds this renderer; the
        // panel's resize brings it to size then (see Graphic::kUnsizedViewExtent).
        m_SceneRenderer = std::make_unique<Graphic::SceneRenderer>( Graphic::kUnsizedViewExtent );
        m_ActiveScene   = std::make_shared<Desert::Core::Scene>( "New Scene", m_SceneRenderer.get() );
        m_PrimaryScene  = m_ActiveScene; // the always-present document #-1 (see SetActiveScene)
    }

    void SceneWorkspace::RebuildRenderRegistry()
    {
        m_RenderRegistry.reset();
        m_RenderRegistry = std::make_unique<Render::RenderRegistry>( m_ActiveScene );
    }

    void SceneWorkspace::ActiveSceneReplaced()
    {
        RebuildRenderRegistry();
        Core::SelectionManager::ClearSelection();
    }

    void SceneWorkspace::BuildSceneSystems( Desert::Core::Scene& scene )
    {
        // The nine collectors that turn components into render data, and their order, live in ONE place
        // (Engine/Core/SceneRenderCollectors.hpp). The gameplay systems below belong to the host: each needs a
        // service only the host owns.
        Desert::Core::AddSceneRenderCollectors( scene );
        scene.AddSystem<ECS::AnimationECSSystem>( m_Animations.get(), m_Assets.get() );
        // AttachmentSystem runs right AFTER animation: weapons-in-hand follow the freshly-posed bone this frame.
        scene.AddSystem<ECS::AttachmentSystem>( &scene );
        // ScriptSystem runs BEFORE physics: scripts set the character's move intent (+ look) which
        // PhysicsECSSystem then executes the same frame.
        scene.AddSystem<ECS::ScriptSystem>( &scene, m_Assets.get() );
        scene.AddSystem<ECS::GameModeSystem>( &scene, m_Assets.get() );
        scene.AddSystem<ECS::PhysicsECSSystem>( &scene );
        // Maps character movement state (speed/onGround from physics) -> locomotion clip; after physics.
        scene.AddSystem<ECS::LocomotionSystem>( &scene );
        scene.AddSystem<ECS::AudioECSSystem>( &scene );
        // Level sequences play last: a keyed Transform wins over this frame's physics and locomotion (UE
        // evaluates sequences after the actors' own tick).
        scene.AddSystem<ECS::LevelSequenceSystem>( &scene, m_Assets.get() );
        // A VFXComponent names its system by handle; the scene has no asset manager, the host does.
        scene.GetVFXWorld().SetSystemLookup( Desert::VFX::MakeAssetSystemLookup( *m_Assets ) );
    }

    void SceneWorkspace::SetActiveScene( uint64_t id )
    {
        if ( id == m_ActiveSceneId )
            return;

        const auto index = IndexOfSceneView(
             m_ExtraScenes, []( const std::unique_ptr<SceneDocument>& doc ) { return doc->Id; }, id );
        if ( id != kPrimarySceneViewId && !index )
        {
            // NAMED rather than ignored: an id that resolves to nothing means a viewport outlived its document.
            LOG_ERROR( "[Editor] Scene view #{} asked to become active but no such document is open — the "
                       "active scene is unchanged ('{}').",
                       id, m_ActiveScene ? m_ActiveScene->GetSceneName() : "<none>" );
            return;
        }

        m_ActiveSceneId = id;
        m_ActiveScene   = index ? m_ExtraScenes[*index]->Scene : m_PrimaryScene;

        // Structural undo/redo context + the scene-bound editing panels follow the active document.
        Commands::SetContext( m_ActiveScene.get(), m_Assets.get() );
        for ( auto& panel : m_Panels )
            panel->SetScene( m_ActiveScene );
        // Documents follow the active scene too; their owner is told here (a middle link must not drop it).
        if ( m_ActiveSceneListener )
            m_ActiveSceneListener( m_ActiveScene );

        // Selection is per-scene (entity UUIDs belong to one registry) — don't carry a stale one across.
        Core::SelectionManager::ClearSelection();

        LOG_INFO( "[Editor] Active scene -> '{}' (view #{})", m_ActiveScene->GetSceneName(), id );
    }

    // ── THE EDITOR'S OWN VIEW, AS SOMETHING THE CHANNEL CAN ADDRESS ────────────────────────────────────
    //
    // The scene's active camera, IF it is the editor's fly camera. Null in Play, where the view belongs to
    // the scene's own CameraComponent — and null is the honest answer there rather than a pinned override,
    // because "the editor camera" is not what is being looked through.
    Desert::Core::EditorCamera* SceneWorkspace::ActiveEditorCamera() const
    {
        if ( !m_ActiveScene )
            return nullptr;
        return dynamic_cast<Desert::Core::EditorCamera*>( m_ActiveScene->GetActiveCamera().get() );
    }

    bool SceneWorkspace::HasPendingRequests() const
    {
        return m_AddSceneViewRequested || m_AddSceneViewportRequested || m_ViewportGridRequested ||
               !m_PendingViewportGrid.empty();
    }

    void SceneWorkspace::ServiceRequests()
    {
        if ( m_AddSceneViewRequested )
        {
            m_AddSceneViewRequested = false;
            AddSceneView();
        }
        if ( m_AddSceneViewportRequested )
        {
            m_AddSceneViewportRequested = false;
            AddSceneViewport();
        }
        if ( m_ViewportGridRequested )
        {
            m_ViewportGridRequested = false;
            BuildViewportGrid();
        }
    }

    void SceneWorkspace::AddSceneView()
    {
        auto           doc = std::make_unique<SceneDocument>();
        const uint64_t id  = m_SceneViewIds.Next();
        doc->Id            = id;
        // Numbered by the id, not by the current count (a closed "Scene 3" must not reappear on a fourth view).
        doc->Name = std::format( "Scene {}", id + 1 ); // the main scene reads as "Scene 1"

        doc->Renderer = std::make_unique<Graphic::SceneRenderer>( Graphic::kUnsizedViewExtent );
        doc->Scene    = std::make_shared<Desert::Core::Scene>( std::string( doc->Name ), doc->Renderer.get() );
        BuildSceneSystems( *doc->Scene );
        if ( const auto inited = doc->Scene->Init(); !inited.IsSuccess() )
            LOG_ERROR( "[EditorLayer] scene view '{}' failed to initialise: {}", doc->Name, inited.GetError() );
        doc->Registry = std::make_unique<Render::RenderRegistry>( doc->Scene );

        const std::string title = SceneWindowTitle( doc->Name, "sceneview", id );
        auto              vp    = std::make_unique<Editor::ViewportPanel>( doc->Scene, m_Assets.get(), title, id );
        // Captures the ID, never the index. See Editor/Core/SceneViewIdentity.hpp.
        vp->SetOnActivate( [this, id] { SetActiveScene( id ); } );
        vp->GetVisibility() = true;
        doc->Viewport       = vp.get();
        m_Panels.Adopt( std::move( vp ) );

        m_ExtraScenes.emplace_back( std::move( doc ) );
        LOG_INFO( "[Editor] Opened scene view #{} (now {} scenes open; views: {})", id, m_ExtraScenes.size() + 1,
                  Graphic::SceneRenderer::DescribeLiveViews() );
    }

    std::vector<uint64_t> SceneWorkspace::DismissedSceneViews() const
    {
        std::vector<uint64_t> dismissed;
        for ( const auto& doc : m_ExtraScenes )
            if ( doc->Viewport != nullptr && !doc->Viewport->GetVisibility() )
                dismissed.push_back( doc->Id );
        return dismissed;
    }

    void SceneWorkspace::CloseSceneView( uint64_t id )
    {
        const auto index = IndexOfSceneView(
             m_ExtraScenes, []( const std::unique_ptr<SceneDocument>& doc ) { return doc->Id; }, id );
        if ( !index )
            return; // already closed — a second X on the same window in the same frame, or a stale request

        // Rebinding BEFORE the teardown, so no panel is holding the dying scene when its registry is destroyed.
        if ( const uint64_t next = ActiveSceneViewAfterClose( m_ActiveSceneId, id ); next != m_ActiveSceneId )
            SetActiveScene( next );

        auto&             doc  = m_ExtraScenes[*index];
        const std::string name = doc->Name;

        // EVERY EXTRA ANGLE ON THIS DOCUMENT GOES WITH IT — collected first, closed after.
        {
            std::vector<uint64_t> onThisDocument;
            for ( const auto& view : m_ExtraViewports )
                if ( view->Scene.lock() == doc->Scene )
                    onThisDocument.push_back( view->Id );
            for ( const uint64_t viewportId : onThisDocument )
                CloseSceneViewport( viewportId );
        }

        // ~PreviewViewport's order: idle the device; the PANEL; the registry; the scene; the renderer last —
        // whose destructor hands the renderer slot back.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        IPanel* panel = doc->Viewport;
        m_Panels.Remove( panel );
        doc->Viewport = nullptr;

        doc->Registry.reset();
        doc->Scene.reset();
        doc->Renderer.reset();
        m_ExtraScenes.erase( m_ExtraScenes.begin() + static_cast<ptrdiff_t>( *index ) );

        LOG_INFO( "[Editor] Closed scene view #{} '{}' ({} scenes open; views: {})", id, name,
                  m_ExtraScenes.size() + 1, Graphic::SceneRenderer::DescribeLiveViews() );
    }

    void SceneWorkspace::AddSceneViewport()
    {
        const auto scene = m_ActiveScene;
        if ( !scene )
        {
            LOG_ERROR( "[Editor] no active scene to open a second viewport on." );
            return;
        }

        auto       renderer  = std::make_unique<Graphic::SceneRenderer>( Graphic::kUnsizedViewExtent );
        const auto viewIndex = scene->AddView( renderer.get() );
        if ( !viewIndex )
        {
            // Scene::AddView has already said why; the renderer dies on the way out and gives its slot back.
            LOG_ERROR( "[Editor] '{}' refused a second viewport.", scene->GetSceneName() );
            return;
        }

        auto view   = std::make_unique<SceneViewport>();
        view->Id    = m_SceneViewIds.Next();
        view->Name  = std::format( "{} (view {})", scene->GetSceneName(), *viewIndex + 1 );
        view->Scene = scene;

        const std::string title = SceneWindowTitle( view->Name, "sceneviewport", view->Id );
        auto              vp =
             std::make_unique<Editor::ViewportPanel>( scene, m_Assets.get(), title, view->Id, renderer.get() );
        vp->GetVisibility() = true;
        view->Viewport      = vp.get();
        m_Panels.Adopt( std::move( vp ) );

        view->Renderer = std::move( renderer );
        m_ExtraViewports.emplace_back( std::move( view ) );

        LOG_INFO( "[Editor] Opened viewport #{} on '{}' ({} view(s) of that world; views: {})",
                  m_ExtraViewports.back()->Id, scene->GetSceneName(), scene->GetViewCount(),
                  Graphic::SceneRenderer::DescribeLiveViews() );
    }

    void SceneWorkspace::BuildViewportGrid()
    {
        const auto scene = m_ActiveScene;
        if ( !scene )
        {
            LOG_ERROR( "[Editor] no active scene to build a viewport grid on." );
            return;
        }

        // The MAIN viewport is always the perspective top-left pane; Top / Front / Right fill the rest.
        static constexpr std::array<ViewportCameraPreset, 3> kExtraAngles = {
             ViewportCameraPreset::Top, ViewportCameraPreset::Front, ViewportCameraPreset::Right };

        // REUSE WHAT IS ALREADY OPEN: a second run re-aims and re-docks rather than opening three more.
        std::vector<SceneViewport*> panes;
        for ( const auto& view : m_ExtraViewports )
            if ( view->Scene.lock() == scene && view->Viewport != nullptr && panes.size() < kExtraAngles.size() )
                panes.push_back( view.get() );

        while ( panes.size() < kExtraAngles.size() )
        {
            const size_t before = m_ExtraViewports.size();
            AddSceneViewport();
            if ( m_ExtraViewports.size() == before )
            {
                LOG_WARN( "[Editor] the viewport grid stops at {} pane(s): '{}' would not open another.",
                          panes.size() + 1, scene->GetSceneName() );
                break;
            }
            panes.push_back( m_ExtraViewports.back().get() );
        }

        // RAW panel names; the dockspace pass turns each into its displayed window title when it docks.
        m_PendingViewportGrid.clear();
        m_PendingViewportGrid.emplace_back( "Scene###scene" );
        if ( const auto aimed =
                  Editor::ViewportPanel::SetCameraPreset( kPrimarySceneViewId, ViewportCameraPreset::Perspective );
             !aimed )
        {
            LOG_WARN( "[Editor] the grid's perspective pane was not aimed: {}", aimed.GetError() );
        }

        for ( size_t i = 0; i < panes.size(); ++i )
        {
            m_PendingViewportGrid.push_back( panes[i]->Viewport->GetName() );
            if ( const auto aimed = Editor::ViewportPanel::SetCameraPreset( panes[i]->Id, kExtraAngles[i] );
                 !aimed )
            {
                LOG_WARN( "[Editor] grid pane '{}' was not aimed: {}", panes[i]->Name, aimed.GetError() );
            }
        }

        LOG_INFO( "[Editor] Viewport grid: {} pane(s) on '{}' (views: {}); the dock split runs on the "
                  "next frame.",
                  m_PendingViewportGrid.size(), scene->GetSceneName(),
                  Graphic::SceneRenderer::DescribeLiveViews() );
    }

    void SceneWorkspace::CloseDismissedSceneViewports()
    {
        std::vector<uint64_t> dismissed;
        for ( const auto& view : m_ExtraViewports )
            if ( view->Viewport != nullptr && !view->Viewport->GetVisibility() )
                dismissed.push_back( view->Id );

        for ( const uint64_t id : dismissed )
            CloseSceneViewport( id );
    }

    void SceneWorkspace::CloseSceneViewport( uint64_t id )
    {
        const auto index = IndexOfSceneView(
             m_ExtraViewports, []( const std::unique_ptr<SceneViewport>& view ) { return view->Id; }, id );
        if ( !index )
            return; // already closed

        auto&             view = m_ExtraViewports[*index];
        const std::string name = view->Name;

        // Same order as CloseSceneView, minus the scene.
        Graphic::Renderer::GetInstance().WaitDeviceIdle();

        m_Panels.Remove( view->Viewport );
        view->Viewport = nullptr;

        if ( const auto scene = view->Scene.lock() )
        {
            if ( !scene->RemoveView( view->Renderer.get() ) )
                LOG_WARN( "[Editor] viewport '{}' was not a view of '{}' when it closed.", name,
                          scene->GetSceneName() );
        }
        view->Renderer.reset();
        m_ExtraViewports.erase( m_ExtraViewports.begin() + static_cast<ptrdiff_t>( *index ) );

        LOG_INFO( "[Editor] Closed viewport '{}' (views: {})", name, Graphic::SceneRenderer::DescribeLiveViews() );
    }

    void SceneWorkspace::Teardown()
    {
        // A SCENE NAMES ITS VIEWS' RENDERERS BY RAW POINTER AND OWNS NONE OF THEM, so a renderer may only die
        // after its scene has let go of it (FIX6). The panels went with PanelRegistry::Clear() before this.
        for ( auto& view : m_ExtraViewports )
        {
            view->Viewport = nullptr;
            if ( const auto scene = view->Scene.lock() )
            {
                if ( !scene->RemoveView( view->Renderer.get() ) )
                    LOG_WARN( "[Editor] viewport '{}' was not a view of '{}' at shutdown.", view->Name,
                              scene->GetSceneName() );
            }
            view->Renderer.reset();
        }
        m_ExtraViewports.clear();

        // The primary document: the pass registry while scene and renderer live, both scene handles, renderer.
        m_RenderRegistry.reset();
        m_ActiveScene.reset();
        m_PrimaryScene.reset();
        m_SceneRenderer.reset();

        for ( auto& doc : m_ExtraScenes )
        {
            doc->Viewport = nullptr;
            doc->Registry.reset();
            doc->Scene.reset();
            doc->Renderer.reset();
        }
        m_ExtraScenes.clear();
    }

    void SceneWorkspace::AppendNewViewCommands( std::vector<PaletteCommand>& commands )
    {
        // A SECOND LIVE SCENE, for the same reason the levels above are here: the channel's vocabulary IS
        // this list, and "New Scene View" was reachable only from Window -> Viewports. That made the one
        // configuration where a renderer can bleed into another renderer — two SceneRenderers recording in
        // one frame against the same shared materials — the one configuration nothing could verify
        // unattended. Г14 needed exactly that check.
        //
        // Sets the same deferred flag the menu item does rather than opening it here: it allocates a
        // renderer slot and GPU resources, which must not happen inside the ImGui pass.
        commands.push_back( { "Scene", "New Scene View", [this]
                              {
                                  RequestAddSceneView();
                                  return PaletteCommandDone();
                              } } );
        // A SECOND ANGLE ON THE SAME WORLD (not a second document).
        commands.push_back( { "Scene", "New Viewport (same scene)", [this]
                              {
                                  RequestAddSceneViewport();
                                  return PaletteCommandDone();
                              } } );
    }

    void SceneWorkspace::AppendViewLayoutCommands( std::vector<PaletteCommand>& commands )
    {
        commands.push_back( { "Scene", "Four-Up Viewports", [this]
                              {
                                  RequestViewportGrid();
                                  return PaletteCommandDone();
                              } } );

        for ( const ViewportCameraPresetRow& preset : kViewportCameraPresets )
        {
            const ViewportCameraPreset p = preset.Preset;
            commands.push_back( { "Scene", std::string( "Viewport Camera: " ) + preset.Name,
                                  [p]() -> Common::BoolResultStr
                                  { return Editor::ViewportPanel::RequestCameraPreset( p ); } } );
        }

        // Closes HIDE the window (the X's route); the teardown runs between frames. Ids captured, never
        // pointers or indices — see Editor/Core/SceneViewIdentity.hpp.
        for ( const auto& view : m_ExtraViewports )
        {
            const uint64_t id = view->Id;
            commands.push_back( { "Scene", std::format( "Close Viewport {}", view->Name ),
                                  [this, id]() -> Common::BoolResultStr
                                  {
                                      const auto index = IndexOfSceneView(
                                           m_ExtraViewports,
                                           []( const std::unique_ptr<SceneViewport>& v ) { return v->Id; }, id );
                                      if ( !index || m_ExtraViewports[*index]->Viewport == nullptr )
                                      {
                                          return Common::MakeFormattedError<bool>(
                                               "viewport #{} is already closed; nothing to close.", id );
                                      }
                                      m_ExtraViewports[*index]->Viewport->GetVisibility() = false;
                                      return PaletteCommandDone();
                                  } } );
        }

        for ( const auto& doc : m_ExtraScenes )
        {
            const uint64_t id = doc->Id;
            commands.push_back(
                 { "Scene", std::format( "Close Scene View {}", doc->Name ), [this, id]() -> Common::BoolResultStr
                   {
                       const auto index = IndexOfSceneView(
                            m_ExtraScenes, []( const std::unique_ptr<SceneDocument>& d ) { return d->Id; }, id );
                       if ( !index || m_ExtraScenes[*index]->Viewport == nullptr )
                       {
                           return Common::MakeFormattedError<bool>(
                                "scene view #{} is already closed; nothing to close.", id );
                       }
                       m_ExtraScenes[*index]->Viewport->GetVisibility() = false;
                       return PaletteCommandDone();
                   } } );
        }
    }
} // namespace Desert::Editor
