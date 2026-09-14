#include "UIEditorPanel.hpp"
#include "UIElementCatalog.hpp"
#include "UIElementFactory.hpp"

#include <Engine/Core/Scene.hpp>
#include <Engine/ECS/Entity.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Graphic/Framebuffer.hpp>
#include <Engine/Graphic/RenderPass.hpp>
#include <Engine/Graphic/Renderer.hpp>
#include <Engine/UI/UILayout.hpp>
#include <Engine/UI/UICanvasRenderer2D.hpp>

#include <Common/Core/Logger.hpp>

#include <ImGui/imgui.h>

#include <algorithm>
#include <cmath>

namespace Desert::Editor
{
    namespace ImGui = ::ImGui;

    namespace
    {
        // The preview target is the canvas's DESIGN resolution, not the window's size, and that is what makes
        // the preview honest: at the reference resolution all three canvas scale modes coincide (Stretch is
        // 1:1, ScaleWithScreen scales by 1, Letterbox fits exactly), so the picture is the canvas as authored
        // rather than the canvas squeezed into whatever the user dragged the window to. The window then just
        // fits that image into its content region.
        //
        // The cap is memory, not policy: Reference Width/Height go up to 7680x4320 in Details, and an RGBA32F
        // attachment that size is half a gigabyte for a preview window. A canvas past the cap is previewed at
        // the cap and SAYS SO in the log with both numbers — under Stretch its element offsets are in design
        // px and would not be scaled down with it, so that preview is the one case that is not to scale.
        constexpr uint32_t kMinDesignPx = 16;
        constexpr uint32_t kMaxDesignPx = 4096;

        uint32_t ClampDesignPx( float v )
        {
            if ( !std::isfinite( v ) || v < static_cast<float>( kMinDesignPx ) )
                return kMinDesignPx;
            return std::min( static_cast<uint32_t>( v ), kMaxDesignPx );
        }
    } // namespace

    UIEditorPanel::UIEditorPanel( const SubjectId& subject, const std::string& displayName,
                                  const std::shared_ptr<::Desert::Core::Scene>& scene )
         : ISubjectDocument( displayName, subject ), m_Scene( scene )
    {
        // An authoring preview is a SECOND view of a scene the viewport is already drawing. The one clock
        // this walk does not own is the UIAnim playhead, which lives in the component; the viewport's pass
        // advances it, and this view must not, or every clip runs at twice its authored speed whenever the
        // window is open.
        m_UIView.DrivesSceneAnimation = false;
    }

    UIEditorPanel::~UIEditorPanel()
    {
        ReleaseTarget();
    }

    entt::entity UIEditorPanel::ResolveCanvasEntity() const
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return entt::null;

        const auto entOpt = scene->FindEntityByID( Subject().Owner );
        if ( !entOpt )
            return entt::null;

        const entt::entity handle = entOpt->get().GetHandle();
        return scene->GetRegistry().has<ECS::UICanvasComponent>( handle ) ? handle : entt::null;
    }

    ECS::UICanvasComponent* UIEditorPanel::ResolveCanvas() const
    {
        const auto scene = m_Scene.lock();
        if ( !scene )
            return nullptr;

        const entt::entity handle = ResolveCanvasEntity();
        if ( handle == entt::null )
            return nullptr;

        return &scene->GetRegistry().get<ECS::UICanvasComponent>( handle );
    }

    void UIEditorPanel::ReleaseTarget()
    {
        if ( !m_Target )
            return;
        Graphic::Renderer::GetInstance().WaitDeviceIdle();
        m_RenderPass.reset();
        m_Target.reset();
        m_TargetWidth  = 0;
        m_TargetHeight = 0;
    }

    bool UIEditorPanel::EnsureTarget( uint32_t width, uint32_t height )
    {
        if ( m_Target && m_TargetWidth == width && m_TargetHeight == height && m_Render2D.IsInitialized() )
            return true;

        ReleaseTarget();

        // RGBA32F, the format the scene's own composite target uses, so the UI2D/UIText/UIGlass shaders
        // write exactly the values they write in the game. What this preview does NOT have is the post
        // stack the game composites the canvas into, so it shows the canvas before tonemapping.
        Graphic::FramebufferSpecification spec;
        spec.DebugName   = "UIEditorPreview";
        spec.Width       = width;
        spec.Height      = height;
        spec.NoResizeble = true;
        spec.Attachments.Attachments.push_back( ::Desert::Core::Formats::ImageFormat::RGBA32F );

        m_Target = Graphic::Framebuffer::Create( spec );
        if ( !m_Target )
        {
            m_PreviewError = "could not create the preview framebuffer";
            LOG_ERROR( "[UI Editor] {} ({}x{})", m_PreviewError, width, height );
            return false;
        }
        // The `/*forceRecreate=*/true` that used to be here asked for something the parameter never
        // delivered: `VulkanFramebuffer::Resize` ignored the flag and recreated unconditionally, which is
        // also what this call site wanted. The parameter is gone; the behaviour is what it always was.
        if ( const auto result = m_Target->Resize( width, height ); !result )
        {
            m_PreviewError = result.GetError();
            LOG_ERROR( "[UI Editor] preview framebuffer resize to {}x{} failed: {}", width, height,
                       m_PreviewError );
            m_Target.reset();
            return false;
        }

        Graphic::RenderPassSpecification passSpec;
        passSpec.TargetFramebuffer = m_Target;
        passSpec.DebugName         = "UIEditorPreview";
        // The authoring backdrop behind the canvas. It is editor chrome, not canvas content: the canvas
        // itself draws whatever its own panels say.
        passSpec.ClearColor.Color = { 0.094f, 0.098f, 0.118f, 1.0f };
        m_RenderPass              = Graphic::RenderPass::Create( passSpec );
        if ( !m_RenderPass )
        {
            m_PreviewError = "could not create the preview render pass";
            LOG_ERROR( "[UI Editor] {} ({}x{})", m_PreviewError, width, height );
            m_Target.reset();
            return false;
        }

        if ( const auto result = m_Render2D.Init( m_Target ); !result )
        {
            m_PreviewError = result.GetError();
            LOG_ERROR( "[UI Editor] preview Render2D init failed: {}", m_PreviewError );
            m_RenderPass.reset();
            m_Target.reset();
            return false;
        }

        m_TargetWidth  = width;
        m_TargetHeight = height;
        m_PreviewError.clear();
        LOG_INFO( "[UI Editor] preview target {}x{} (canvas design resolution)", width, height );
        return true;
    }

    void UIEditorPanel::OnPreUpdate()
    {
        m_PreviewRecorded = false;

        // WAS ANYBODY LOOKING? Read here and cleared here, because the editor runs every OnPreUpdate before
        // the document well draws anything — so what this reads is last frame's answer, which is the only
        // one that exists at this point in the frame. See the header for why this document counts its own
        // hidden frames instead of using the six-slot sweep.
        m_FramesUndrawn  = m_DrawnLastFrame ? 0u : m_FramesUndrawn + 1u;
        m_DrawnLastFrame = false;
        if ( m_FramesUndrawn >= kFramesUndrawnBeforeTargetRelease )
        {
            ReleaseTarget();
            return;
        }

        const auto scene = m_Scene.lock();
        if ( !scene )
        {
            ReleaseTarget();
            return;
        }

        const ECS::UICanvasComponent* canvas = ResolveCanvas();
        if ( !canvas )
        {
            // The subject is gone. The window itself is closed by the editor's own liveness sweep
            // (IsSubjectAlive) with a named reason; releasing here is so the target does not outlive the
            // canvas by the frames that takes.
            ReleaseTarget();
            return;
        }

        // U7-2 refused the preview here whenever this document's canvas was not the one RenderCanvas2D
        // elected — the FIRST in the registry — because the alternative was sizing the target to this
        // canvas's design resolution and then showing a picture of a different one. Ю1 made the renderer
        // askable, so the document's own canvas is simply what gets drawn (see the call below) and there is
        // nothing left to refuse: every canvas in a scene is previewable, by name.
        //
        // Cleared on the way THROUGH so that EnsureTarget below is what writes a message again if the target
        // itself cannot be built.
        m_PreviewError.clear();

        const entt::entity canvasEntity = ResolveCanvasEntity();
        const auto&        canvasData   = canvas->Data;
        if ( !canvasData.Visible )
            return;

        const uint32_t w      = ClampDesignPx( canvasData.ReferenceWidth );
        const uint32_t h      = ClampDesignPx( canvasData.ReferenceHeight );
        const bool     capped = canvasData.ReferenceWidth > static_cast<float>( kMaxDesignPx ) ||
                            canvasData.ReferenceHeight > static_cast<float>( kMaxDesignPx ) ||
                            canvasData.ReferenceWidth < static_cast<float>( kMinDesignPx ) ||
                            canvasData.ReferenceHeight < static_cast<float>( kMinDesignPx );
        // Only when the size actually changes, so a capped canvas does not log once per frame.
        if ( capped && ( w != m_TargetWidth || h != m_TargetHeight ) )
            LOG_WARN( "[UI Editor] canvas design resolution {}x{} previewed at {}x{} (preview cap); a Stretch "
                      "canvas this large is NOT shown to scale",
                      canvasData.ReferenceWidth, canvasData.ReferenceHeight, w, h );
        if ( !EnsureTarget( w, h ) )
            return;

        const ::Desert::UI::Rect viewport{ 0.0f, 0.0f, static_cast<float>( w ), static_cast<float>( h ) };

        auto& renderer = Graphic::Renderer::GetInstance();
        renderer.BeginRenderPass( m_RenderPass.get(), /*clearFrame=*/true );
        m_Render2D.BeginFrame( { viewport.X, viewport.Y, viewport.W, viewport.H } );

        // input = nullptr is what makes the preview inert: buttons draw their normal state, nothing is
        // hovered, pressed, dragged or typed into, and no button action can fire from an authoring window.
        // It is the same "design mode" call EditorUIPass makes when Play-in-editor is off.
        //
        // worldViewProj = nullptr on purpose too: a WorldSpace canvas is billboarded by the camera in the
        // viewport, but there is no camera here — the authoring view shows it flat, at its design size.
        //
        // m_UIView is THIS window's own view state, and inertness is not enough without it: the frame hands
        // its hot election over at the end whether or not it had input, so this preview used to clear the
        // viewport's elected element every single frame it was open — one scene, no second document needed.
        // See UICanvasContext.hpp.
        //
        // ONE canvas, and the frame around it is still a frame: this window is a document over a single
        // UICanvasComponent, so unlike the viewport it names its canvas instead of drawing them all. The
        // cell it uses is this window's own (canvas x view) cell, so previewing the canvas here and playing
        // it in the viewport are two independent screen stacks — which is the point of the pair, and what
        // lets an author look at the Settings screen while the game sits on the main menu.
        //
        // THIS DOCUMENT'S OWN CANVAS, named. The window is a document over one UICanvasComponent, so the
        // entity it was opened on IS the answer — no election, no guard, and no second implementation of the
        // canvas pass (which is what this window's previous ImGui-based preview was, and why it was deleted).
        m_UIView.Materials = &m_Render2D.Materials();
        // This window is an authoring view by definition — it has no pointer and its whole purpose is to
        // show what was authored. An overlay canvas opened here is therefore shown as authored, unplaced,
        // which is exactly what the marquee and the drag handles need.
        m_UIView.AuthoringPreview = true;
        ::Desert::UI::BeginUIFrame( m_UIView, scene->GetRegistry(), viewport );
        if ( const auto drawn = ::Desert::UI::RenderCanvas2D( m_UIView, scene->GetRegistry(), canvasEntity,
                                                              m_Render2D.GetDrawList(),
                                                              /*worldViewProj=*/nullptr,
                                                              /*input=*/nullptr );
             !drawn )
        {
            // Reached only if the subject stopped being a canvas between ResolveCanvas() above and here.
            m_PreviewError = drawn.GetError();
            LOG_ERROR( "[UI Editor] {}", m_PreviewError );
        }
        ::Desert::UI::EndUIFrame( m_UIView, scene->GetRegistry(), m_Render2D.GetDrawList(), /*input=*/nullptr );
        m_Render2D.Flush();
        renderer.EndRenderPass();

        m_PreviewRecorded = true;
    }

    void UIEditorPanel::OnUIRender()
    {
        m_DrawnLastFrame = true;

        const auto                    scene  = m_Scene.lock();
        const ECS::UICanvasComponent* canvas = ResolveCanvas();
        if ( !scene || !canvas )
        {
            // One frame at most: the editor closes a document whose subject is gone, with the reason. Said
            // rather than left blank so that frame is not a window that looks broken.
            ImGui::TextDisabled( "This canvas no longer exists; closing." );
            return;
        }

        const entt::entity canvasEntity = ResolveCanvasEntity();
        const auto&        canvasData   = canvas->Data;

        // Toolbar, generated from the one element catalog the viewport's "UI" menu also reads. Buttons wrap
        // to the next line instead of running off the edge — a dozen of them do not fit a docked window.
        {
            const ImGuiStyle& style     = ImGui::GetStyle();
            const float       rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            for ( std::size_t i = 0; i < kUIElementCount; ++i )
            {
                const UIElementEntry& entry = kUIElements[i];
                const std::string     label = std::string( entry.Icon ) + " + " + entry.Label;
                if ( ImGui::Button( label.c_str() ) )
                    CreateUIElement( *scene, canvasEntity, i );

                if ( i + 1 >= kUIElementCount )
                    break;
                const std::string next = std::string( kUIElements[i + 1].Icon ) + " + " + kUIElements[i + 1].Label;
                const float       nextWidth = ImGui::CalcTextSize( next.c_str() ).x + style.FramePadding.x * 2.0f;
                if ( ImGui::GetItemRectMax().x + style.ItemSpacing.x + nextWidth < rightEdge )
                    ImGui::SameLine();
            }
        }
        ImGui::TextDisabled( "add UI elements, then edit anchors/colour in Details" );
        ImGui::Separator();

        if ( !canvasData.Visible )
        {
            ImGui::TextDisabled( "Canvas is hidden (UI Canvas -> Visible)." );
            return;
        }
        if ( !m_PreviewError.empty() )
        {
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "Preview unavailable: %s",
                                m_PreviewError.c_str() );
            return;
        }

        // Fit the design-resolution image into the content region, centred, preserving its aspect.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail  = ImGui::GetContentRegionAvail();
        if ( avail.x < 1.0f || avail.y < 1.0f )
            return;

        const ::Desert::UI::Rect fit = ::Desert::UI::CanvasRect(
             static_cast<float>( m_TargetWidth ), static_cast<float>( m_TargetHeight ), avail.x, avail.y );
        const ImVec2 imageMin( origin.x + fit.X, origin.y + fit.Y );
        const ImVec2 imageMax( imageMin.x + fit.W, imageMin.y + fit.H );

        if ( m_PreviewRecorded )
        {
            // Built on first use rather than in the constructor: a document is constructed between frames
            // and the ImGui texture cache needs a live renderer backend.
            if ( !m_UIHelper )
            {
                m_UIHelper = std::make_unique<Editor::UI::UIHelper>();
                m_UIHelper->Init();
            }
            ImGui::SetCursorScreenPos( imageMin );
            m_UIHelper->Image( m_Target->GetColorAttachmentImage(), ImVec2( fit.W, fit.H ) );
        }
        ImGui::GetWindowDrawList()->AddRect( imageMin, imageMax, IM_COL32( 90, 90, 100, 200 ) );

        ImGui::SetCursorScreenPos( origin );
        ImGui::Dummy( avail );
    }

} // namespace Desert::Editor
