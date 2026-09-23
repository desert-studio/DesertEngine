#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <Engine/Desert.hpp>

#include "Editor/Core/SceneViewIdentity.hpp"
#include "Editor/Core/Selection/AuthoringContext.hpp"
#include "Editor/Core/ViewportModes.hpp"
#include "Editor/Panels/IPanel.hpp"

#include "Editor/Widgets/UIHelper/ImGuiUI.hpp"

#include "LightGizmoRenderer.hpp"
#include "ViewportCameraPreset.hpp"
#include "PerfHudOverlay.hpp"
#include "Tools/FoliagePaintTool.hpp"
#include "Tools/CubeGridTool.hpp"
#include "Tools/PolyEditTool.hpp"
#include "Tools/TerrainPaintTool.hpp"
#include "Tools/GizmoController.hpp"
#include "Tools/PickingController.hpp"

namespace Desert::Editor
{
    class AsyncMeshLoader; // async cook of dropped meshes (defined in Import/AsyncMeshLoader.hpp)

    // Which handle of a selected UI element is being dragged in the viewport (in-scene UI editing).
    // Body = move; the rest are the 8 resize handles (corners + edge midpoints).
    enum class UIHandle
    {
        None,
        Body,
        L,
        R,
        T,
        B,
        TL,
        TR,
        BL,
        BR,
        AnchorMin, // draggable anchor markers on the parent rect (re-anchor without moving the element)
        AnchorMax
    };

    class ViewportPanel : public IPanel, public Common::EventHandler
    {
    public:
        // `title` is the ImGui window title/id. Multi-scene editing spawns extra viewports, so each needs
        // its own unique "###id" (two windows sharing one id merge into a single dockable window).
        //
        // `sceneViewId` is this view's name in SceneViewIdentity's sense, and it is here because the
        // viewport is one of the three surfaces that can OWN the bone-authoring context
        // (Editor/Core/Selection/AuthoringContext.hpp): several viewports exist, they must be told apart
        // when one of them writes the context, and an index would name the wrong one the moment a view is
        // closed — the whole argument SceneViewIdentity.hpp makes.
        //
        // @p viewRenderer is WHICH ANGLE of @p scene this panel shows, identified by the renderer that
        // records it rather than by an index into Scene's view list — an index names a different view the
        // moment an earlier one is closed, which is the same argument SceneViewIdentity.hpp makes about
        // documents. Null means view 0, the angle every single-viewport scene has.
        ViewportPanel( const std::shared_ptr<Desert::Core::Scene>& scene,
                       const Assets::AssetManager* assetManager = nullptr, std::string title = "Scene###scene",
                       uint64_t                sceneViewId  = kPrimarySceneViewId,
                       Graphic::SceneRenderer* viewRenderer = nullptr );

        // WHICH VIEW OF THE SCENE THIS PANEL DRAWS. Scene::GetViewCount() when this panel's view has been
        // closed under it, which every accessor below reads as "no such view" and nothing reads as 0.
        [[nodiscard]] size_t ViewIndex() const;
        // This view's camera. Empty when the view is gone — the panel then says "Camera was not found"
        // instead of quietly showing another angle's picture.
        [[nodiscard]] std::shared_ptr<::Desert::Core::Camera> ViewCamera() const;
        ~ViewportPanel() override; // defined in the .cpp (unique_ptr<AsyncMeshLoader> needs the complete type)

        // A SELF-REGISTERING TYPE MUST NOT BE COPYABLE OR MOVABLE. The constructor pushes `this` into
        // s_Live and the destructor erases by pointer value, so a copy would never register yet would
        // take the ORIGINAL's entry out on destruction, and a moved-from object would leave a live entry
        // naming a shell. Nothing in the tree copies one today — every panel is std::make_unique'd —
        // which is exactly why the compiler should be what keeps it that way rather than habit. A8-2.
        ViewportPanel( const ViewportPanel& )            = delete;
        ViewportPanel& operator=( const ViewportPanel& ) = delete;
        ViewportPanel( ViewportPanel&& )                 = delete;
        ViewportPanel& operator=( ViewportPanel&& )      = delete;
        void OnUIRender() override;

        // A SECOND VIEWPORT MUST OPEN BIG ENOUGH TO BE A VIEWPORT. Without this the base class's (0,0)
        // lets ImGui size the window to its content, and a viewport's content is an image sized from the
        // space the window gives it — so the first frame has nothing to measure and the window opens as a
        // ~330x50 stub showing its toolbar and no picture. Measured: the first "New Viewport (same scene)"
        // came up exactly that size. The primary viewport is unaffected because it is docked from
        // imgui.ini and FirstUseEver never fights a saved layout.
        [[nodiscard]] glm::vec2 GetDefaultSize() const override
        {
            return { 960.0f, 640.0f };
        }

        // The scene image must reach the window edges — any padding would frame it with dead pixels.
        [[nodiscard]] glm::vec2 GetWindowPadding() const override
        {
            return { 0.0f, 0.0f };
        }
        void OnPreUpdate() override;

        void OnEvent( Common::Event& e ) override;

        // THE VIEW `scene`'s RENDERER MUST BE GIVEN THIS FRAME: the user's persisted answer (`user`,
        // which is EditorPreferences::DebugView), minus whatever the viewports looking at that scene are
        // hiding right now. Called from the one place the editor pushes a debug view down,
        // EditorLayer::UpdateSceneFrame, so a mode's suppression exists only in the copy that reaches the
        // renderer and can never be written to ~/.desertengine/editor.json. Editor/Core/ViewportModes.hpp
        // carries the argument for why that separation is the fix and a fence around each Save() is not.
        //
        // IT IS KEYED ON THE SCENE, NOT GLOBAL, and that closes a second defect of the old code: the
        // suppression used to be a single write into the one shared preference struct, so putting ONE
        // viewport into 2D mode blanked the grid in every other viewport as well. A scene has one
        // SceneRenderer, so per-scene is the finest granularity that exists here; two viewports on one
        // scene still share an answer, and any of them being in 2D mode hides the grid for that scene.
        static Graphic::DebugViewState EffectiveDebugView( const Graphic::DebugViewState& user,
                                                           const Desert::Core::Scene&     scene );

        // Flip 2D UI mode on every viewport showing `scene`, from outside the panel — the command palette's
        // "Toggle 2D UI mode". It exists for the same reason К6 gave the snap steps and the Perf HUD names:
        // a mode that can only be reached by clicking a checkbox is a mode no unattended run can enter, so
        // the scenario it is part of cannot be photographed and therefore cannot be proved. It writes
        // nothing and saves nothing, exactly like the checkbox.
        static void ToggleUIMode( const Desert::Core::Scene& scene );

        // ── 07 §14.2's MODE SWITCHER, FROM OUTSIDE THE PANEL ──────────────────────────────────────
        //
        // The palette's "Viewport mode: ..." entries, and therefore the control channel's. Same argument
        // as ToggleUIMode above and one more: the four-way switcher's ONLY other door is a mouse click on
        // a toolbar segment, and synthetic input is closed on this machine — so without this the modes
        // could never be photographed and a switcher nobody can see working is a switcher nobody can
        // check. It replaces the palette's old "Toggle the control rig overlay", which reached three
        // process-wide statics directly and could name no owner.
        //
        // REFUSES WITH A REASON rather than doing nothing: "there is no viewport", or the selected entity
        // cannot be authored that way (`ModeAvailability`). Over the channel a silent success is
        // indistinguishable from a mode that was entered, which is the one thing a caller must not get
        // wrong.
        NO_DISCARD static Common::BoolResultStr RequestAuthoringMode( Core::AuthoringMode mode );

        // ── NAMED CAMERA ANGLES ───────────────────────────────────────────────────────────────────
        //
        // The half of UE's four-up pattern that carries the meaning; ViewportCameraPreset.hpp argues
        // why the other half (the fixed splitter widget) is refused in favour of docking.
        //
        // TWO ENTRY POINTS BECAUSE THERE ARE TWO QUESTIONS, and one implementation behind them. The
        // toolbar and the palette mean "the viewport I am working in"; the grid builder means "the
        // third pane", which it knows by scene-view id and could not name any other way — an index
        // into the live list is a different window the moment one closes (SceneViewIdentity.hpp).
        NO_DISCARD static Common::BoolResultStr RequestCameraPreset( ViewportCameraPreset preset );
        NO_DISCARD static Common::BoolResultStr SetCameraPreset( uint64_t sceneViewId, ViewportCameraPreset preset );

        // CAN THE SELECTED ENTITY BE AUTHORED THIS WAY, and if not, the sentence that says why. An error
        // rather than a bool because every caller shows the reason: the toolbar as the disabled segment's
        // tooltip, the palette as the refusal on the wire.
        NO_DISCARD Common::BoolResultStr ModeAvailability( Core::AuthoringMode mode ) const;

        // Called (once, while this viewport window has ImGui focus) so the editor can make this viewport's
        // scene the active one — the Outliner/Details/gizmo then follow whichever viewport you work in.
        void SetOnActivate( std::function<void()> cb )
        {
            m_OnActivate = std::move( cb );
        }

    private:
        // THE VIEWPORT THE USER IS WORKING IN: the one holding the bone-authoring context, else the
        // first live one. Named once, because picking "the first" blindly moves a command off the view
        // the user is in the moment a second viewport exists — and several viewports at once is a
        // shipped feature here, not a corner case. Null only when no viewport exists at all.
        static ViewportPanel* ActiveViewport();

        // Aim THIS viewport's camera. Refuses with a reason when the view has no editor camera — a
        // closed view, or Play mode, where the camera is the scene's and not the user's to orbit.
        NO_DISCARD Common::BoolResultStr ApplyCameraPreset( ViewportCameraPreset preset );

        bool OnMousePressed( Common::MouseButtonPressedEvent& e );
        bool OnKeyPressedEvent( Common::KeyPressedEvent& e );

    private:
        // Viewport data access
        const glm::vec2& GetSize() const
        {
            return m_ViewportData.Size;
        }
        bool IsHovered() const
        {
            return m_ViewportData.IsHovered;
        }

    private:
        // On spawning a mesh, auto-assign a "sidecar" material so packs come with their look: looks for
        // <stem>.demat next to the mesh, else any *.demat in the mesh's folder, else in the parent folder
        // (the collection root). Assigns it to every material slot. No-op if none found.
        void ApplySidecarMaterial( ECS::Entity& entity, const std::string& meshSourcePath );

        // Viewport material DnD: raycast the mesh under the cursor and assign the dropped .demat
        // to its material elements (all of them — the hit carries no submesh id yet).
        void AssignMaterialAtCursor( const std::string& materialPath );

        // Godot-style toolbar row ABOVE the image: mode, transform tools, snap, contextual
        // skeleton toggle, camera gear (right). Replaces the old floating in-viewport overlay.
        void DrawViewportToolbar();

        // Corner XYZ orientation gizmo (a small triad tracking the camera's rotation) so you always know
        // which way world X/Y/Z point in the current view. Overlay only — pure ImGui, no scene interaction.
        void DrawViewAxisGizmo( const glm::vec2& viewportPos, const glm::vec2& viewportSize );

        // Draws the active scene's UICanvas over the viewport + (for a selected UI element) a selection marquee
        // and 8 drag/resize handles, and applies mouse drag to its UILayout offsets. In-scene UI editing.
        void DrawUIInScene();

    private:
        std::pair<float, float> GetMouseViewportSpace() const;

        struct ViewportData
        {
            glm::vec2 MousePosition;
            glm::vec2 Size;
            glm::vec2 ViewportPos;
            bool      IsHovered = false;
            float     DpiScale  = 1.0f;
        };

        ViewportData m_ViewportData;

        // True while the cursor is over the corner view-axis gizmo — set in DrawViewAxisGizmo, read in
        // OnMousePressed to suppress scene picking (a click there snaps the camera, it doesn't select).
        bool m_ViewAxisGizmoHovered = false;

        // WHAT THIS ONE VIEWPORT IS DOING — not what the user has chosen to see. 2D UI-editing mode
        // (toolbar "2D") hides the grid and the orientation triad so a screen-space canvas reads like a UI
        // designer, and it does so by SUPPRESSING them on the way to the renderer: there is deliberately
        // no second copy of the user's grid preference here to restore from, because a stashed copy is
        // what made a mode's suppression reachable by every EditorPreferences::Save() in the editor
        // (К10 — see Editor/Core/ViewportModes.hpp). Toggling this writes nothing and saves nothing.
        Core::ViewportModes m_Modes;
        bool                m_UIPreview = false; // Design (drag/select) <-> Preview (buttons interactive)

        // Every ViewportPanel alive right now, so EffectiveDebugView can ask "is anybody looking at this
        // scene in 2D mode?" without the push site having to know about panels. Registered by the
        // constructor and removed by the destructor, so a closed scene view stops suppressing the moment
        // it stops existing — which a flag hanging off the scene or off the preferences could not manage.
        static std::vector<ViewportPanel*> s_Live;

        // In-scene UI drag/resize state. Offsets are captured at drag start so the drag is absolute (no drift).
        UIHandle  m_UIDrag = UIHandle::None;
        glm::vec2 m_UIDragStartMouse{};
        glm::vec2 m_UIDragStartOffMin{};
        glm::vec2 m_UIDragStartOffMax{};
        glm::vec4 m_UIDragStartRect{}; // element screen edges (left,top,right,bottom) at anchor-drag start

        // Resize is deferred from OnUIRender (within the recording window) to OnPreUpdate
        // (start of next frame, before any rendering) to avoid destroying descriptor set pools
        // while they are bound to a recording command buffer.
        std::optional<glm::vec2> m_PendingViewportSize;

        std::shared_ptr<Desert::Core::Scene>  m_Scene;

        // ── THIS VIEW'S BONE AUTHORING ────────────────────────────────────────────────────────────
        //
        // The state lives here, in the surface that owns it, and is PUBLISHED to
        // Core::ActiveAuthoringContext() while the user is working in this viewport. That is what
        // replaced Core::SkeletonEditMode's four process-wide statics: they were one copy for the whole
        // editor, so a second viewport and a second Sequencer shared one selected bone and one pose-mode
        // bit and overwrote each other every frame.
        uint64_t               m_SceneViewId = kPrimarySceneViewId;
        // Non-owning: EditorLayer owns the renderer and destroys it after this panel. Null for the
        // primary viewport, which is always view 0.
        Graphic::SceneRenderer* m_ViewRenderer = nullptr;
        Core::AuthoringContext m_Authoring;

        // Publish this view's context. Points it at whatever is selected HERE first — the context is keyed
        // on the entity, and a bone index kept across a change of character is an index into another rig.
        void ClaimAuthoringContext();

        // 07 §14.2: Object / Skeleton / Pose / Control as ONE control, replacing the "Skeleton" toggle
        // that used to sit here. Drawn from `kAuthoringModes` so a mode cannot be added to the enum and
        // forgotten on the strip.
        void DrawAuthoringModeSwitch();

        // ── WHERE THE TOOLBAR'S RIGHT-HAND CLUSTER BEGINS ────────────────────────────────────────
        //
        // The show-flags eye, the view-mode combo and the camera gear are PINNED to the right edge with
        // absolute `SameLine( x )` calls, so the left-hand run of the strip does not end at the panel's
        // edge — it ends here, and anything drawn past it is painted UNDER them. Measured, not feared:
        // §14.2's fourth segment landed beneath the eye button at the editor's default layout, drawn and
        // highlighted and unclickable, which is the worst of the three possible outcomes because it looks
        // like a rendering glitch rather than a missing control.
        //
        // One derivation, read by both sides. Two copies of this arithmetic is exactly how it happened.
        NO_DISCARD static float ToolbarGearX();
        NO_DISCARD static float ToolbarViewModeX();
        NO_DISCARD static float ToolbarRightClusterX();

        // Take the context and put this viewport into @p mode, saying so if it is refused.
        void EnterAuthoringMode( Core::AuthoringMode mode );

        // The ordinary, per-frame route: the user is working in this viewport, or nobody has claimed bone
        // authoring at all (the editor's cold start, and what makes the toolbar work before anything else
        // has ever been focused).
        void TakeAuthoringContextIfFocused();

        // WHO THIS VIEWPORT IS when it writes the context. Built once from the id, because the owner is
        // compared by value on every write and a value rebuilt per call would be two owners that merely
        // look alike.
        const Core::AuthoringOwner m_AuthoringOwner;

        std::function<void()>                 m_OnActivate; // fired while this viewport window is focused
        const Assets::AssetManager*           m_AssetManager = nullptr; // for prefab drag-drop instantiate
        std::unique_ptr<Editor::UI::UIHelper> m_UIHelper;
        std::unique_ptr<LightGizmoRenderer>   m_LightGizmoRenderer;
        PerfHudOverlay                        m_PerfHud; // View -> Perf HUD viewport overlay
        Tools::FoliagePaintTool               m_FoliageTool;  // UE5-style foliage painting (extracted)
        Tools::CubeGridTool                   m_CubeGridTool; // UE5-style CubeGrid blockout (Modeling mode)
        Tools::PolyEditTool                   m_PolyEditTool; // face select + push/pull (Modeling mode)
        Tools::TerrainPaintTool               m_TerrainTool;  // terrain splat-layer painting (extracted)
        Tools::GizmoController                m_Gizmo;       // object + bone transform gizmos (extracted)
        Tools::PickingController              m_Picking;     // ray-pick + select (extracted)
        std::unique_ptr<AsyncMeshLoader>      m_AsyncLoader; // background cook of dropped meshes (no hitch)

        // Drain finished async cooks (main thread): register + assign the mesh to its pending entity. Called
        // once per frame from OnUIRender. Also draws the loading progress bar while cooks are in flight.
        void UpdateAsyncLoads();
    };
} // namespace Desert::Editor