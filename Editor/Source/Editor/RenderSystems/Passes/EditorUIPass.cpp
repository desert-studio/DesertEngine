#include <Editor/Core/ShotOptions.hpp>
#include "EditorUIPass.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/Graphic/SceneRenderer.hpp>
#include <Engine/Core/Serialize/WorldPartitionStreamingPerformance.hpp>
#include <Engine/Core/LevelTravel.hpp>
#include <Engine/UI/Ecs/LoadingOverlay.hpp>
#include <Engine/UI/Ecs/UICanvasLayoutEcs.hpp>
#include <Engine/UI/Ecs/UICanvasRendererEcs.hpp>
#include <UI/UIDataStore.hpp>
#include <Engine/UI/Ecs/UIWindowClipboard.hpp>
#include <Engine/Core/EngineContext.hpp>

#include <Editor/Core/Selection/UIPreview.hpp>
#include <Editor/Core/UIProbeRegistry.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Editor::Render
{
    EditorUIPass::~EditorUIPass()
    {
        if ( const auto scene = m_Scene.lock() )
        {
            scene->UnregisterExtensionPass( "EditorUI2D" );
            // The slot is keyed by the scene's ADDRESS, and a freed scene's address is reused by the next
            // one — the same false-negative UICanvasContext::Registry warns about. Dropping it here is
            // what stops a reopened document from inheriting the closed one's numbers.
            Core::UIProbeRegistry::Get().Forget( scene.get() );
        }
    }

    Common::BoolResultStr EditorUIPass::Install( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        m_Scene = scene;

        // The UI is drawn into the document's view (view 0, whose target this is), so its rows are that view's
        // (RT-LEDGER-4). The document's RenderRegistry, which owns this pass, is destroyed before that view's
        // renderer (SceneWorkspace), so the view closes with none of them left.
        const ::Desert::Graphic::SceneRenderer* view = scene->GetSceneRenderer();
        if ( view == nullptr )
            return Common::MakeError( "EditorUIPass: the scene has no view to draw its UI for" );
        if ( const auto result = m_Render2D.Init( scene->GetTargetFramebuffer(), view->GetLedgerInstance() );
             !result )
            return Common::MakeError( "EditorUIPass: " + result.GetError() );

        Graphic::ExtensionPass pass;
        pass.Name                  = "EditorUI2D";
        pass.Point                 = Graphic::RDG::ExtensionPoint::UI;
        // THE FRAME'S UI IS GATHERED IN SETUP (UE: Slate's elements are batched before the render pass that draws
        // them). The canvas walk - input feed, clicks, the draw list - runs while the frame graph is built, so the
        // node can declare one binding block per batch it will draw (Render2D::DeclareBindings) and the graph
        // validates them before anything is recorded; the exec only uploads and draws the list (Render2D::Flush).
        // Once per frame per view, as before: a view's graph is built and executed before the next view's.
        //
        // Render2D samples the backdrop pyramid behind a blurred panel: an engine image the frame graph's blur
        // nodes write as storage, so the pass names it and the graph brings it to a sampled layout before the UI
        // draws.
        pass.Declare = [this]( Graphic::RenderPassDeclaration& declared, const Graphic::ExtensionPassContext& ctx )
        {
            const auto scene = m_Scene.lock();
            if ( !scene || !ctx.Target )
                return;

            // THE CANVAS IS THE NODE'S RENDER AREA (RenderPassDeclaration::TargetExtent), not ctx.Target's size:
            // after the temporal resolve the UI draws into the OUTPUT-extent overlay set, while ctx.Target is the
            // render-extent scene target - laid out there, the canvas filled only the render-scale corner of the
            // image (87 % screen percentage: 1248x746 of 1435x857). UE: UMG's root fills the viewport's output
            // size (SConstraintCanvas under the DPI scale), never the scene's internal resolution.
            const glm::uvec2 extent = declared.TargetExtent();
            const float      w      = static_cast<float>( extent.x );
            const float      h      = static_cast<float>( extent.y );

            // Camera view-proj for world-space canvases (screen-space ignores it).
            glm::mat4        vp( 1.0f );
            const glm::mat4* vpPtr = nullptr;
            if ( ctx.Camera )
            {
                vp    = ctx.Camera->GetProjectionMatrix() * ctx.Camera->GetViewMatrix();
                vpPtr = &vp;
            }

            m_Render2D.BeginFrame( { 0.0f, 0.0f, w, h } );

            // UI Preview (Play-in-editor): feed the viewport's pointer/keyboard into the canvas so buttons /
            // toggles / sliders react in the editor. The ViewportPanel wrote this snapshot in viewport-display
            // px; scale it into the framebuffer's px space. Design mode leaves input null (normal authoring).
            auto&       pv = Editor::Core::UIPreview::Get();
            UI::WindowClipboard clipboard( *::Desert::EngineContext::GetInstance().GetWindow() );
            UI::UIInput         input;
            std::string         clicked;

            // A HEADLESS CAPTURE HAS NO CURSOR, so `--ui-pointer` is its cursor (Ю12). Until this existed
            // there was no arrangement of flags that could photograph anything the UI does in RESPONSE to
            // a pointer — a tooltip appearing after its hover delay and flipping at the edge of the view, a
            // context menu opening where it was clicked — so the whole overlay state machine was provable
            // only by a unit test. It is read here rather than written into UIPreview because the
            // ViewportPanel rewrites that snapshot every frame it draws and would erase it.
            const auto& shot     = ShotOptions::Get();
            const bool  cliInput = shot.Active() && shot.HasUIPointer;
            const bool  feed =
                 cliInput || ( pv.Enabled && pv.HasInput && pv.DisplaySize.x > 0.0f && pv.DisplaySize.y > 0.0f );
            if ( cliInput )
            {
                input.MousePx = shot.UIPointer;

                // THE PRESS WAITS FOR AN ELECTION TO LAND ON. A capture's first frames have none — the
                // scene is still being loaded and the canvas has not been walked yet — and the press edge
                // happens exactly ONCE, on the first frame the button is reported down. Delivered
                // immediately it was therefore spent on whatever the half-built frame elected, and the
                // context menu never opened: measured, and it looks exactly like "the flag does nothing".
                // `Hot` is the PREVIOUS frame's winner, so a non-null one means the UI is up and the
                // element under the pointer this frame is the same one.
                //
                // Under `--play` the canvas is walked for the whole load before Play starts, while the rows a
                // script fills on OnStart do not exist yet: an edge spent there landed on an empty list and an
                // expander or a row was never pressed. The press waits for the second walk in Play, whose `Hot`
                // was elected over the filled rows.
                if ( shot.PlayActive() && scene->IsPlaying() )
                    ++m_CliPlayWalks;
                const bool live      = !shot.PlayActive() || m_CliPlayWalks >= 2;
                const bool ready     = live && m_UIView.Hot != UI::NodeId::Null;
                input.MouseDown      = ready && shot.UIPress == ShotOptions::UIButtonHeld::Left;
                input.MouseRightDown = ready && shot.UIPress == ShotOptions::UIButtonHeld::Right;
            }
            else if ( feed )
            {
                input.MousePx       = { pv.MousePx.x * ( w / pv.DisplaySize.x ),
                                        pv.MousePx.y * ( h / pv.DisplaySize.y ) };
                input.MouseDown      = pv.Down;
                input.MouseReleased  = pv.Released;
                input.MouseRightDown = pv.RightDown;
                input.ScrollDelta   = pv.Scroll;
                input.Keys           = pv.Keys;
                input.Mods           = pv.Mods;
                input.TypedText     = pv.TypedText;
                input.Clipboard      = &clipboard;
            }

            std::vector<std::string> uiMessages;
            // EVERY CANVAS OF THE LEVEL, in authored order. This pass used to ask UI::SoleCanvas and REFUSE
            // a level with two canvases: an honest refusal while a view could hold the runtime state of one
            // canvas only, and it is gone with that limit (Ю4) — m_UIView now holds one cell per (canvas x
            // this view), so a HUD and a pause menu are simply two canvases, drawn in Sort Order.
            //
            // m_UIView is this VIEW's state — one per EditorUIPass, and the editor builds one pass per open
            // scene document, so two viewports still do not walk into each other's hover clocks, hot element
            // or screen stacks.
            //
            // This view's UI materials. Set here, beside the walk, because the cache belongs to the backend
            // that will draw the list and a view must never be handed another view's pipelines — see
            // UIViewContext::Materials.
            m_UIView.Materials = &m_Render2D.Materials();

            // And the worlds. Same rule and a stronger one: this source owns renderer SLOTS, and it
            // learns which of its captures are still wanted from the walks below — so it must be THIS
            // view's, never shared with another (UIViewContext::RenderTextures).
            m_UIView.RenderTextures = &m_RenderTextures;

            // DESIGN MODE IS AN AUTHORING VIEW, AND PREVIEW IS NOT. With Play-in-editor off the overlays of
            // the level — tooltips, menus, dialogs, the toast stack — are drawn where they were authored, so
            // they can be selected, moved and edited in the viewport like any other canvas. Turning Preview
            // on hands them to their state machine and they disappear until something opens them, which is
            // what the author is previewing.
            m_UIView.AuthoringPreview = !feed;

            // AN AUTHORED LEVEL IS NOT A GAME. Edit shows each UI clip at its playhead (the Sequencer moves
            // it); Play / Paused is the game, where AutoPlay clips start — at t = 0, because entering Play
            // dropped their players (Core::BeginPlay). Auto-playing in Edit ran a three-second clip out while
            // the level was being looked at, and Play then inherited the finished playhead: a black viewport.
            m_UIView.GameWorld = scene->GetState() != ::Desert::Core::Scene::SceneState::Edit;

            const std::vector<entt::entity> canvases = UI::CanvasesInDrawOrder( scene->GetRegistry() );
            UI::BeginUIFrame( m_UIView, scene->GetRegistry(), UI::Rect{ 0.0f, 0.0f, w, h }, m_FrameDtSeconds );
            for ( const entt::entity canvas : canvases )
                if ( const auto drawn = UI::RenderCanvas2D(
                          m_UIView, scene->GetRegistry(), canvas, m_Render2D.GetDrawList(), vpPtr,
                          feed ? &input : nullptr, feed ? &clicked : nullptr, feed ? &pv.Focused : nullptr );
                     !drawn )
                    LOG_ERROR( "[UI Preview] {}", drawn.GetError() );
            UI::EndUIFrame( m_UIView, scene->GetRegistry(), m_Render2D.GetDrawList(), feed ? &input : nullptr,
                            feed ? &pv.Focused : nullptr, feed ? &clicked : nullptr,
                            feed ? &uiMessages : nullptr );

            // BEFORE Flush and AFTER the frame: this is the one moment the frame's draw list is complete
            // and still readable, and it is the same object Flush is about to turn into draw calls — so the
            // panel's numbers cannot be a second tally that drifts from it. Once per FRAME, not per canvas:
            // the list holds every canvas's geometry, so a per-canvas capture counted the earlier canvases'
            // batches again for each canvas after them. The sink does nothing unless the panel armed it.
            Core::UIProbeRegistry::Get()
                 .Slot( scene.get() )
                 .Capture( m_UIView, scene->GetRegistry(), canvases, m_Render2D.GetDrawList(),
                           UI::Rect{ 0.0f, 0.0f, w, h } );
            // World streaming waiting for the cell under the camera (WP12): the overlay a game shows, drawn into
            // the same list as the level's canvases, so a viewport capture sees what a player would.
            if ( const auto* wait = scene->GetRegistry().try_ctx<::Desert::Core::WorldStreamingWait>();
                 wait != nullptr && wait->Assessment.Blocks() )
                UI::DrawStreamingWaitOverlay( m_Render2D.GetDrawList(), w, h, wait->FramesWaiting );
            // Glass panels sample THIS VIEW's blurred scene snapshot, built just before this phase as a transient
            // of this view's graph (ctx.Graph): each glass batch's block names it (BackdropTexture).
            if ( ctx.Graph.Transients.BackdropBlur.IsValid() )
                declared.Read( ctx.Graph.Transients.BackdropBlur, Graphic::RDG::Access::SampledGraphics,
                               Graphic::RDG::SubresourceRange::All() );
            m_Render2D.DeclareBindings( declared, ctx.Graph.Transients.BackdropBlur );

            // A button fired in preview: report it; quit / open-URL are NOT executed here — they would close
            // the editor or leave it. Scene-load is, in Play, through Core::OpenLevel (below). Interactive
            // toggles/sliders/inputs already mutated in the walk. Everything that is NOT one of those three
            // process-level encodings is a gameplay message and goes on the same queue as the pointer events
            // below, because a preview whose buttons are heard by scripts and whose pointer events are heard by
            // scripts is one preview; dropping the button half was the defect this replaces.
            if ( feed && !clicked.empty() )
            {
                LOG_INFO( "[UI Preview] button action: {}", clicked );
                constexpr std::string_view kScene = "scene:";
                // LoadScene is Core::OpenLevel, the call the game's own button makes: in Play the editor's
                // boundary loads the level into the PLAYED world (PlayWorldTravel) and Stop returns to the
                // authored one. A preview of the authored level (Edit) has no game world to travel.
                if ( clicked.starts_with( kScene ) && m_UIView.GameWorld )
                {
                    if ( const auto queued = ::Desert::Core::OpenLevel( clicked.substr( kScene.size() ) );
                         !queued )
                        LOG_ERROR( "[UI Preview] {}", queued.GetError() );
                }
                const bool processLevel =
                     clicked == "quit" || clicked.starts_with( kScene ) || clicked.starts_with( "url:" );
                if ( !processLevel )
                    UI::UIMessageQueue::Get().Push( clicked );
            }
            for ( const std::string& msg : uiMessages ) // pointer enter/exit, press/release, drops
            {
                LOG_INFO( "[UI Preview] {}", msg );
                UI::UIMessageQueue::Get().Push( msg ); // scripts hear preview messages too, if any run
            }
        };
        // Draws the list the setup gathered, over the blocks it declared. The backdrop pyramid is only built when
        // the canvas asked for it LAST frame, so hand the flag back after flushing.
        pass.Execute = [this]( const Graphic::ExtensionPassContext& ctx,
                               Graphic::RDG::PassContext&           node ) -> Common::BoolResultStr
        {
            if ( m_Scene.expired() || ctx.Target == nullptr )
                return BOOLSUCCESS; // the setup gathered nothing either
            const Common::BoolResultStr flushed = m_Render2D.Flush( node, ctx.Graph.Transients.BackdropBlur, 0 );
            if ( auto* renderer = ctx.Renderer )
                renderer->SetBackdropBlurNeeded( m_Render2D.UsedBackdrop() );
            return flushed;
        };
        scene->RegisterExtensionPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
