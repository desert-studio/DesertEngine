#include <Editor/Core/ShotOptions.hpp>
#include "EditorUIPass.hpp"

#include <Engine/Graphic/Renderer.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIDataStore.hpp>

#include <Editor/Core/Selection/UIPreview.hpp>
#include <Editor/Core/UIProbeRegistry.hpp>

#include <Common/Core/Logger.hpp>

namespace Desert::Editor::Render
{
    EditorUIPass::~EditorUIPass()
    {
        if ( const auto scene = m_Scene.lock() )
        {
            scene->UnregisterExternalPass( "EditorUI2D" );
            // The slot is keyed by the scene's ADDRESS, and a freed scene's address is reused by the next
            // one — the same false-negative UICanvasContext::Registry warns about. Dropping it here is
            // what stops a reopened document from inheriting the closed one's numbers.
            Core::UIProbeRegistry::Get().Forget( scene.get() );
        }
    }

    Common::BoolResultStr EditorUIPass::Install( const std::shared_ptr<::Desert::Core::Scene>& scene )
    {
        m_Scene = scene;

        if ( const auto result = m_Render2D.Init( scene->GetTargetFramebuffer() ); !result )
            return Common::MakeError( "EditorUIPass: " + result.GetError() );

        Graphic::ExternalPassSpecification pass;
        pass.Name                  = "EditorUI2D";
        pass.Phase                 = Graphic::RenderPhase::UI;
        pass.Dependencies          = { Graphic::RenderPassDependency( Graphic::RenderPhase::Geometry ) };
        pass.PipelineSpecification = m_Render2D.GetPipeline()->GetSpecification();
        pass.Execute               = [this]( const Graphic::ExternalPassContext& ctx )
        {
            const auto scene = m_Scene.lock();
            if ( !scene || !ctx.Target )
                return;

            const float w = static_cast<float>( ctx.Target->GetFramebufferWidth() );
            const float h = static_cast<float>( ctx.Target->GetFramebufferHeight() );

            // Camera view-proj for world-space canvases (screen-space ignores it).
            glm::mat4        vp( 1.0f );
            const glm::mat4* vpPtr = nullptr;
            if ( ctx.Camera )
            {
                vp    = ctx.Camera->GetProjectionMatrix() * ctx.Camera->GetViewMatrix();
                vpPtr = &vp;
            }

            m_Render2D.BeginFrame( { 0.0f, 0.0f, w, h } );

            // Glass panels sample the blurred scene snapshot built just before this phase. It is only
            // built when the canvas asked for it LAST frame, so hand the flag back after flushing.
            if ( auto* renderer = scene->GetSceneRenderer() )
                m_Render2D.SetBackdrop( renderer->GetBackdropBlurImage().get(),
                                        renderer->GetBackdropBlurMaxLod() );

            // UI Preview (Play-in-editor): feed the viewport's pointer/keyboard into the canvas so buttons /
            // toggles / sliders react in the editor. The ViewportPanel wrote this snapshot in viewport-display
            // px; scale it into the framebuffer's px space. Design mode leaves input null (normal authoring).
            auto&       pv = Editor::Core::UIPreview::Get();
            UI::UIInput input;
            std::string clicked;

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
                const bool ready     = m_UIView.Hot != entt::null;
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
                input.Escape         = pv.Escape;
                input.ScrollDelta   = pv.Scroll;
                input.Tab           = pv.Tab;
                input.Submit        = pv.Submit;
                input.Backspace     = pv.Backspace;
                input.TypedText     = pv.TypedText;
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

            // DESIGN MODE IS AN AUTHORING VIEW, AND PREVIEW IS NOT. With Play-in-editor off the overlays of
            // the level — tooltips, menus, dialogs, the toast stack — are drawn where they were authored, so
            // they can be selected, moved and edited in the viewport like any other canvas. Turning Preview
            // on hands them to their state machine and they disappear until something opens them, which is
            // what the author is previewing.
            m_UIView.AuthoringPreview = !feed;

            const std::vector<entt::entity> canvases = UI::CanvasesInDrawOrder( scene->GetRegistry() );
            UI::BeginUIFrame( m_UIView, scene->GetRegistry(), UI::Rect{ 0.0f, 0.0f, w, h } );
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
            m_Render2D.Flush();

            if ( auto* renderer = scene->GetSceneRenderer() )
                renderer->SetBackdropBlurNeeded( m_Render2D.UsedBackdrop() );

            // A button fired in preview: report it, but DON'T execute scene-load / quit / open-URL here —
            // that would close or switch the editor. Interactive toggles/sliders/inputs already mutated in
            // the walk. Everything that is NOT one of those three process-level encodings is a gameplay
            // message and goes on the same queue as the pointer events below, because a preview whose
            // buttons are heard by scripts and whose pointer events are heard by scripts is one preview;
            // dropping the button half was the defect this replaces.
            if ( feed && !clicked.empty() )
            {
                LOG_INFO( "[UI Preview] button action: {}", clicked );
                const bool processLevel =
                     clicked == "quit" || clicked.rfind( "scene:", 0 ) == 0 || clicked.rfind( "url:", 0 ) == 0;
                if ( !processLevel )
                    UI::UIMessageQueue::Get().Push( clicked );
            }
            for ( const std::string& msg : uiMessages ) // pointer enter/exit, press/release, drops
            {
                LOG_INFO( "[UI Preview] {}", msg );
                UI::UIMessageQueue::Get().Push( msg ); // scripts hear preview messages too, if any run
            }
        };

        scene->RegisterExternalPass( std::move( pass ) );
        return BOOLSUCCESS;
    }
} // namespace Desert::Editor::Render
