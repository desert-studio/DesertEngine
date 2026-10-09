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
#include <vector>

namespace Desert::Core
{
    class EditorCamera;
}

namespace Desert::Editor
{
    class PanelRegistry;
    class ViewportPanel;

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
        void RebuildRenderRegistry();

        // THE ACTIVE SCENE'S WORLD WAS REPLACED (Open, New, Stop's snapshot restore): its entities were torn down
        // and rebuilt. Everything keyed by the old entities follows the world, in ONE place for all three paths:
        // the render registry is rebuilt and the selection is emptied — as UE's USelection is on a world change.
        // A UUID that survives the swap (a template opened twice, a snapshot restore) names an entity of a
        // different world; keeping it would leave Outliner "1 selected" and Details "Entity not found". Stop is
        // the one caller that re-selects afterwards: its snapshot is the authored world the selection came from
        // (PlaySession::Stop, as UE's EndPlayMap selects the PIE selection's editor counterparts).
        void ActiveSceneReplaced();

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
    };
} // namespace Desert::Editor
