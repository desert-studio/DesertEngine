#pragma once

// THE OPEN WORLDS OF THE LEVEL EDITOR (UE: SLevelViewport / LevelViewportLayout plus the editor world).
//
// Owns the primary scene and its renderer and pass registry, every extra scene DOCUMENT (New Scene View) with
// its own renderer and registry, every extra VIEWPORT onto an open document (New Viewport, Four-Up), and which
// document the editor is bound to. A member of EditorLayer BY VALUE, declared before m_Panels; its
// collaborators arrive by reference in the constructor — no singleton, no pointer back to EditorLayer.
//
// Requests from the menu, the palette and the control channel are VERBS (RequestX); OnUpdate services them
// between frames, because opening and closing a view allocates or destroys GPU resources, which is not legal
// inside the ImGui pass that noticed the click.

#include <Engine/Desert.hpp>
#include "Editor/Core/CommandPalette.hpp"
#include "Editor/Core/SceneViewIdentity.hpp"
#include "Editor/RenderSystems/RenderRigistry.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Desert::Core
{
    class EditorCamera;
}

namespace Desert::Graphic::System
{
    class DisplacementCache;
}

namespace Desert::Editor
{
    class PanelRegistry;
    class ViewportPanel;
    class PlaySession;

    class SceneWorkspace
    {
    public:
        // UE's FEditorDelegates::MapChange in miniature: told after the panels have been rebound and before the
        // selection is cleared, so the documents (which the workspace does not own) follow the active scene.
        using ActiveSceneListener = std::function<void( const std::shared_ptr<Desert::Core::Scene>& )>;

        // An extra scene opened alongside the primary one. See Editor/Core/SceneViewIdentity.hpp for why the
        // id, never the position, names it.
        struct SceneDocument
        {
            uint64_t                                Id = kPrimarySceneViewId;
            std::string                             Name;
            std::shared_ptr<Desert::Core::Scene>    Scene;
            std::unique_ptr<Graphic::SceneRenderer> Renderer;
            std::unique_ptr<Render::RenderRegistry> Registry;
            ViewportPanel*                          Viewport = nullptr; // non-owning; lives in PanelRegistry
        };

        // A SECOND ANGLE, not a second document: no Scene and no RenderRegistry of its own.
        struct SceneViewport
        {
            uint64_t    Id = kPrimarySceneViewId;
            std::string Name;
            // WEAK: the document this looks at can be closed while this viewport is open.
            std::weak_ptr<Desert::Core::Scene>      Scene;
            std::unique_ptr<Graphic::SceneRenderer> Renderer;
            ViewportPanel*                          Viewport = nullptr; // non-owning; lives in PanelRegistry
        };

        SceneWorkspace( PanelRegistry& panels, const std::shared_ptr<Assets::AssetManager>& assets,
                        const std::unique_ptr<Animation::AnimationLibrary>& animations );

        // ── the primary document ───────────────────────────────────────────────────────────────────
        void CreatePrimaryScene(); // the renderer and the always-present document #-1 (ctor, once)
        [[nodiscard]] const std::shared_ptr<Desert::Core::Scene>& ActiveScene() const
        {
            return m_ActiveScene;
        }
        // The active view IF it is the editor's fly camera; null in Play, where the scene's own
        // CameraComponent drives (the channel's `viewport` subject, `--camera`/`--look`, the entity palette).
        [[nodiscard]] Desert::Core::EditorCamera*                 ActiveEditorCamera() const;
        [[nodiscard]] const std::shared_ptr<Desert::Core::Scene>& PrimaryScene() const
        {
            return m_PrimaryScene;
        }
        [[nodiscard]] uint64_t ActiveSceneId() const
        {
            return m_ActiveSceneId;
        }
        [[nodiscard]] Render::RenderRegistry* PrimaryRegistry() const
        {
            return m_RenderRegistry.get();
        }
        [[nodiscard]] Graphic::SceneRenderer* PrimaryRenderer() const
        {
            return m_SceneRenderer.get();
        }
        // Old registry FIRST: its destructor unregisters the editor passes by name, and an assignment would run it
        // after the new registry had already registered them again.
        // THE WORLDS' FRAME (UE: UEditorEngine::Tick, the loop over the WorldContexts): the primary scene, then
        // every document, each a full independent render. @p shotCounting is this frame's ShotRecordGate verdict.
        [[nodiscard]] Common::BoolResultStr TickWorlds( const Common::Timestep& ts, PlaySession& play,
                                                        bool shotCounting );
        // The worlds inside UI render-texture elements, of the primary scene and of every document, recorded
        // before any pass of the frame opens (Vulkan has no nested render pass).
        void TickRenderTextures( const Common::Timestep& ts );
        void RebuildRenderRegistry();
        // THE FRAME'S OWN CENSUS: the four after-the-frame entries of the control gate (ControlService::
        // OnFramePresented). The views decide them while they record, on the render task, so they are read in the
        // frame's own order (EditorLayer's job "Editor: frame census", OVERLAP-ON-5), never at the announcement
        // while the next frame's views may be writing them.
        struct FrameCensus
        {
            // The active world's displaced primitives still on their base mesh (DisplacementStats::Unsettled).
            bool DisplacementUnsettled = false;
            // Any view's subsurface scatter still averaging its history (SceneRenderer::
            // SubsurfaceHistoryConverging): PendingWork::SubsurfaceHistory.
            bool SubsurfaceHistoryConverging = false;
            // Any view drew a material's default-surface stand-in while its own pipeline compiled
            // (SceneRenderer::GetStandInDraws): PendingWork::MaterialPipelines.
            bool MaterialPipelinesCompiling = false;
            // Any view drew grass whose instance buffers may be truncated (SceneRenderer::GrassInstancesSettling,
            // a camera cut): PendingWork::GrassInstances.
            bool GrassInstancesSettling = false;
        };
        // What the census reads, handed over BY VALUE when the frame is captured, on the game thread: the
        // renderers of the primary scene, of every document and of every extra angle (in that order), and the
        // active world's displacement cache. The workspace's containers are never walked off this thread. Raw
        // pointers: a renderer or a world is destroyed only behind the render task's fence (~SceneRenderer joins;
        // RenderTask's FENCE), so a job enqueued in this frame has finished before either goes.
        struct FrameCensusSources
        {
            std::vector<const Graphic::SceneRenderer*> Renderers;
            const Graphic::System::DisplacementCache*  Displacement = nullptr;
        };
        [[nodiscard]] FrameCensusSources CaptureFrameCensusSources() const;
        // ANY THREAD that orders after the views it reads: the render task in the frame's order, or the game
        // thread when nothing is in flight (serial mode, the overlap off).
        [[nodiscard]] static FrameCensus ReadFrameCensus( const FrameCensusSources& sources );

        // THE ACTIVE SCENE'S WORLD WAS REPLACED (Open, New, Stop's snapshot restore): its entities were torn down
        // and rebuilt. Everything keyed by the old entities follows the world, in ONE place for all three paths:
        // the render registry is rebuilt and the selection is emptied — as UE's USelection is on a world change.
        // A UUID that survives the swap (a template opened twice, a snapshot restore) names an entity of a
        // different world; keeping it would leave Outliner "1 selected" and Details "Entity not found". Stop is
        // the one caller that re-selects afterwards: its snapshot is the authored world the selection came from
        // (PlaySession::Stop, as UE's EndPlayMap selects the PIE selection's editor counterparts).
        void ActiveSceneReplaced();

        // THE ACTIVE SCENE'S OLD WORLD IS GONE (its entities cleared, Scene::Clear): every view of it releases
        // what it built from that world (SceneRenderer::ReleaseWorldResources), the device is idled and the
        // deferred deletions are drained - UE's world teardown (GC + FlushRenderingCommands) before the next world
        // loads. Called by the opener between Clear and the load; without it the old world's device memory was
        // still resident, or owed deletion, at the next world's peak (FIELD-MEM p3: a reopen peaked 1080 MB
        // higher).
        void ReleaseActiveWorld();
        /// The device-memory window of one scene open (VulkanAllocator::BeginSceneOpen): starts it at the top
        /// of an open, and samples the footprint at the end of the load, which runs without frames.
        void BeginSceneOpenMeasure( std::string_view scene );
        void SampleSceneOpenMeasure();

        // The standard ECS systems, shared by the primary scene and every extra document.
        void BuildSceneSystems( Desert::Core::Scene& scene );

        // ── which document the editor is bound to ──────────────────────────────────────────────────
        void SetActiveScene( uint64_t id );
        void OnActiveSceneChanged( ActiveSceneListener listener )
        {
            m_ActiveSceneListener = std::move( listener );
        }

        // ── requests (menu, palette, control channel) and their service between frames ─────────────
        void RequestAddSceneView()
        {
            m_AddSceneViewRequested = true;
        }
        void RequestAddSceneViewport()
        {
            m_AddSceneViewportRequested = true;
        }
        void RequestViewportGrid()
        {
            m_ViewportGridRequested = true;
        }
        [[nodiscard]] bool HasPendingRequests() const;
        void               ServiceRequests();

        // ── closing ────────────────────────────────────────────────────────────────────────────────
        // The ids of the documents whose window the user dismissed. The CLOSE is the coordinator's: a playing
        // document ends Play first (UE closes a level after EndPlayMap), then CloseSceneView below.
        [[nodiscard]] std::vector<uint64_t> DismissedSceneViews() const;
        void                                CloseSceneView( uint64_t id );
        void                                CloseDismissedSceneViewports();
        void                                CloseSceneViewport( uint64_t id );
        // OnDetach's half: every renderer after the scene that names it, in the order the close sites use.
        void Teardown();

        // ── the four-up grid's window names, drained by the dockspace pass on the next frame ───────
        [[nodiscard]] const std::vector<std::string>& PendingViewportGrid() const
        {
            return m_PendingViewportGrid;
        }
        void ClearPendingViewportGrid()
        {
            m_PendingViewportGrid.clear();
        }

        [[nodiscard]] const std::vector<std::unique_ptr<SceneDocument>>& Documents() const
        {
            return m_ExtraScenes;
        }
        [[nodiscard]] const std::vector<std::unique_ptr<SceneViewport>>& Viewports() const
        {
            return m_ExtraViewports;
        }

        // Palette providers, registered in the palette's existing group order (see EditorLayer::OnAttach).
        void AppendNewViewCommands( std::vector<PaletteCommand>& commands );    // New Scene View, New Viewport
        void AppendViewLayoutCommands( std::vector<PaletteCommand>& commands ); // Four-Up, presets, closes

    private:
        static Common::BoolResultStr TickWorld( Desert::Core::Scene& scene, Render::RenderRegistry* registry,
                                                const Common::Timestep& ts, PlaySession& play, bool shotCounting );
        // EditorPreferences::CameraSpeed reaches the editor camera once, on the first tick: Scene::Init makes the
        // camera and the first Init is deferred to the first scene load (SceneInitDeferral).
        void ApplyCameraSpeedOnce();
        void AddSceneView();
        void AddSceneViewport();
        void BuildViewportGrid();

        PanelRegistry&                                      m_Panels;
        const std::shared_ptr<Assets::AssetManager>&        m_Assets;
        const std::unique_ptr<Animation::AnimationLibrary>& m_Animations;
        ActiveSceneListener                                 m_ActiveSceneListener;

        // m_ActiveScene is the ACTIVE document — rebound to the focused viewport's scene; m_PrimaryScene keeps
        // the original (document #-1) so it can be rebound back to.
        std::unique_ptr<Graphic::SceneRenderer>     m_SceneRenderer;
        std::shared_ptr<Desert::Core::Scene>        m_ActiveScene;
        std::shared_ptr<Desert::Core::Scene>        m_PrimaryScene;
        std::unique_ptr<Render::RenderRegistry>     m_RenderRegistry;
        std::vector<std::unique_ptr<SceneDocument>> m_ExtraScenes;
        std::vector<std::unique_ptr<SceneViewport>> m_ExtraViewports;
        // ONE id source for documents AND viewports: they share the ImGui window-id and authoring-owner spaces.
        SceneViewIdSource        m_SceneViewIds;
        uint64_t                 m_ActiveSceneId = kPrimarySceneViewId;
        std::vector<std::string> m_PendingViewportGrid; // raw panel names, grid order TL, TR, BL, BR

        bool m_AddSceneViewRequested     = false;
        bool m_AddSceneViewportRequested = false;
        bool m_ViewportGridRequested     = false;
        bool m_CameraSpeedApplied        = false;
    };
} // namespace Desert::Editor
