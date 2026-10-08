#include "UICanvasRenderer2D.hpp"

#include <Engine/UI/UIOverlay.hpp>

#include <Engine/ECS/Components.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Text/BakedFont.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/Text/Utf8.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIPathGeometry.hpp>
#include <Engine/UI/UIWalkCtx.hpp>
#include <Engine/UI/UIRichText.hpp>
#include <Engine/UI/Widgets/UIWidgets.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <span>
#include <unordered_set>
#include <array>
#include <optional>
#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Desert::UI
{
    namespace Walk
    {
        // Every non-empty UIScreen name in @p e's sub-tree, in draw order.
        //
        // WHY A SUB-TREE WALK AND NOT `reg.view<UIScreenComponent>()`: the seed below picks "the first screen
        // that exists" and re-seeds when the current name does not, and a registry-wide view answers about
        // the WHOLE SCENE. While only one canvas was ever drawn that was indistinguishable from asking the
        // canvas; now that the canvas is named, a second canvas's screens would seed and re-seed this one's
        // context, and a menu canvas could silently drive a HUD canvas's screen machine.
        template <typename F>
        void ForEachScreenName( entt::registry& reg, entt::entity e, F&& fn )
        {
            if ( !reg.valid( e ) )
                return;
            if ( reg.has<ECS::UIScreenComponent>( e ) )
            {
                const std::string& n = reg.get<ECS::UIScreenComponent>( e ).Data.Name;
                if ( !n.empty() )
                    fn( n );
            }
            if ( reg.has<ECS::RelationshipComponent>( e ) )
                for ( auto c : reg.get<ECS::RelationshipComponent>( e ).Children )
                    ForEachScreenName( reg, c, fn );
        }

        // Recursively draw one element. `forcedRect` (non-null) is the rect assigned by a parent auto-layout
        // group — it overrides the element's own anchors for position + size.
        //
        // A CANVAS IS A TREE AND THIS WALK IS ITS TRAVERSAL. The depth is the authored nesting, and every
        // container below recurses over a Relationship child list or over a window of one, so nothing
        // here can revisit an element it has already drawn.
        // NOLINTNEXTLINE(misc-no-recursion)
        void DrawElement( WalkCtx& ctx, entt::registry& reg, entt::entity e, const Rect& parent, float scale,
                          Graphic::Render2D::DrawList2D& dl, const UIInput* input, std::string* outClicked,
                          entt::entity* focused, std::vector<PopupInfo>* popups,
                          std::vector<entt::entity>* focusables, const Graphic::Render2D::ClipRegion2D& clipRegion,
                          HitScope scope, const Rect* forcedRect )
        {
            // The visibility axis, before anything else is computed. Hidden and Collapsed both stop here
            // and take the whole sub-tree with them — nothing drawn, nothing hit-tested, no tween clock
            // advanced (so an intro tween with Rewind On Hide replays when the element comes back, exactly
            // as it does for a screen that is not current).
            //
            // The two differ only in the parent's layout, which was decided one level UP: a Collapsed child
            // of a layout group never reaches this function at all, because the group left no slot for it
            // and its siblings closed the gap. A Hidden one reaches it with a slot and leaves a hole. Under
            // plain anchor layout the two are the same picture, because there is no packing to close.
            // An element some retainer names as its mask: capture its subtree into the frame's mask layer for
            // it, before (and regardless of) its own visibility — a hidden element is a pure mask, UE's mask
            // texture as an element. Input is not routed through the capture.
            if ( ctx.Root != nullptr && ctx.MaskCapture != e && ctx.MaskTargets.contains( e ) )
            {
                auto& mask = ctx.Root->MaskLayer( static_cast<int64_t>( entt::to_integral( e ) ) );
                if ( mask.Empty() )
                {
                    if ( dl.HasTransform() )
                        mask.PushTransform( dl.GetTransform() );
                    std::vector<PopupInfo>    noPopups;
                    std::vector<entt::entity> noFocus;
                    std::string               noClick;
                    entt::entity              noFocused = entt::null;
                    const entt::entity        outer     = ctx.MaskCapture;
                    ctx.MaskCapture                     = e;
                    DrawElement( ctx, reg, e, parent, scale, mask, nullptr, &noClick, &noFocused, &noPopups,
                                 &noFocus, clipRegion, scope, forcedRect );
                    ctx.MaskCapture = outer;
                }
            }

            if ( !IsElementVisible( reg, e ) && ctx.MaskCapture != e )
                return;

            // UE Retainer Box: the element and its subtree are recorded into their own layer and shown
            // through one composite with the element's effect. Render2D::AddRetainedPasses renders the layer as a
            // graph pass.
            if ( ctx.Retaining != e && reg.has<ECS::UIRetainerComponent>( e ) )
            {
                const UIRetainerData&      rd    = reg.get<ECS::UIRetainerComponent>( e ).Data;
                uint32_t                   index = 0;
                auto&                      layer = dl.BeginRetainedLayer( &index );
                if ( dl.HasTransform() )
                    layer.PushTransform( dl.GetTransform() );
                const entt::entity outer = ctx.Retaining;
                ctx.Retaining            = e;
                DrawElement( ctx, reg, e, parent, scale, layer, input, outClicked, focused, popups, focusables,
                             clipRegion, scope, forcedRect );
                ctx.Retaining = outer;

                Graphic::Render2D::RetainerEffect fx;
                fx.Opacity       = rd.Opacity;
                fx.Haze          = rd.Haze;
                fx.HazeAmplitude = rd.HazeAmplitude * scale;
                fx.HazeScale     = rd.HazeScale * scale;
                fx.HazeSpeed     = rd.HazeSpeed;
                fx.Time          = static_cast<float>( ctx.View.Time );
                // A keyed clip REPLACES the authored amplitude while it drives it (never written back).
                if ( const UIClipSample* clip = ctx.View.Animation().Sample( e ); clip != nullptr )
                {
                    if ( const std::optional<float> amplitude = clip->HazeAmplitude; amplitude.has_value() )
                        fx.HazeAmplitude = *amplitude * scale;
                }

                int64_t maskKey = -1;
                if ( rd.Mask )
                    if ( const auto m = ctx.MaskOf.find( e ); m != ctx.MaskOf.end() )
                    {
                        fx.Mask       = true;
                        fx.InvertMask = rd.InvertMask;
                        maskKey       = static_cast<int64_t>( entt::to_integral( m->second ) );
                    }
                dl.AddRetainedComposite( index, maskKey, fx, glm::vec4( 1.0f ) );
                return;
            }

            // THIS ELEMENT'S STYLE, resolved once, before the rect: a themed padding changes what the
            // Content Size Fitter measures, so the style has to exist before the geometry does.
            const ElementStyle st = StyleFor( ctx, reg, e );

            Rect       rect      = parent;
            const bool hasLayout = reg.has<ECS::UILayoutComponent>( e );
            if ( forcedRect )
                rect = *forcedRect; // positioned + sized by the parent's layout group
            else if ( hasLayout )
            {
                const auto& L = reg.get<ECS::UILayoutComponent>( e ).Data;
                rect          = ResolveRect( L.AnchorMin, L.AnchorMax, L.OffsetMin * scale, L.OffsetMax * scale,
                                             L.CustomMinimumSize * scale, parent );
            }
            if ( hasLayout ) // fitters reshape the resolved rect (also applied inside a layout group)
            {
                const auto& L = reg.get<ECS::UILayoutComponent>( e ).Data;
                rect          = ApplyAspectFit( rect, L.AspectRatio, static_cast<int>( L.AspectMode ) );
                if ( ( L.FitWidth || L.FitHeight ) && reg.has<ECS::UILayoutGroupComponent>( e ) )
                {
                    const glm::vec2 content = GroupContentPx( reg, e, st, scale );
                    if ( L.FitWidth )
                        rect.W = content.x;
                    if ( L.FitHeight )
                        rect.H = content.y;
                }
            }

            // Screen gating: a screen sub-tree draws only while it is current, or while it is the one
            // handing over. During a hand-over both are on screen — the incoming sliding in and fading up,
            // the outgoing doing the reverse — which is the whole transition.
            float     screenFade = 1.0f;
            glm::vec2 screenSlide( 0.0f );
            if ( reg.has<ECS::UIScreenComponent>( e ) )
            {
                const std::string& name      = reg.get<ECS::UIScreenComponent>( e ).Data.Name;
                const bool         isCurrent = ( name == ctx.Canvas.Screen );
                const bool         isLeaving = ( name == ctx.Canvas.ScreenFrom && ctx.Canvas.ScreenT < 1.0f );
                if ( !isCurrent && !isLeaving )
                    return; // not on screen: skip the whole sub-tree, input included

                if ( ctx.Canvas.ScreenT < 1.0f )
                {
                    const float k   = Ease( ctx.Canvas.ScreenEasing, ctx.Canvas.ScreenT );
                    const float dir = ctx.Canvas.ScreenBack ? -1.0f : 1.0f;
                    if ( isCurrent )
                    {
                        screenFade    = k;
                        screenSlide.x = ( 1.0f - k ) * ctx.Canvas.ScreenSlidePx * dir;
                    }
                    else // leaving: pushed out the opposite way
                    {
                        screenFade    = 1.0f - k;
                        screenSlide.x = -k * ctx.Canvas.ScreenSlidePx * dir;
                    }
                }
            }

            // Tween: shift/resize the resolved rect and stage the colour multiplier its draws will use.
            // Applied on the way out, never written back — see SampleTween.
            TweenSample tween = SampleTween( ctx, reg, e );
            ApplyAnimClip( ctx, e, tween ); // a clip layers on top of the one-shot tween

            // A binding can hide the element outright — skip the sub-tree, input included.
            const BindingSample binding = SampleBinding( reg, e, tween, ctx.Canvas );
            if ( binding.Hide )
                return;
            rect.X += ( tween.Offset.x + screenSlide.x ) * scale;
            rect.Y += ( tween.Offset.y + screenSlide.y ) * scale;
            rect.W += tween.Size.x * scale;
            rect.H += tween.Size.y * scale;

            // --- The render transform (Ю8) ---------------------------------------------------------
            // Pushed HERE, after the rect is final and before a single primitive of this element or of
            // its sub-tree is emitted, and popped by the guard below on the way out. That placement is
            // the whole of "the parent's transform acts on its children": there is no per-child code
            // for inheritance, the children simply draw while their ancestors' matrices are on the
            // stack. It is also after every early return above, so no path can leave one pushed.
            //
            // Nothing is pushed for a neutral transform, so a canvas that rotates nothing produces the
            // byte-identical vertex stream it produced before this existed.
            struct TransformRestore
            {
                Graphic::Render2D::DrawList2D& List;
                bool                           Pushed;
                ~TransformRestore()
                {
                    if ( Pushed )
                        List.PopTransform();
                }
            } transformRestore{ dl, false };

            if ( hasLayout )
            {
                const auto& L = reg.get<ECS::UILayoutComponent>( e ).Data;
                if ( L.Rotation != 0.0f || L.Scale != glm::vec2( 1.0f, 1.0f ) )
                {
                    // Pivot is a FRACTION of this element's own rect, so it keeps meaning across a
                    // resize and across the canvas scale — the pixels it names are recomputed here.
                    const glm::vec2 pivotPx( rect.X + L.Pivot.x * rect.W, rect.Y + L.Pivot.y * rect.H );
                    dl.PushTransform( Graphic::Render2D::MakeTransform2D( pivotPx, L.Rotation, L.Scale ) );
                    transformRestore.Pushed = true;
                }
            }

            // THE POINTER GOES THROUGH THE SAME MATRIX, BACKWARDS. Everything below hit-tests against
            // `rect`, which is the element's rect BEFORE the transform; the geometry that reaches the
            // screen is that rect AFTER it. So the pointer is brought into the same space by inverting
            // the accumulated matrix the draw list is holding — the very one the vertices went through,
            // read back rather than rebuilt. Two matrices that must agree is the defect this project
            // pays for most often; there is one here, used in two directions.
            const glm::vec2 pointerPx =
                 input ? ( dl.HasTransform()
                                ? Graphic::Render2D::TransformPoint2D(
                                       Graphic::Render2D::InverseTransform2D( dl.GetTransform() ), input->MousePx )
                                : input->MousePx )
                       : glm::vec2( 0.0f );

            // This element's on-screen bounding box. The clip is a scissor and the scissor is axis
            // aligned, so both the drawn clip and the pointer's clip are this box (DrawList2D::
            // PushClipRect computes it through the same TransformedAABB2D).
            const auto ScreenBounds = [&dl]( const Rect& r ) { return ScreenBoundsOf( dl, r ); };

            // Tints nest: a faded panel fades its children with it.
            const glm::vec4 parentTint = ctx.View.Tint;
            ctx.View.Tint              = parentTint * tween.Tint * glm::vec4( 1.0f, 1.0f, 1.0f, screenFade );
            struct TintRestore
            {
                UIViewContext& Ctx;
                glm::vec4      Prev;
                ~TintRestore()
                {
                    Ctx.Tint = Prev;
                }
            } tintRestore{ ctx.View, parentTint };

            // The hit-test axis lives on the layout (every UI element has one); an element without one
            // takes the default. Four values, resolved into the three questions the walk actually asks —
            // may I be elected, may I react, and what may my children do — and each is narrowed by what
            // an ancestor already allowed, so permissions only ever shrink going down.
            const UIHitTest hitTest =
                 hasLayout ? reg.get<ECS::UILayoutComponent>( e ).Data.HitTest : UIHitTest::All;

            // Blocking elects itself precisely so the pointer STOPS here: it is the greyed-out form and the
            // modal dialog, which must swallow the click rather than let it reach what is behind them.
            const bool electsSelf = scope.Elect && ( hitTest == UIHitTest::All || hitTest == UIHitTest::Blocking );
            // This element's own value only; the inherited half arrives through `electsSelf` above and is
            // ANDed in by `interactive` below.
            const bool responds = hitTest == UIHitTest::All || hitTest == UIHitTest::ChildrenOnly;

            // ONE PREDICATE FOR BOTH INPUT PATHS, and it is the whole point of this line existing.
            //
            // У4 shipped the two axes honoured by the POINTER alone: election ran through `scope`, and the
            // controls compared against `ctx.View.Hot`. The keyboard reached the same controls by a route that
            // asked neither — the Tab list took every focusable in the tree, and Enter fired on whatever
            // `focused` held — so a button inside a Blocking panel was still tabbable and still fired. That
            // is precisely the greyed-out modal the fourth enum value was added for, operable by keyboard.
            //
            // So "may this element be interacted with" is decided ONCE, here, and the pointer sites (`hot`)
            // and the keyboard sites (the focus list, Enter, the input field's typing) all read this same
            // value. Two predicates that must agree is the defect shape this project keeps paying for; one
            // predicate cannot disagree with itself.
            //
            // It also closes a one-frame hole the pointer had on its own: `ctx.View.Hot` is LAST frame's winner,
            // so an element whose ancestor became Blocking since then was still `responds && e == ctx.View.Hot`
            // for one frame. ANDing this frame's `electsSelf` in is what makes the permission current.
            const bool interactive = electsSelf && responds;

            // Blocking and None both close the sub-tree to the pointer; they differ only in whether the
            // element itself stops it, which is `electsSelf` above.
            const HitScope childScope{ scope.Elect &&
                                       ( hitTest == UIHitTest::All || hitTest == UIHitTest::ChildrenOnly ) };

            // What the widget below draws from — every value it reads was resolved above, once.
            ElementFrame frame{ ctx,        reg,     e,      scale,      dl,         input,
                                outClicked, focused, popups, focusables, clipRegion, childScope,
                                st,         rect,    tween,  binding,    pointerPx,  interactive };

            if ( forcedRect || hasLayout )
            {
                const glm::vec2 mn( rect.X, rect.Y );
                const glm::vec2 mx( rect.X + rect.W, rect.Y + rect.H );

                // Elect the hot element: last writer in draw order = topmost. Clipped-away pixels don't
                // count, so a scrolled-out row can't be clicked through its viewport.
                //
                // `rect` is tested against the UNDONE pointer and `clipRegion` against the screen one,
                // because they live in different spaces on purpose: the element's own rect is what the
                // transform acts on, the clip is a region of the SCREEN — the scissor box the hardware cut,
                // intersected with the oblique edges the draw list cut the geometry with. Both halves, so a
                // pixel the clipper removed cannot still take the pointer.
                if ( input && electsSelf && PointIn( rect, pointerPx ) &&
                     Graphic::Render2D::ClipRegionContains( clipRegion, input->MousePx ) )
                {
                    ctx.View.HotNext = e;
                    // The drag ghost is drawn at the cursor in SCREEN space, so what it needs is the
                    // element's footprint on screen — the same box, for a straight element.
                    ctx.View.HotNextRect = ScreenBounds( rect );
                }
                // This element is what the pointer is over (resolved last frame) AND it may be interacted
                // with at all — the same predicate the keyboard sites below read.
                const bool hot = interactive && e == ctx.View.Hot;
                frame.Mn       = mn;
                frame.Mx       = mx;
                frame.Hot      = hot;

                // A drop target outlines itself while a drag it would accept is in flight.
                if ( ctx.View.Drag.Active && reg.has<ECS::UIDropTargetComponent>( e ) )
                {
                    const auto& dt = reg.get<ECS::UIDropTargetComponent>( e ).Data;
                    if ( Accepts( dt, ctx.View.Drag.Payload ) )
                        dl.AddRect( mn, mx,
                                    glm::vec4( st.Color( StyleSlot::DropTargetHighlight, dt.HighlightColor ),
                                               hot ? 1.0f : 0.6f ),
                                    hot ? 3.0f : 2.0f );
                }

                // Panels and buttons render their sprite (single or 9-slice) tinted by the colour, or a flat
                // box when no sprite is bound. Button hover/press state needs input plumbing (a later slice),
                // so the normal state is drawn for now. Rounding / gradient / effects also come later.
                if ( reg.has<ECS::UIButtonComponent>( e ) )
                    DrawButtonWidget( frame );
                else if ( reg.has<ECS::UIPanelComponent>( e ) )
                    DrawPanelWidget( frame );
                else if ( reg.has<ECS::UIProgressBarComponent>( e ) )
                    DrawProgressBarWidget( frame );
                else if ( reg.has<ECS::UIPathComponent>( e ) )
                    DrawPathWidget( frame );
                else if ( reg.has<ECS::UIToggleComponent>( e ) )
                    DrawToggleWidget( frame );
                else if ( reg.has<ECS::UISliderComponent>( e ) )
                    DrawSliderWidget( frame );
                else if ( reg.has<ECS::UIInputFieldComponent>( e ) )
                    DrawInputFieldWidget( frame );
                else if ( reg.has<ECS::UIDropdownComponent>( e ) )
                    DrawDropdownWidget( frame );

                if ( reg.has<ECS::UITextComponent2D>( e ) )
                    DrawTextWidget( frame );

                if ( reg.has<ECS::UIIconComponent>( e ) )
                    DrawIconWidget( frame );

                if ( reg.has<ECS::UIImageComponent>( e ) )
                    DrawImageWidget( frame );

                if ( reg.has<ECS::UIRenderTextureComponent>( e ) )
                    DrawRenderTextureWidget( frame );

                // Keyboard focus: record this control for Tab-cycling, and draw a focus ring when it holds
                // focus (InputField draws its own coloured border, so skip the generic ring there).
                //
                // GATED BY THE SAME PREDICATE THE POINTER USES. An ungated list is what let Tab walk into a
                // Blocking panel and hand Enter a target the mouse could never have reached; it is also why
                // Tab now steps OVER such a control rather than sticking on it.
                if ( interactive && IsFocusable( reg, e ) )
                {
                    if ( focusables )
                        focusables->push_back( e );
                    if ( focused && *focused == e && !reg.has<ECS::UIInputFieldComponent>( e ) )
                        dl.AddRect(
                             mn, mx,
                             glm::vec4( st.Color( StyleSlot::FocusRing, glm::vec3( 0.30f, 0.62f, 0.98f ) ), 1.0f ),
                             std::max( 1.0f, 2.0f * scale ) );
                }
            }

            if ( reg.has<ECS::RelationshipComponent>( e ) )
                DrawChildren( frame );
        }

        // The directional focus step a frame's keys ask for: -1 = previous focusable (Up / W), +1 = next
        // (Down / S), 0 = none. A menu is walked with the arrows the way UE's Slate navigation walks it. When
        // both directions arrive in one frame the LAST event wins — the order the host saw them in.
        int NavigateStep( const UIInput& input )
        {
            int step = 0;
            for ( const UIKeyEvent& k : input.Keys )
            {
                if ( k.Key == Common::KeyCode::Down || k.Key == Common::KeyCode::S )
                    step = 1;
                else if ( k.Key == Common::KeyCode::Up || k.Key == Common::KeyCode::W )
                    step = -1;
            }
            return step;
        }
    } // namespace Walk

    using namespace Walk;

    void BeginUIFrame( UIViewContext& view, entt::registry& reg, const Rect& viewportPx, float frameDtSeconds )
    {
        // This view is now looking at another scene. Entity ids are unique only inside a registry, so every
        // per-entity clock and every (canvas x view) cell the view holds would answer to ids that mean
        // something else here — drop them.
        if ( view.Registry != &reg )
        {
            view.Reset();
            view.Registry = &reg;
        }

        // A canvas destroyed since the last frame takes its cell with it, THIS frame. entt recycles entity
        // ids, so a cell left behind is not dead weight: the next canvas created can be handed that id and
        // would open on a stranger's screen with a stranger's hover clocks. Same shape as the preview that
        // held its renderer slot until something destroyed it (Docs/RENDERER_FRAME_STATE.md).
        view.RetireDeadCanvases( reg );

        // THIS VIEW's frame delta, advanced once per FRAME and not once per canvas — and handed in by the
        // host, which owns the frame's timestep (as FSlateApplication::Tick takes the engine's DeltaTime).
        // A clock read here measured how long the walk took rather than the step the frame stands for, so
        // a fixed-step run (--play) did not draw tick N on frame N and a slow build drifted the playheads.
        // Clamped so a long stall doesn't snap animations.
        view.FrameDt = std::clamp( frameDtSeconds, 0.0f, 0.1f );
        view.Time += view.FrameDt;
        ++view.FrameIndex; // drives the tween rewind-on-hide check

        // The scene's UI clips, stepped by the one view that owns scene time and evaluated by every view.
        view.Animation().Evaluate( reg, UIAnimationStep{ .DtSeconds = view.FrameDt,
                                                         .Advance   = view.DrivesSceneAnimation,
                                                         .GameWorld = view.GameWorld } );

        // A scene swap leaves the elected entity dangling — drop it rather than matching a recycled id.
        if ( view.Hot != entt::null && !reg.valid( view.Hot ) )
            view.Hot = entt::null;

        // The election is over the whole frame: every canvas of this view writes into it in draw order and
        // the topmost writer wins, which is what lets an overlay canvas take the pointer from the HUD.
        view.HotNext = entt::null;
        view.Focusables.clear();

        // Where this view draws, for the whole frame. Stated once here rather than handed to each canvas,
        // so the walks, the overlay placement and the drag ghost cannot be looking at different rectangles.
        view.ViewportPx = viewportPx;

        // Design <-> Preview: the other mode's open set is meaningless here, so it goes. See
        // UIViewContext::AuthoringLastFrame.
        if ( view.AuthoringPreview != view.AuthoringLastFrame )
        {
            CloseAllOverlays( view, reg );
            view.AuthoringLastFrame = view.AuthoringPreview;
        }

        // An authoring view shows every overlay where it was authored. Written into the CELLS rather than
        // consulted at each draw site, so "is this overlay showing" has one answer that both the drawing
        // walk and the layout walk read — see UIViewContext::AuthoringPreview.
        if ( view.AuthoringPreview )
        {
            for ( const entt::entity c : reg.view<ECS::UIOverlayComponent>() )
            {
                UICanvasContext& cell = view.CanvasState( c );
                cell.OverlayOpen      = true;
                cell.OverlayShift     = glm::vec2( 0.0f );
            }
        }
        view.FrameOpen = true;
    }

    Common::BoolResultStr RenderCanvas2D( UIViewContext& view, entt::registry& reg, entt::entity canvasEntity,
                                          Graphic::Render2D::DrawList2D& dl, const glm::mat4* worldViewProj,
                                          const UIInput* input, std::string* outClicked, entt::entity* focused )
    {
        // The frame's viewport, not this call's: one view is one framebuffer, and BeginUIFrame is where that
        // is said. The FrameOpen check below is what guarantees it has been said before this is read.
        const Rect& viewportPx = view.ViewportPx;
        // The canvas is the caller's answer, checked before anything else touches the context. Electing one
        // here — which is what this function did, `*reg.view<UICanvasComponent>().begin()` — meant a scene's
        // second canvas was drawn by nothing and reported by nothing.
        if ( canvasEntity == entt::null || !reg.valid( canvasEntity ) )
            return Common::MakeFormattedError( "[UI] RenderCanvas2D was given no canvas to draw (entity {})",
                                               static_cast<std::uint32_t>( canvasEntity ) );
        if ( !reg.has<ECS::UICanvasComponent>( canvasEntity ) )
            return Common::MakeFormattedError(
                 "[UI] RenderCanvas2D was given entity {} as a canvas, but it carries no UICanvasComponent",
                 static_cast<std::uint32_t>( canvasEntity ) );

        // Refused rather than drawn: without a BeginUIFrame the view's clock never advances, so the canvas
        // would come out looking right and standing perfectly still — the silent wrong answer, in the one
        // shape a frame cannot show.
        if ( !view.FrameOpen )
            return Common::MakeFormattedError(
                 "[UI] RenderCanvas2D was called for canvas {} outside a frame of its view; call "
                 "BeginUIFrame / EndUIFrame around the frame's canvases",
                 static_cast<std::uint32_t>( canvasEntity ) );
        if ( view.Registry != &reg )
            return Common::MakeFormattedError(
                 "[UI] RenderCanvas2D was given a registry the open frame does not belong to (canvas {})",
                 static_cast<std::uint32_t>( canvasEntity ) );

        // THE PAIR, BOUND HERE AND NOWHERE ELSE: this view's own cell for this canvas.
        WalkCtx ctx{ view, view.CanvasState( canvasEntity ), CanvasStyle{} };
        ctx.Root = &dl;
        ResolveRetainerMasks( ctx, reg );

        const auto& canvasData = reg.get<ECS::UICanvasComponent>( canvasEntity ).Data;
        if ( !canvasData.Visible )
            return Common::MakeSuccess( false ); // a canvas that asked not to be drawn, not a failure

        // --- Overlays (Ю12) -------------------------------------------------------------------------
        // A closed overlay is a canvas the runtime is not showing YET, which is a different statement from
        // UICanvasData::Visible being false — that one is the author saying never. Success(false), because
        // the canvas was named correctly and simply has no pixels this frame, exactly like a WorldSpace
        // canvas behind the camera.
        const UIOverlayData* overlay = OverlayDataOf( reg, canvasEntity );
        if ( overlay != nullptr && !ctx.Canvas.OverlayOpen )
            return Common::MakeSuccess( false );
        // THE CANVAS'S THEME, RESOLVED ONCE PER WALK (Ю13). Asked of the service by handle every frame
        // rather than cached in the cell, which is what makes a theme switch — a different handle in the
        // slot, or a hot reload of the same file — reach the very next frame with no scene reload and no
        // invalidation to remember. An empty slot answers nullptr and every element then draws the colours
        // its author typed, which is the state of every canvas authored before themes existed.
        {
            ctx.Style = CanvasStyle( view.Resources().Theme( canvasData.Theme ), canvasData.FontScale,
                                     canvasData.HighContrast );

            // The refusals below are accumulated against ONE theme. Pointing the slot at another one must
            // let the same name be reported again — it may be a typo against this theme and a real style in
            // that one, and a set that outlived the theme it was built for would silence exactly that.
            if ( ctx.Canvas.WarnedStylesTheme != canvasData.Theme )
            {
                ctx.Canvas.WarnedStyles.clear();
                ctx.Canvas.WarnedStylesTheme = canvasData.Theme;
            }
        }

        Rect  canvasRect;
        float scale;
        if ( canvasData.RenderMode == UICanvasRenderMode::WorldSpace && worldViewProj &&
             reg.has<ECS::TransformComponent>( canvasEntity ) )
        {
            // Billboard: project the canvas entity's world position to the screen, centre + distance-scale it
            // (mirrors the ImGui renderer so world-space UI matches).
            const glm::vec3 wpos = glm::vec3( reg.get<ECS::TransformComponent>( canvasEntity ).GetTransform()[3] );
            const glm::vec4 clip = ( *worldViewProj ) * glm::vec4( wpos, 1.0f );
            if ( clip.w <= 0.0001f )
                // Behind the camera: the canvas is real and was asked for correctly, it simply has no pixels
                // this frame. That is success(false), not an error and not success(true).
                return Common::MakeSuccess( false );
            const float sx = viewportPx.X + ( clip.x / clip.w * 0.5f + 0.5f ) * viewportPx.W;
            const float sy = viewportPx.Y + ( 1.0f - ( clip.y / clip.w * 0.5f + 0.5f ) ) * viewportPx.H;
            const float k  = canvasData.WorldScale / clip.w;
            const float w  = canvasData.ReferenceWidth * k;
            const float h  = canvasData.ReferenceHeight * k;
            canvasRect     = Rect{ sx - w * 0.5f, sy - h * 0.5f, w, h };
            scale          = k;
        }
        else
        {
            const CanvasFit fit = ResolveCanvas( canvasData, viewportPx );
            canvasRect          = fit.Root;
            scale               = fit.Scale;
        }

        // --- Overlay placement and the modal scrim ---------------------------------------------------
        if ( overlay != nullptr )
        {
            // ONE DISPLACEMENT MOVES THE WHOLE AUTHORED TREE. A tooltip beside the cursor, a submenu beside
            // its item and a menu at the click point are all the same operation on the canvas root, which is
            // why there is one placement mechanism rather than four. UICanvasLayout::EnumerateCanvas applies
            // the identical shift for the identical cell, so where it draws and where it can be clicked stay
            // one answer.
            canvasRect.X += ctx.Canvas.OverlayShift.x;
            canvasRect.Y += ctx.Canvas.OverlayShift.y;

            if ( overlay->Kind == UIOverlayKind::Modal )
            {
                // THE SCRIM IS WHAT MAKES A MODAL MODAL, and it is two things at once.
                //
                // It is a rectangle over the WHOLE view, drawn before this canvas's own content so the
                // dialog sits on top of its own dim. And it is an ELECTION: for every point the dialog does
                // not cover, the frame's single hot element becomes this canvas itself. A button underneath
                // compares against that election exactly as it always has and finds it is not the winner, so
                // "the click does not reach what is below" is a consequence of Ю4's one-election-per-view
                // rule and not a second rule beside it. There is no `if ( modalOpen )` anywhere in the
                // controls, because there is nowhere for one to live.
                //
                // The election does NOT depend on the scrim being visible. A fully transparent scrim still
                // captures — a modal that asked for no dim is still a modal — and tying capture to alpha
                // would make Scrim Opacity two settings wearing one name.
                const float     a = std::clamp( overlay->ScrimOpacity, 0.0f, 1.0f );
                const glm::vec4 scrim( overlay->ScrimColor, a );
                if ( a > 0.0f )
                    dl.AddRectFilled( { viewportPx.X, viewportPx.Y },
                                      { viewportPx.X + viewportPx.W, viewportPx.Y + viewportPx.H },
                                      Tinted( ctx, scrim ) );
                if ( input && PointIn( viewportPx, input->MousePx ) )
                {
                    ctx.View.HotNext     = canvasEntity;
                    ctx.View.HotNextRect = viewportPx;
                }
            }
        }

        // The canvas's own Background Sprite: the full-canvas backdrop, drawn under everything and OUTSIDE
        // the safe area (a notch inset is where content must not go, not where the wallpaper stops). It goes
        // in before the children so anything they draw lands on top of it.
        //
        // This field was reflected, serialized and shown in Details for its whole life and NOTHING read it —
        // section 1.3, a dead setting. It has no colour of its own, so unlike a panel it cannot fall back to
        // a flat fill when the handle does not resolve: that would paint an opaque white sheet over the
        // scene. It draws only what it can resolve, and says so once when it cannot.
        if ( HandleSet( canvasData.Sprite ) )
        {
            TextureRef bg = ResolveAnimatedFrame( view.Resources(), canvasData.Sprite );
            if ( !bg )
                bg = ResolveSpriteImage( view.Resources(), canvasData.Sprite );
            if ( bg )
            {
                dl.AddImage( bg.Id, { canvasRect.X, canvasRect.Y },
                             { canvasRect.X + canvasRect.W, canvasRect.Y + canvasRect.H }, { 0.0f, 0.0f },
                             { 1.0f, 1.0f }, glm::vec4( 1.0f ) );
                ctx.Canvas.WarnedBackground = Assets::AssetHandle{};
            }
            else if ( ctx.Canvas.WarnedBackground != canvasData.Sprite )
            {
                // Once per handle, not once per frame — a background that never resolves would otherwise
                // write a log line at frame rate.
                ctx.Canvas.WarnedBackground = canvasData.Sprite;
                LOG_ERROR( "[UI] canvas Background Sprite {} did not resolve to an image; the canvas draws "
                           "no backdrop this frame",
                           static_cast<uint64_t>( canvasData.Sprite ) );
            }
        }

        // Top-level content lays out inside the safe area (mobile notches); 0 insets = full canvas.
        const Rect childRoot = InsetRect( canvasRect, canvasData.SafeArea.x * scale, canvasData.SafeArea.y * scale,
                                          canvasData.SafeArea.z * scale, canvasData.SafeArea.w * scale );

        // --- Screen machine: seed on first use, then advance the running transition ---
        {
            if ( reg.has<ECS::UIScreenStackComponent>( canvasEntity ) )
            {
                const auto& st  = reg.get<ECS::UIScreenStackComponent>( canvasEntity ).Data;
                ctx.Canvas.ScreenTime    = st.TransitionTime;
                ctx.Canvas.ScreenSlidePx = st.SlidePx;
                ctx.Canvas.ScreenEasing  = st.Easing;
                if ( ctx.Canvas.Screen.empty() )
                    ctx.Canvas.Screen = st.InitialScreen;
            }
            // Seed, or re-seed when the current name doesn't exist here — otherwise a name left over from
            // another scene would hide every screen in this one.
            //
            // THE STEADY STATE IS ASKED OF THE SCREENS, NOT OF THE TREE, and that is a measurement and not
            // a tidy-up. This used to be one ForEachScreenName over the whole canvas sub-tree, EVERY FRAME
            // — 60 001 entities visited on a canvas holding a 20 000-row list, which is 8.6 ms of a Debug
            // frame spent deciding that a screen machine nobody is using still has the name it had. It is
            // also precisely the residue that would have made Ю17's claim false: a container that walks a
            // window of sixteen rows buys nothing while the frame around it still walks all of them.
            //
            // The question "is ctx.Canvas.Screen one of THIS canvas's screens" is answered by the screens
            // themselves — there are a handful of UIScreenComponents in a scene, and each names its canvas
            // by walking UP, which is the depth of the tree and not its size. The sub-tree walk survives
            // for the RE-SEED, where the answer has to be the first screen in DRAW order and a pool's
            // iteration order is creation order; that path runs once per canvas per view rather than once
            // per frame.
            //
            // MEMBERSHIP IS ASKED THE SAME WAY THE SUB-TREE WALK ANSWERED IT — "is canvasEntity an
            // ancestor of this screen", not "is it the screen's NEAREST canvas". The two differ for a
            // canvas nested under another canvas, which nothing authors today; using CanvasOf here would
            // have been a second, unrelated behaviour change smuggled into a cost fix.
            const auto underCanvas = [&reg, canvasEntity]( entt::entity e )
            {
                // Bounded rather than trusting the tree to be acyclic, for the reason CanvasOf states.
                const std::size_t limit = reg.size() + 1;
                std::size_t       steps = 0;
                for ( entt::entity cur = e; cur != entt::null && reg.valid( cur ) && steps < limit; ++steps )
                {
                    if ( cur == canvasEntity )
                    {
                        return true;
                    }
                    cur = reg.has<ECS::RelationshipComponent>( cur )
                               ? reg.get<ECS::RelationshipComponent>( cur ).Parent
                               : entt::null;
                }
                return false;
            };

            bool currentExists = false;
            bool anyScreenHere = false;
            for ( const entt::entity s : reg.view<ECS::UIScreenComponent>() )
            {
                const std::string& n = reg.get<ECS::UIScreenComponent>( s ).Data.Name;
                if ( n.empty() || !underCanvas( s ) )
                {
                    continue;
                }
                anyScreenHere = true;
                if ( n == ctx.Canvas.Screen )
                {
                    currentExists = true;
                    break;
                }
            }
            if ( anyScreenHere && !currentExists )
            {
                std::string firstScreen;
                ForEachScreenName( reg, canvasEntity,
                                   [&firstScreen]( const std::string& n )
                                   {
                                       if ( firstScreen.empty() )
                                       {
                                           firstScreen = n;
                                       }
                                   } );
                if ( !firstScreen.empty() )
                {
                    ctx.Canvas.Screen = firstScreen;
                    ctx.Canvas.ScreenFrom.clear();
                    ctx.Canvas.ScreenStack.clear();
                    ctx.Canvas.ScreenT = 1.0f;
                }
            }
            if ( ctx.Canvas.ScreenT < 1.0f )
            {
                ctx.Canvas.ScreenT =
                     ctx.Canvas.ScreenTime > 0.0f
                          ? std::min( 1.0f, ctx.Canvas.ScreenT + ctx.View.FrameDt / ctx.Canvas.ScreenTime )
                          : 1.0f;
                if ( ctx.Canvas.ScreenT >= 1.0f )
                    ctx.Canvas.ScreenFrom.clear(); // hand-over finished; the outgoing screen stops drawing
            }
        }

        std::vector<PopupInfo> popups;
        // The canvas is drawn into the viewport and nowhere else, so that is the outermost clip both halves
        // start from. Built as a region rather than a Rect so every level below narrows ONE type.
        Graphic::Render2D::ClipRegion2D rootClip;
        (void)Graphic::Render2D::IntersectClipRegion(
             rootClip, glm::mat3( 1.0f ), { viewportPx.X, viewportPx.Y },
             { viewportPx.X + viewportPx.W, viewportPx.Y + viewportPx.H } );
        // A TOOLTIP AND A TOAST ARE INERT TO INPUT, BY CONSTRUCTION AND NOT BY AUTHORING DISCIPLINE.
        //
        // Both are drawn above everything, so if they could be elected they would take the pointer from the
        // very thing they are about: a tooltip that catches the cursor loses the hover that opened it and
        // closes itself, one frame on and one frame off forever. A toast in a corner would swallow a click
        // on the HUD beneath it, which is precisely "stealing focus". Neither contributes a focusable
        // either, so Tab cannot walk into a notification that is about to disappear.
        //
        // A context menu and a modal are the opposite: capturing the pointer IS what they are for.
        const bool inert = overlay != nullptr &&
                           ( overlay->Kind == UIOverlayKind::Tooltip || overlay->Kind == UIOverlayKind::Toast );
        if ( reg.has<ECS::RelationshipComponent>( canvasEntity ) )
            for ( auto c : reg.get<ECS::RelationshipComponent>( canvasEntity ).Children )
                if ( reg.valid( c ) )
                    DrawElement( ctx, reg, c, childRoot, scale, dl, input, outClicked, focused, &popups,
                                 inert ? nullptr : &ctx.View.Focusables, rootClip, HitScope{ !inert } );

        // A ShowScreen / BackScreen button fired during the walk: start the hand-over now, so the very
        // next frame already draws both screens mid-transition.
        if ( !ctx.Canvas.ScreenReq.empty() || ctx.Canvas.ScreenReqBack )
        {
            if ( ctx.Canvas.ScreenReqBack )
            {
                if ( !ctx.Canvas.ScreenStack.empty() ) // at the bottom of the stack Back is simply ignored
                {
                    ctx.Canvas.ScreenFrom = ctx.Canvas.Screen;
                    ctx.Canvas.Screen     = ctx.Canvas.ScreenStack.back();
                    ctx.Canvas.ScreenStack.pop_back();
                    ctx.Canvas.ScreenT    = 0.0f;
                    ctx.Canvas.ScreenBack = true;
                }
            }
            else if ( ctx.Canvas.ScreenReq != ctx.Canvas.Screen )
            {
                ctx.Canvas.ScreenStack.push_back( ctx.Canvas.Screen );
                ctx.Canvas.ScreenFrom = ctx.Canvas.Screen;
                ctx.Canvas.Screen     = ctx.Canvas.ScreenReq;
                ctx.Canvas.ScreenT    = 0.0f;
                ctx.Canvas.ScreenBack = false;
            }
            ctx.Canvas.ScreenReq.clear();
            ctx.Canvas.ScreenReqBack = false;
        }

        // Open dropdown option lists, drawn LAST so they overlay everything.
        for ( const PopupInfo& pi : popups )
        {
            if ( !reg.valid( pi.Entity ) || !reg.has<ECS::UIDropdownComponent>( pi.Entity ) )
                continue;
            auto&       d       = reg.get<ECS::UIDropdownComponent>( pi.Entity ).Data;
            const auto  options = SplitOptions( ctx.View.Resources().Text(), d.Options );
            const float rowH    = pi.Box.H;
            const Rect  popup{ pi.Box.X, pi.Box.Y + pi.Box.H, pi.Box.W,
                              rowH * static_cast<float>( options.size() ) };
            dl.AddRectFilled( { popup.X, popup.Y }, { popup.X + popup.W, popup.Y + popup.H },
                              glm::vec4( pi.Style.Color( StyleSlot::DropdownBackground, d.Background ), 1.0f ),
                              pi.Style.Metric( StyleSlot::DropdownCornerRadius, d.CornerRadius ) * pi.Scale );

            bool clickedOption = false;
            for ( std::size_t i = 0; i < options.size(); ++i )
            {
                const Rect row{ popup.X, popup.Y + static_cast<float>( i ) * rowH, popup.W, rowH };
                const bool hover = input && input->MousePx.x >= row.X && input->MousePx.x <= row.X + row.W &&
                                   input->MousePx.y >= row.Y && input->MousePx.y <= row.Y + row.H;
                if ( hover )
                    dl.AddRectFilled(
                         { row.X, row.Y }, { row.X + row.W, row.Y + row.H },
                         glm::vec4( pi.Style.Color( StyleSlot::DropdownHighlight, d.Highlight ), 1.0f ) );
                UITextData td;
                td.Text     = options[i];
                td.FontSize = pi.Style.FontSize( StyleSlot::DropdownFont, d.FontSize );
                td.Color    = pi.Style.Color( StyleSlot::DropdownText, d.TextColor );
                td.Font     = pi.Style.Font( StyleSlot::DropdownFont, Assets::AssetHandle{} );
                td.Align    = UITextAlign::Left;
                DrawText2D( ctx.View.Resources(), dl, td, row, pi.Scale, ctx.View.Tint, ctx.View.Time );
                if ( hover && input->MouseReleased )
                {
                    d.SelectedIndex = static_cast<int>( i );
                    d.Open          = false;
                    clickedOption   = true;
                }
            }
            // A click outside both the popup and the box closes it (the box click is toggled in the walk).
            if ( input && input->MouseReleased && !clickedOption )
            {
                const bool inPopup = input->MousePx.x >= popup.X && input->MousePx.x <= popup.X + popup.W &&
                                     input->MousePx.y >= popup.Y && input->MousePx.y <= popup.Y + popup.H;
                const bool inBox = input->MousePx.x >= pi.Box.X && input->MousePx.x <= pi.Box.X + pi.Box.W &&
                                   input->MousePx.y >= pi.Box.Y && input->MousePx.y <= pi.Box.Y + pi.Box.H;
                if ( !inPopup && !inBox )
                    d.Open = false;
            }
        }

        return Common::MakeSuccess( true );
    }

    void EndUIFrame( UIViewContext& view, entt::registry& reg, Graphic::Render2D::DrawList2D& dl,
                     const UIInput* input, entt::entity* focused, std::string* outClicked,
                     std::vector<std::string>* outMessages )
    {
        // Named, not shrugged off: closing a frame that was never opened would hand over an election nobody
        // made and fire enter/exit against a stale one. It is a caller bug and it is reported as one.
        if ( !view.FrameOpen )
        {
            LOG_ERROR( "[UI] EndUIFrame was called with no frame of this view open; every canvas of a frame "
                       "must sit between BeginUIFrame and EndUIFrame" );
            return;
        }

        // --- Pointer events, drag & drop -------------------------------------------------------------
        // Everything here runs on the freshly elected hot element, AFTER the tree is laid out: enter/exit
        // edges, press/release callbacks, and the drag lifecycle. Messages go out through the same channel
        // as button actions, so a host that dispatches those handles these for free.
        if ( input )
        {
            auto emit = [&]( const std::string& msg )
            {
                if ( msg.empty() )
                    return;
                if ( outMessages )
                    outMessages->push_back( msg );
                else if ( outClicked && outClicked->empty() )
                    *outClicked = msg;
            };
            auto events = [&]( entt::entity e ) -> const UIPointerEventsData*
            {
                return ( e != entt::null && reg.valid( e ) && reg.has<ECS::UIPointerEventsComponent>( e ) )
                            ? &reg.get<ECS::UIPointerEventsComponent>( e ).Data
                            : nullptr;
            };

            // The hit-test axis already says what the pointer does with an element, and the routing must
            // read that answer rather than invent a second one. An element with no UILayout — the canvas
            // itself is the only one — takes the default, so a canvas-level listener is reachable.
            auto hitTestOf = [&]( entt::entity e )
            {
                return ( e != entt::null && reg.valid( e ) && reg.has<ECS::UILayoutComponent>( e ) )
                            ? reg.get<ECS::UILayoutComponent>( e ).Data.HitTest
                            : UIHitTest::All;
            };

            // May @p e hear a pointer event about ITSELF? Only All. ChildrenOnly is transparent to the
            // pointer, so telling it about a press it cannot receive would contradict the field that says
            // it cannot; Blocking responds to nothing by definition. Either may still sit on the route of a
            // descendant that does respond — being silent is not the same as being absent.
            auto respondsToPointer = [&]( entt::entity e )
            { return e != entt::null && reg.valid( e ) && hitTestOf( e ) == UIHitTest::All; };

            // The route of an event aimed at @p target: the chain from the canvas down to it, ANCESTORS
            // FIRST. Built by walking Parent and reversing, because that is the only direction the
            // relationship stores and a tree has exactly one path to its root.
            auto chainOf = [&]( entt::entity target )
            {
                std::vector<entt::entity> chain;
                for ( entt::entity t = target; t != entt::null && reg.valid( t ); )
                {
                    chain.push_back( t );
                    t = reg.has<ECS::RelationshipComponent>( t ) ? reg.get<ECS::RelationshipComponent>( t ).Parent
                                                                 : entt::null;
                }
                std::reverse( chain.begin(), chain.end() );
                return chain;
            };

            // One step of a route. Returns true when the route must end here — which is a property of the
            // listener and NOT of whether it had anything to say, so an element may swallow an event while
            // emitting nothing.
            auto step = [&]( entt::entity e, UIEventPhase phase, std::string UIPointerEventsData::*msg )
            {
                const auto* ev = events( e );
                if ( ev == nullptr || ev->Phase != phase || !respondsToPointer( e ) )
                    return false;
                emit( ev->*msg );
                return ev->StopPropagation;
            };

            // Tunnel down the chain, then bubble back up it. Two passes over one chain rather than two
            // chains, so an element cannot be reached in one pass and missed in the other.
            auto route = [&]( entt::entity target, std::string UIPointerEventsData::*msg )
            {
                // Blocking STOPS THE POINTER, and a routed press IS that pointer, so it stops here for the
                // ancestors too: a greyed-out form or a modal scrim that let the canvas behind it hear the
                // click would be blocking for the hit test and not blocking for the event the hit test
                // produced, which is one word meaning two things. It can only ever be the TARGET — a
                // Blocking element closes its sub-tree to election, so nothing under it is ever hot.
                //
                // Enter/Exit are deliberately NOT subject to this. They report where the pointer IS, which
                // is a question about geometry, while a press asks who HANDLES it, which is what Blocking
                // is a statement about. Silencing the whole chain on hover instead would fire Exit on every
                // ancestor the moment the pointer crossed onto a blocked child and Enter again when it
                // left — the very flicker the chain-difference rule exists to prevent.
                if ( hitTestOf( target ) == UIHitTest::Blocking )
                    return;

                const std::vector<entt::entity> chain = chainOf( target );
                for ( std::size_t i = 0; i < chain.size(); ++i )
                    if ( step( chain[i], UIEventPhase::Tunnel, msg ) )
                        return;
                for ( std::size_t i = chain.size(); i-- > 0; )
                    if ( step( chain[i], UIEventPhase::Bubble, msg ) )
                        return;
            };

            if ( view.HotNext != view.Hot ) // the pointer crossed a boundary this frame
            {
                // Enter/Exit are the DIFFERENCE of the two chains, not a route (see UIPointerEventsData).
                // The shared prefix is everything the pointer never left, so a move between two children of
                // one panel reports nothing about the panel.
                const std::vector<entt::entity> from = chainOf( view.Hot );
                const std::vector<entt::entity> to   = chainOf( view.HotNext );

                std::size_t common = 0;
                while ( common < from.size() && common < to.size() && from[common] == to[common] )
                    ++common;

                // Leave innermost-first and enter outermost-first: the pointer crosses one boundary at a
                // time, and it crosses them in that order.
                for ( std::size_t i = from.size(); i-- > common; )
                    if ( const auto* ev = events( from[i] ); ev != nullptr && respondsToPointer( from[i] ) )
                        emit( ev->OnExitMessage );
                for ( std::size_t i = common; i < to.size(); ++i )
                    if ( const auto* ev = events( to[i] ); ev != nullptr && respondsToPointer( to[i] ) )
                        emit( ev->OnEnterMessage );
            }

            const bool pressed = input->MouseDown && !view.PrevDown; // UIInput carries held + release only
            if ( pressed )
            {
                route( view.HotNext, &UIPointerEventsData::OnDownMessage );

                // Start a drag from a draggable element. The ghost is the source's own footprint, so the
                // cursor carries something the size of what it picked up.
                if ( view.HotNext != entt::null && reg.valid( view.HotNext ) &&
                     reg.has<ECS::UIDraggableComponent>( view.HotNext ) )
                {
                    // Only PENDING for now — a press that never moves is a click, not a drag.
                    const auto& d      = reg.get<ECS::UIDraggableComponent>( view.HotNext ).Data;
                    view.Drag.Pending  = true;
                    view.Drag.Source   = view.HotNext;
                    view.Drag.Payload  = d.Payload;
                    view.Drag.Ghost    = d.GhostOpacity;
                    view.Drag.Size     = { view.HotNextRect.W, view.HotNextRect.H };
                    view.Drag.PressPos = input->MousePx;
                }
            }
            // Promote the pending press to a real drag once the pointer travels far enough.
            if ( view.Drag.Pending && !view.Drag.Active && input->MouseDown )
            {
                constexpr float kDragStartPx = 4.0f;
                if ( glm::length( input->MousePx - view.Drag.PressPos ) > kDragStartPx )
                    view.Drag.Active = true;
            }

            if ( input->MouseReleased )
            {
                route( view.HotNext, &UIPointerEventsData::OnUpMessage );

                if ( view.Drag.Active )
                {
                    // Drop on the element under the cursor, or on the nearest ancestor that accepts — a
                    // target is usually a panel whose children are what you actually point at.
                    for ( entt::entity t = view.HotNext; t != entt::null && reg.valid( t ); )
                    {
                        if ( reg.has<ECS::UIDropTargetComponent>( t ) )
                        {
                            const auto& dt = reg.get<ECS::UIDropTargetComponent>( t ).Data;
                            if ( Accepts( dt, view.Drag.Payload ) && t != view.Drag.Source )
                            {
                                emit( dt.OnDropMessage.empty() ? view.Drag.Payload
                                                               : dt.OnDropMessage + "|" + view.Drag.Payload );
                                break;
                            }
                        }
                        t = reg.has<ECS::RelationshipComponent>( t )
                                 ? reg.get<ECS::RelationshipComponent>( t ).Parent
                                 : entt::null;
                    }
                }
                view.Drag = UIDragState{}; // a plain click on a draggable ends here too
            }
            // THE OVERLAY MACHINE RUNS ON THIS FRAME'S ELECTION AND ON THIS FRAME'S PRESS EDGE — so it has
            // to be here, BEFORE PrevDown is overwritten with the current state. Running it after cost an
            // afternoon: right-click worked (the machine keeps its own PrevRightDown) and every left-button
            // path silently never fired, because `MouseDown && !PrevDown` is false once PrevDown has been
            // brought up to date. An authoring view has no machine at all — it shows every overlay as
            // authored (UIViewContext::AuthoringPreview) — and running a pointer machine over a pointer it
            // does not have would open menus nobody clicked.
            if ( !view.AuthoringPreview )
                UpdateOverlays( view, reg, *input );

            view.PrevDown = input->MouseDown;

            // The ghost rides on top of everything, drawn after the tree so nothing overlaps it.
            if ( view.Drag.Active )
            {
                const glm::vec2 half = view.Drag.Size * 0.5f;
                const glm::vec2 mn   = input->MousePx - half;
                const glm::vec2 mx   = input->MousePx + half;
                // The ghost is drawn AFTER the walk, so there is no WalkCtx in scope to resolve through.
                // It takes the DRAG SOURCE's style — the ghost is a picture of that element — and it gets
                // there by rebuilding the source's own (canvas x view) cell rather than by a second copy
                // of StyleFor's rules, which would be two statements of "which style does this element
                // resolve through" and therefore two that can disagree.
                ElementStyle ghostStyle;
                if ( reg.valid( view.Drag.Source ) )
                {
                    const entt::entity ghostCanvas = CanvasOf( reg, view.Drag.Source );
                    if ( ghostCanvas != entt::null && reg.has<ECS::UICanvasComponent>( ghostCanvas ) )
                    {
                        const auto& ghostCanvasData = reg.get<ECS::UICanvasComponent>( ghostCanvas ).Data;
                        WalkCtx     ghostCtx{ view, view.CanvasState( ghostCanvas ),
                                          CanvasStyle( view.Resources().Theme( ghostCanvasData.Theme ),
                                                           ghostCanvasData.FontScale, ghostCanvasData.HighContrast ) };
                        ghostStyle = StyleFor( ghostCtx, reg, view.Drag.Source );
                    }
                }
                dl.AddRectFilled(
                     mn, mx,
                     glm::vec4( ghostStyle.Color( StyleSlot::DragGhost, glm::vec3( 0.35f, 0.55f, 0.85f ) ),
                                view.Drag.Ghost * 0.6f ),
                     6.0f );
                dl.AddRect(
                     mn, mx,
                     glm::vec4( ghostStyle.Color( StyleSlot::DragGhostBorder, glm::vec3( 0.75f, 0.87f, 1.0f ) ),
                                view.Drag.Ghost ),
                     2.0f );
            }
        }

        // The election is handed over ONCE per frame, after every canvas of the view has written into it —
        // the topmost writer in draw order wins, across canvases. Doing this per walk gave the answer to
        // whichever canvas happened to be drawn last. HotNext is cleared by the next BeginUIFrame rather
        // than here, so it stays readable between the two.
        view.Hot = view.HotNext;

        // Tab and Down/S advance keyboard focus to the next focusable control, Up/W steps back (both wrap;
        // effective next frame). The list spans every canvas of the frame, so focus can leave a HUD and enter
        // an overlay. With nothing focused, either direction lands on the FIRST control — the top of a menu.
        int step = 0;
        if ( input != nullptr )
            step = input->Pressed( Common::KeyCode::Tab ) ? 1 : NavigateStep( *input );
        if ( focused != nullptr && step != 0 && !view.Focusables.empty() )
        {
            const std::size_t n   = view.Focusables.size();
            std::size_t       idx = 0; // not-found -> focus the first
            for ( std::size_t i = 0; i < n; ++i )
                if ( view.Focusables[i] == *focused )
                {
                    idx = step > 0 ? ( i + 1 ) % n : ( i + n - 1 ) % n;
                    break;
                }
            *focused = view.Focusables[idx];
        }

        view.FrameOpen = false;
    }
} // namespace Desert::UI
