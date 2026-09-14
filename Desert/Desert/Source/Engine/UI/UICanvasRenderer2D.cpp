#include "UICanvasRenderer2D.hpp"

#include <Engine/ECS/Components.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/ResourceRegistry.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/Text/FontBaker.hpp>
#include <Engine/Text/Utf8.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UIDataStore.hpp>

#include <Common/Core/Logger.hpp>

#include <algorithm>
#include <optional>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Desert::UI
{
    namespace
    {
        bool HandleSet( const Assets::AssetHandle& h )
        {
            return static_cast<uint64_t>( h ) != 0;
        }

        // Seconds since the first UI frame — a shared wall clock so every time-driven effect (pulse, marquee,
        // hover eases) animates without any per-frame dt being plumbed through the stateless walk.
        //
        // THE ONE STATIC LEFT IN THIS FILE, and deliberately: it is a CLOCK, not state. Every view reads it
        // and keeps its own last reading in its context, which is what a shared clock has to look like once
        // two views draw in one frame — the previous arrangement kept the last reading here too, so of two
        // walks in a frame the second measured no time at all. Nothing here is written after the first call.
        //
        // Its consequence is worth knowing before comparing frames: a canvas with a marquee or a running
        // tween is NOT byte-reproducible run to run, because its phase comes from this clock rather than
        // from the frame counter. Measured on MainMenu (one marquee): 0.63-0.85% of pixels differ between
        // two runs of the same binary, and 0.405% even at 400 frames when the intro tween has settled.
        // UI_ElementProbe has no marquee and no tween, and its floor is exactly 0.
        float NowSeconds()
        {
            static const auto epoch = std::chrono::steady_clock::now();
            return std::chrono::duration<float>( std::chrono::steady_clock::now() - epoch ).count();
        }

        // ONE CANVAS BEING WALKED BY ONE VIEW — the two coordinates of the key, bound together for the
        // length of the walk. They travel as one argument rather than two so that no recursion step can
        // pick up the wrong half: every ctx.Canvas below is guaranteed to be ctx.View's own cell for the
        // canvas being drawn, because RenderCanvas2D is the only place that builds one and it builds it
        // from view.CanvasState( canvas ).
        struct WalkCtx
        {
            UIViewContext&   View;
            UICanvasContext& Canvas;
        };

        // Per-button hover interpolation (0=rest, 1=hovered), eased each frame toward the target so hover
        // colours cross-fade instead of snapping. The clock is keyed by entity INSIDE the view's context —
        // entt::entity is unique only within a registry, so a map shared between views answered to entity 7
        // of every scene at once.
        float HoverEase( WalkCtx& ctx, entt::entity e, bool hovered )
        {
            float&      t = ctx.Canvas.HoverT[e];
            const float k = std::clamp( ctx.View.FrameDt * 12.0f, 0.0f, 1.0f ); // exponential approach
            t += ( ( hovered ? 1.0f : 0.0f ) - t ) * k;
            return t;
        }

        // --- Screens ----------------------------------------------------------------------------------
        // A canvas can hold several UIScreen sub-trees; exactly one is current, and a ShowScreen button
        // moves between them (BackScreen returns). Like the tweens, the live state is kept in the view's
        // context and not in the component: navigating in the editor must not rewrite the authored scene.
        void RequestScreen( WalkCtx& ctx, const std::string& name, bool back )
        {
            ctx.Canvas.ScreenReq     = name;
            ctx.Canvas.ScreenReqBack = back;
        }

        float Ease( ECS::UIEasing e, float t )
        {
            t = std::clamp( t, 0.0f, 1.0f );
            switch ( e )
            {
                case ECS::UIEasing::QuadIn:
                    return t * t;
                case ECS::UIEasing::QuadOut:
                    return 1.0f - ( 1.0f - t ) * ( 1.0f - t );
                case ECS::UIEasing::QuadInOut:
                    return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * ( 1.0f - t ) * ( 1.0f - t );
                case ECS::UIEasing::CubicIn:
                    return t * t * t;
                case ECS::UIEasing::CubicOut:
                    return 1.0f - std::pow( 1.0f - t, 3.0f );
                case ECS::UIEasing::CubicInOut:
                    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow( -2.0f * t + 2.0f, 3.0f ) * 0.5f;
                case ECS::UIEasing::BackOut:
                {
                    constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
                    return 1.0f + c3 * std::pow( t - 1.0f, 3.0f ) + c1 * std::pow( t - 1.0f, 2.0f );
                }
                case ECS::UIEasing::ElasticOut:
                {
                    if ( t <= 0.0f || t >= 1.0f )
                        return t;
                    constexpr float c4 = 2.0f * 3.14159265f / 3.0f;
                    return std::pow( 2.0f, -10.0f * t ) * std::sin( ( t * 10.0f - 0.75f ) * c4 ) + 1.0f;
                }
                case ECS::UIEasing::BounceOut:
                {
                    constexpr float n1 = 7.5625f, d1 = 2.75f;
                    if ( t < 1.0f / d1 )
                        return n1 * t * t;
                    if ( t < 2.0f / d1 )
                    {
                        t -= 1.5f / d1;
                        return n1 * t * t + 0.75f;
                    }
                    if ( t < 2.5f / d1 )
                    {
                        t -= 2.25f / d1;
                        return n1 * t * t + 0.9375f;
                    }
                    t -= 2.625f / d1;
                    return n1 * t * t + 0.984375f;
                }
                case ECS::UIEasing::Linear:
                default:
                    return t;
            }
        }

        // What a tween contributes this frame. Identity when the element has none.
        struct TweenSample
        {
            glm::vec2 Offset{ 0.0f }; // design px
            glm::vec2 Size{ 0.0f };   // design px
            glm::vec4 Tint{ 1.0f };   // multiplies colour + alpha
        };

        // A generic from->to animation, evaluated here on the way to the screen and NEVER written back
        // into the authored component — so a tween is safe to run in the editor, previews live in Design
        // mode, and stopping it simply restores the authored look. Its playhead therefore lives in the
        // view's context, which is what lets two views animate the same element independently.
        TweenSample SampleTween( WalkCtx& ctx, entt::registry& reg, entt::entity e )
        {
            TweenSample out;
            if ( !reg.has<ECS::UITweenComponent>( e ) )
                return out;
            const auto& tw = reg.get<ECS::UITweenComponent>( e ).Data;

            float&    clock    = ctx.Canvas.TweenT[e];
            uint64_t& lastSeen = ctx.Canvas.TweenSeen[e];
            // Not evaluated last frame => this element was hidden (or the canvas was). Replaying from the
            // top is what an intro tween should do when its screen comes back.
            if ( tw.RewindOnHide && lastSeen + 1 != ctx.View.FrameIndex )
                clock = 0.0f;
            lastSeen = ctx.View.FrameIndex;

            if ( tw.Playing )
                clock += ctx.View.FrameDt;

            const float dur = std::max( 0.001f, tw.Duration );
            float       t   = ( clock - tw.Delay ) / dur; // <0 while delayed
            switch ( tw.Loop )
            {
                case ECS::UITweenLoop::Loop:
                    t = t > 0.0f ? std::fmod( t, 1.0f ) : 0.0f;
                    break;
                case ECS::UITweenLoop::PingPong:
                {
                    if ( t > 0.0f )
                    {
                        const float cycle = std::fmod( t, 2.0f );
                        t                 = cycle <= 1.0f ? cycle : 2.0f - cycle;
                    }
                    else
                        t = 0.0f;
                    break;
                }
                case ECS::UITweenLoop::Once:
                default:
                    t = std::clamp( t, 0.0f, 1.0f );
                    break;
            }

            const glm::vec4 v = glm::mix( tw.From, tw.To, Ease( tw.Easing, t ) );
            switch ( tw.Property )
            {
                case ECS::UITweenProperty::Offset:
                    out.Offset = glm::vec2( v );
                    break;
                case ECS::UITweenProperty::Size:
                    out.Size = glm::vec2( v );
                    break;
                case ECS::UITweenProperty::Opacity:
                    out.Tint.a = v.x;
                    break;
                case ECS::UITweenProperty::Color:
                    out.Tint = glm::vec4( glm::vec3( v ), 1.0f );
                    break;
            }
            return out;
        }

        // A keyed CLIP (UIAnim) on top of the one-shot tween: several property lanes, many keys, one
        // playhead. Segments ease with the key they arrive at, so an author shapes each leg separately.
        void ApplyAnimClip( WalkCtx& ctx, entt::registry& reg, entt::entity e, TweenSample& out )
        {
            if ( !reg.has<ECS::UIAnimComponent>( e ) )
                return;
            auto& clip = reg.get<ECS::UIAnimComponent>( e ).Data;

            // The playhead is a runtime field (never serialized): the canvas drives it while Playing, and
            // the Sequencer pauses playback and writes it directly to scrub.
            //
            // It is the one clock in this walk that lives in the SCENE rather than in the view, so only the
            // view that owns the scene's time advances it. Let both an editor viewport and the UI Editor
            // preview advance it and every clip runs at twice its authored speed — the mirror image of the
            // bug the context fixes, and the reason DrivesSceneAnimation is a field and not an assumption.
            if ( clip.Playing && ctx.View.DrivesSceneAnimation )
            {
                clip.Time += ctx.View.FrameDt;
                if ( clip.Duration > 0.0f )
                    clip.Time =
                         clip.Loop ? std::fmod( clip.Time, clip.Duration ) : std::min( clip.Time, clip.Duration );
            }

            for ( const ECS::UIAnimTrack& tr : clip.Tracks )
            {
                if ( tr.Keys.empty() )
                    continue;

                glm::vec4 v = tr.Keys.front().Value;
                if ( clip.Time >= tr.Keys.back().Time )
                {
                    v = tr.Keys.back().Value;
                }
                else
                {
                    for ( size_t i = 1; i < tr.Keys.size(); ++i )
                    {
                        const ECS::UIAnimKey& k0 = tr.Keys[i - 1];
                        const ECS::UIAnimKey& k1 = tr.Keys[i];
                        if ( clip.Time < k0.Time || clip.Time > k1.Time )
                            continue;
                        const float span = k1.Time - k0.Time;
                        const float u    = span > 1e-6f ? ( clip.Time - k0.Time ) / span : 1.0f;
                        v                = glm::mix( k0.Value, k1.Value, Ease( k1.Easing, u ) );
                        break;
                    }
                }

                switch ( tr.Property )
                {
                    case ECS::UITweenProperty::Offset:
                        out.Offset += glm::vec2( v );
                        break;
                    case ECS::UITweenProperty::Size:
                        out.Size += glm::vec2( v );
                        break;
                    case ECS::UITweenProperty::Opacity:
                        out.Tint.a *= v.x;
                        break;
                    case ECS::UITweenProperty::Color:
                        out.Tint *= glm::vec4( glm::vec3( v ), 1.0f );
                        break;
                }
            }
        }

        // --- Data binding (MVVM-lite) -----------------------------------------------------------------
        // Ties an element to a key in the UI data store. Like the tweens, the bound value is applied on
        // the way to the screen and never written back, so gameplay can drive a label without the scene
        // ever being modified.
        struct BindingSample
        {
            bool                       Hide = false; // Visible target said no
            std::optional<std::string> Text;         // Text target, string value
            std::optional<double>      Number;       // Text target, numeric value
            std::optional<float>       Value;        // Slider / ProgressBar target
        };

        BindingSample SampleBinding( entt::registry& reg, entt::entity e, TweenSample& tw )
        {
            BindingSample out;
            if ( !reg.has<ECS::UIBindingComponent>( e ) )
                return out;
            const auto& b = reg.get<ECS::UIBindingComponent>( e ).Data;
            if ( b.Key.empty() )
                return out;

            const UIDataStore& store = UIDataStore::Get();
            switch ( b.Target )
            {
                case ECS::UIBindTarget::Text:
                {
                    // A NUMBER AND A STRING ARE DIFFERENT ARGUMENTS, not two spellings of one. A number is
                    // the count a translation's plural form is chosen from and what `{n}` prints in the
                    // reader's own locale; a string simply stands in for the authored text (and is then
                    // resolved on the same terms, so gameplay can write a key into the store and have it
                    // translated). Deciding between them here rather than at the draw site keeps the two
                    // in one place.
                    //
                    // `store.Text` converts a number to text, so it is asked SECOND: asking it first would
                    // turn every numeric binding into a C-locale string and lose the count.
                    if ( const auto n = store.Number( b.Key ) )
                    {
                        out.Number = *n;
                        break;
                    }
                    if ( const auto t = store.Text( b.Key ) )
                        out.Text = *t;
                    break;
                }
                case ECS::UIBindTarget::Value:
                    if ( const auto n = store.Number( b.Key ) )
                        out.Value = static_cast<float>( *n );
                    break;
                case ECS::UIBindTarget::Opacity:
                    if ( const auto n = store.Number( b.Key ) )
                        tw.Tint.a *= std::clamp( static_cast<float>( *n ), 0.0f, 1.0f );
                    break;
                case ECS::UIBindTarget::Color:
                    if ( const auto c = store.Color( b.Key ) )
                        tw.Tint *= glm::vec4( *c, 1.0f );
                    break;
                case ECS::UIBindTarget::Visible:
                    if ( const auto v = store.Bool( b.Key ) )
                        out.Hide = !*v;
                    break;
            }
            return out;
        }

        /**
         * @brief THE ONE PLACE AN AUTHORED UI STRING BECOMES A DRAWN STRING.
         *
         * Every label on a canvas goes through here, which is what makes the subsystem's decisive relation
         * true by construction rather than by discipline: a key is translated, a literal is not, and no
         * draw site gets to decide differently.
         *
         * The bound value, when there is one, stands in for the authored text and is then resolved on the
         * SAME terms — so gameplay writing a key into the data store gets a translation, and gameplay
         * writing a player's name gets the name.
         */
        std::string ResolveLabel( const std::string& authored, const BindingSample& binding )
        {
            Localization::FormatArguments args;
            if ( binding.Number )
                args.Count = *binding.Number;

            const std::string_view source =
                 binding.Text ? std::string_view( *binding.Text ) : std::string_view( authored );
            const auto resolved = Localization::Localization::Get().Resolve( source, args );

            // A number bound to a LITERAL label replaces it, formatted for the reader's locale — that is
            // what "bind this number to this label" has always meant here, and it is now locale-aware
            // instead of going through a C-locale printf. A number bound to a KEY is the key's `{n}`
            // argument instead, which the resolve above has already spent.
            if ( binding.Number && !binding.Text &&
                 resolved.Outcome == Localization::Localization::Outcome::Literal )
            {
                return Localization::FormatNumber( Localization::Localization::Get().Language(), *binding.Number,
                                                   0 );
            }
            return resolved.Text;
        }

        // The tint of the element being drawn lives in the context (UICanvasContext::Tint) — multiplied into
        // its colours so Opacity/Color tweens reach every control. The two draw helpers outside the walk
        // (DrawText2D, DrawIcon) take the already-tinted colour as an argument rather than reading it, so
        // they need no context at all.
        glm::vec4 Tinted( const WalkCtx& ctx, const glm::vec4& c )
        {
            return c * ctx.View.Tint;
        }

        // --- Hit testing ------------------------------------------------------------------------------
        // Controls used to each test the cursor against their own rect, so two overlapping ones both lit
        // up. Instead the walk elects a single HOT element: every raycast-target whose rect (and clip)
        // contains the pointer overwrites the candidate, and since children draw after parents the last
        // writer is the topmost. Controls compare against the PREVIOUS frame's winner — the same one-frame
        // deferral ImGui uses, which avoids a second layout pass and is invisible in practice. The election
        // and the drag both live in UICanvasContext (Hot / HotNext / Drag).

        bool PointIn( const Rect& r, const glm::vec2& p )
        {
            return p.x >= r.X && p.x <= r.X + r.W && p.y >= r.Y && p.y <= r.Y + r.H;
        }

        // Does this target accept the payload in flight? An empty filter takes anything.
        bool Accepts( const ECS::UIDropTargetData& t, const std::string& payload )
        {
            return t.Accepts.empty() || payload.rfind( t.Accepts, 0 ) == 0;
        }

        // Split a ';'-separated option string into its items (empty items skipped).
        //
        // EACH ITEM IS RESOLVED ON ITS OWN (Ю15), not the list: a dropdown mixes translated labels with
        // proper nouns — a server name, a player's own preset — far more often than it is wholly one or
        // the other, and a per-list rule would force the author to choose. The separator is not part of
        // any item, so a key never contains one.
        std::vector<std::string> SplitOptions( const std::string& s )
        {
            std::vector<std::string> out;
            std::string              cur;
            for ( char c : s )
            {
                if ( c == ';' )
                {
                    if ( !cur.empty() )
                        out.push_back( Localization::Localization::Get().Resolve( cur ).Text );
                    cur.clear();
                }
                else
                    cur += c;
            }
            if ( !cur.empty() )
                out.push_back( Localization::Localization::Get().Resolve( cur ).Text );
            return out;
        }

        // What an ancestor's UIHitTest allows this sub-tree to do — the half of the hit-test axis that is
        // INHERITED. Both old booleans were read off the element alone and the walk descended regardless,
        // which is why "this element and everything under it is invisible to the pointer" and "grey out
        // this whole panel" could not be said at all: the flag had to be cleared by hand on every
        // descendant. Passed down the recursion, never up, and only ever narrowed — a sub-tree can lose
        // permission and never regain it, so no child can re-enable itself inside a disabled dialog.
        //
        // IT IS ONE BOOLEAN AND NOT TWO, and that was measured rather than assumed. The obvious shape is a
        // pair — may this sub-tree be ELECTED, may it RESPOND — but the second carries no information the
        // first does not: reacting to anything, pointer or keyboard, is gated on `interactive` below, and
        // `interactive` already ANDs in the inherited term through `electsSelf`. A mutation that removed a
        // separate inherited "respond" left every test green, which is what a field with no observable
        // effect looks like (DC 1.3), so it is not here.
        struct HitScope
        {
            bool Elect = true; // may anything in here become the hot element (i.e. stop the pointer)?
        };

        // An open dropdown whose option list is drawn AFTER the whole tree, so it overlays everything.
        struct PopupInfo
        {
            entt::entity Entity;
            Rect         Box;   // the dropdown's box rect (screen px)
            float        Scale; // canvas scale for its text
        };

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

        // Keyboard-focusable controls (Tab cycles between them; Enter activates the focused one).
        bool IsFocusable( entt::registry& reg, entt::entity e )
        {
            return reg.has<ECS::UIButtonComponent>( e ) || reg.has<ECS::UIInputFieldComponent>( e ) ||
                   reg.has<ECS::UIToggleComponent>( e ) || reg.has<ECS::UISliderComponent>( e ) ||
                   reg.has<ECS::UIDropdownComponent>( e );
        }

        // Resolve a sprite AssetHandle to its runtime GPU Image2D (non-owning; the image service owns it and
        // Render2D keys its per-texture executor by the raw pointer). nullptr when unset / unresolvable.
        Graphic::Image2D* ResolveSpriteImage( const Assets::AssetHandle& handle )
        {
            if ( !HandleSet( handle ) )
                return nullptr;
            auto* texService = Runtime::ResourceRegistry::GetTextureService();
            if ( !texService )
                return nullptr;
            auto* tex = texService->Get( handle );
            if ( !tex )
                return nullptr;
            auto* imgService = Runtime::ResourceRegistry::GetImageService();
            if ( !imgService )
                return nullptr;
            return static_cast<Graphic::Image2D*>( imgService->Resolve( tex->GetImageHandle() ) );
        }

        // Resolve an element's UI-material slot to the entry Render2D will draw it with, or nullptr when
        // the slot is unset.
        //
        // THE ONE CASE THAT IS NOT AN ERROR AND IS STILL REPORTED: a walk with no GPU backend behind it
        // (`ctx.View.Materials == nullptr`) — a unit test, or a host that never wired one. The element then
        // draws its ordinary fill, which is a FALLBACK, so it is named. Reported once per view because a
        // per-frame line buries the log and gets the whole message ignored; the picture is what keeps
        // saying it, every frame.
        const void* ResolveUIMaterial( WalkCtx& ctx, entt::entity e, const Assets::AssetHandle& handle )
        {
            if ( !HandleSet( handle ) )
                return nullptr;

            if ( !ctx.View.Materials )
            {
                if ( ctx.View.WarnedMaterial != handle )
                {
                    ctx.View.WarnedMaterial = handle;
                    LOG_WARN( "[UI] element {} has material {} but this view has no 2D backend, so it "
                              "draws its plain fill instead",
                              static_cast<uint32_t>( e ), static_cast<uint64_t>( handle ) );
                }
                return nullptr;
            }

            return ctx.View.Materials->ResolveMaterial( handle );
        }

        // An animated (GIF) sprite's current frame — a pure function of wall-clock time. Non-GIF handles
        // resolve to nullptr here, so ordinary textures fall through to ResolveSpriteImage.
        Graphic::Image2D* ResolveAnimatedFrame( const Assets::AssetHandle& handle )
        {
            auto* animService = Runtime::ResourceRegistry::GetAnimatedImageService();
            return animService ? animService->Resolve( handle ) : nullptr;
        }

        // Draw a filled UI box: a sprite (tinted by `color`) when one is bound + resolvable, else a flat
        // colour. `srcBorder` (L,T,R,B in SOURCE pixels) enables 9-slice — corners stay unstretched (x
        // scale), edges/centre stretch — so image panels/buttons resize without distorting their borders.
        // Mirrors the ImGui DrawBox so both render paths look identical.
        void DrawBox( Graphic::Render2D::DrawList2D& dl, const glm::vec2& mn, const glm::vec2& mx,
                      const glm::vec4& color, const Assets::AssetHandle& sprite, const glm::vec4& srcBorder,
                      float scale, float rounding )
        {
            // An animated sprite plays stretched to the box; static sprites / 9-slice keep the path below.
            if ( Graphic::Image2D* frame = ResolveAnimatedFrame( sprite ) )
            {
                dl.AddImage( frame, mn, mx, { 0.0f, 0.0f }, { 1.0f, 1.0f }, color );
                return;
            }

            Graphic::Image2D* img = ResolveSpriteImage( sprite );
            if ( !img )
            {
                dl.AddRectFilled( mn, mx, color, rounding );
                return;
            }

            const void* tex = img;
            const float tw  = static_cast<float>( img->GetWidth() );
            const float th  = static_cast<float>( img->GetHeight() );
            const bool  nine =
                 tw > 0.0f && th > 0.0f &&
                 ( srcBorder.x > 0.0f || srcBorder.y > 0.0f || srcBorder.z > 0.0f || srcBorder.w > 0.0f );
            if ( !nine )
            {
                dl.AddImage( tex, mn, mx, { 0.0f, 0.0f }, { 1.0f, 1.0f }, color );
                return;
            }

            const float hw = ( mx.x - mn.x ) * 0.5f, hh = ( mx.y - mn.y ) * 0.5f;
            const float pl = std::min( srcBorder.x * scale, hw ), pt = std::min( srcBorder.y * scale, hh );
            const float pr = std::min( srcBorder.z * scale, hw ), pb = std::min( srcBorder.w * scale, hh );
            const float xs[4] = { mn.x, mn.x + pl, mx.x - pr, mx.x };
            const float ys[4] = { mn.y, mn.y + pt, mx.y - pb, mx.y };
            const float us[4] = { 0.0f, srcBorder.x / tw, 1.0f - srcBorder.z / tw, 1.0f };
            const float vs[4] = { 0.0f, srcBorder.y / th, 1.0f - srcBorder.w / th, 1.0f };
            for ( int r = 0; r < 3; ++r )
                for ( int c = 0; c < 3; ++c )
                    dl.AddImage( tex, { xs[c], ys[r] }, { xs[c + 1], ys[r + 1] }, { us[c], vs[r] },
                                 { us[c + 1], vs[r + 1] }, color );
        }

        // --- Text layout (Phase E: rich text + word-wrap + auto-size + vertical align + overflow) --------
        // Lays a UIText string into SDF glyph quads inside `rect`. `scale` maps design px -> screen px; the SDF
        // stays crisp at any resulting size. Handles the per-element font asset, multi-line + word-wrap,
        // line-spacing, horizontal/vertical alignment, auto-size, overflow (ellipsis/clip) and rich text.

        // One character carrying its resolved rich-text style. `bold` => faux-bold (the glyph is drawn a
        // second time nudged in X, since the atlas has a single weight).
        struct StyledChar
        {
            uint32_t  ch    = 0; // Unicode codepoint (NOT a byte — see Engine/Text/Utf8)
            glm::vec4 color = glm::vec4( 1.0f );
            bool      bold  = false;
        };

        // A laid-out line: its characters plus the total advance width in EM units (baked-pixel space, before
        // the on-screen scale `s` is applied). Kept in em so auto-size can rescale without re-measuring glyphs.
        struct TextLine
        {
            std::vector<StyledChar> chars;
            float                   width = 0.0f;
        };

        // A positioned glyph quad in pixel space — computed once, then drawn for shadow / outline / main so
        // every pass stays in perfect lock-step.
        struct PlacedGlyph
        {
            float     x0, y0, x1, y1;
            float     u0, v0, u1, v1;
            glm::vec4 color;
            bool      bold;
        };

        // "rrggbb" or "rrggbbaa" -> vec4. false (and out untouched) on any malformed input.
        bool ParseHexColor( const std::string& hex, glm::vec4& out )
        {
            auto nib = []( char c ) -> int
            {
                if ( c >= '0' && c <= '9' )
                    return c - '0';
                if ( c >= 'a' && c <= 'f' )
                    return c - 'a' + 10;
                if ( c >= 'A' && c <= 'F' )
                    return c - 'A' + 10;
                return -1;
            };
            if ( hex.size() != 6 && hex.size() != 8 )
                return false;
            int v[8];
            for ( size_t k = 0; k < hex.size(); ++k )
                if ( ( v[k] = nib( hex[k] ) ) < 0 )
                    return false;
            out.r = ( v[0] * 16 + v[1] ) / 255.0f;
            out.g = ( v[2] * 16 + v[3] ) / 255.0f;
            out.b = ( v[4] * 16 + v[5] ) / 255.0f;
            out.a = hex.size() == 8 ? ( v[6] * 16 + v[7] ) / 255.0f : 1.0f;
            return true;
        }

        // Expand text into styled characters. When `rich`, parse a BBCode subset — [color=#rrggbb]..[/color]
        // (nestable) and [b]..[/b] — consuming the tags; an unrecognised '[' is emitted literally.
        std::vector<StyledChar> BuildStyledChars( const std::string& text, const glm::vec4& baseColor, bool rich )
        {
            std::vector<StyledChar> out;
            out.reserve( text.size() );
            if ( !rich )
            {
                for ( size_t i = 0; i < text.size(); )
                    out.push_back( { Text::Utf8Next( text, i ), baseColor, false } );
                return out;
            }

            std::vector<glm::vec4> colorStack{ baseColor };
            int                    boldDepth = 0;
            for ( size_t i = 0; i < text.size(); )
            {
                if ( text[i] == '[' )
                {
                    const size_t close = text.find( ']', i );
                    if ( close != std::string::npos )
                    {
                        const std::string tag     = text.substr( i + 1, close - i - 1 );
                        bool              handled = true;
                        if ( tag == "b" )
                            ++boldDepth;
                        else if ( tag == "/b" )
                            boldDepth = std::max( 0, boldDepth - 1 );
                        else if ( tag == "/color" )
                        {
                            if ( colorStack.size() > 1 )
                                colorStack.pop_back();
                        }
                        else if ( tag.rfind( "color=#", 0 ) == 0 )
                        {
                            glm::vec4 c = colorStack.back();
                            ParseHexColor( tag.substr( 7 ), c ); // keep parent colour on malformed hex
                            colorStack.push_back( c );
                        }
                        else
                            handled = false;

                        if ( handled )
                        {
                            i = close + 1;
                            continue;
                        }
                    }
                }
                out.push_back( { Text::Utf8Next( text, i ), colorStack.back(), boldDepth > 0 } );
            }
            return out;
        }

        // Greedy word-wrap into lines. `adv(ch)` gives a glyph's em advance; explicit '\n' always breaks.
        // A word longer than maxWidthEm overflows its own line rather than being character-split.
        template <typename AdvFn>
        std::vector<TextLine> LayoutLines( const std::vector<StyledChar>& chars, AdvFn adv, float maxWidthEm,
                                           bool wrap )
        {
            std::vector<TextLine>   lines;
            TextLine                cur;
            std::vector<StyledChar> word;
            float                   wordW = 0.0f;

            auto flushWord = [&]()
            {
                for ( const auto& c : word )
                {
                    cur.chars.push_back( c );
                    cur.width += adv( c.ch );
                }
                word.clear();
                wordW = 0.0f;
            };
            auto trimTrailingSpaces = [&]()
            {
                while ( !cur.chars.empty() && cur.chars.back().ch == ' ' )
                {
                    cur.width -= adv( ' ' );
                    cur.chars.pop_back();
                }
            };
            auto pushLine = [&]()
            {
                lines.push_back( std::move( cur ) );
                cur = TextLine{};
            };

            for ( const StyledChar& c : chars )
            {
                if ( c.ch == '\n' )
                {
                    flushWord();
                    pushLine();
                    continue;
                }
                if ( c.ch == ' ' )
                {
                    flushWord();
                    cur.chars.push_back( c );
                    cur.width += adv( ' ' );
                    continue;
                }
                word.push_back( c );
                wordW += adv( c.ch );
                if ( wrap && !cur.chars.empty() && ( cur.width + wordW ) > maxWidthEm )
                {
                    trimTrailingSpaces();
                    pushLine(); // the pending word carries over and starts the fresh line
                }
            }
            flushWord();
            trimTrailingSpaces();
            lines.push_back( std::move( cur ) );
            return lines;
        }

        // @p tint is the caller's accumulated element tint (UICanvasContext::Tint), passed in rather than
        // read from a global so this helper stays a pure function of its arguments.
        void DrawText2D( Graphic::Render2D::DrawList2D& dl, const ECS::UITextData& t, const Rect& rect,
                         float scale, const glm::vec4& tint )
        {
            if ( t.Text.empty() )
                return;

            auto* fontService = Runtime::ResourceRegistry::GetFontService();
            if ( !fontService )
                return;
            // Font is an asset handle on the element (drag-drop / preloaded); unset falls back to the default.
            const uint64_t fontHandle = static_cast<uint64_t>( t.Font ) != 0 ? static_cast<uint64_t>( t.Font )
                                                                             : fontService->DefaultFontHandle();
            // Non-ASCII (Cyrillic, CJK, …) is only in the atlas if it was asked for: request this string's
            // codepoints first, so a re-bake — if any — happens before the font is resolved and the text
            // draws correctly on its very first frame instead of a frame late.
            fontService->RequestGlyphs( fontHandle, Text::Utf8Decode( t.Text ) );

            Runtime::Font* font = fontService->Get( fontHandle, Text::kDefaultBakePixelHeight );
            if ( !font || !font->Atlas || !font->Baked.Valid() || font->Baked.PixelHeight <= 0.0f )
                return;

            const Text::BakedFont& bf    = font->Baked;
            const void*            atlas = font->Atlas.get();

            auto glyph = [&]( uint32_t ch ) -> const Text::Glyph*
            {
                const auto it = bf.Glyphs.find( ch );
                return it == bf.Glyphs.end() ? nullptr : &it->second;
            };
            auto advEm = [&]( uint32_t ch ) -> float
            {
                const Text::Glyph* g = glyph( ch );
                return g ? g->Advance : 0.0f;
            };

            const std::vector<StyledChar> chars =
                 BuildStyledChars( t.Text, glm::vec4( t.Color, 1.0f ) * tint, t.RichText );

            // Marquee: a single clipped line scrolling leftward, repeated seamlessly across the width. A news
            // ticker / running banner. Pure function of the shared clock, so it needs no per-frame state.
            if ( t.Marquee )
            {
                const float sM       = ( t.FontSize * scale ) / bf.PixelHeight;
                float       contentW = 0.0f;
                for ( const StyledChar& sc : chars )
                    contentW += advEm( sc.ch ) * sM;
                const float gap    = std::max( 40.0f * scale, rect.W * 0.35f );
                const float period = std::max( 1.0f, contentW + gap );
                const float off    = std::fmod( NowSeconds() * t.MarqueeSpeed * scale, period );
                const float blockH = ( bf.Ascent - bf.Descent ) * sM;
                const float baseY  = rect.Y + ( rect.H - blockH ) * 0.5f + bf.Ascent * sM;

                dl.PushClipRect( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } );
                for ( float startX = rect.X - off; startX < rect.X + rect.W; startX += period )
                {
                    float penX = startX;
                    for ( const StyledChar& sc : chars )
                    {
                        const Text::Glyph* g = glyph( sc.ch );
                        if ( !g )
                            continue;
                        if ( g->Width > 0.0f && g->Height > 0.0f )
                        {
                            const float x0 = penX + g->OffsetX * sM;
                            const float y0 = baseY + g->OffsetY * sM;
                            dl.AddText( atlas, { x0, y0 }, { x0 + g->Width * sM, y0 + g->Height * sM },
                                        { g->U0, g->V0 }, { g->U1, g->V1 }, sc.color );
                        }
                        penX += g->Advance * sM;
                    }
                }
                dl.PopClipRect();
                return;
            }

            const float pad    = 6.0f;
            const float availW = std::max( 1.0f, rect.W - 2.0f * pad );
            const float availH = std::max( 1.0f, rect.H - 2.0f * pad );
            // Em-space vertical advance per line: the font's line box times the user's line-spacing multiplier.
            const float lineHemEm = ( bf.Ascent - bf.Descent ) * std::max( 0.1f, t.LineSpacing );

            auto layoutAt = [&]( float s ) -> std::vector<TextLine>
            {
                const float threshold = t.Wrap ? availW / s : std::numeric_limits<float>::max();
                return LayoutLines( chars, advEm, threshold, t.Wrap );
            };

            float                 s     = ( t.FontSize * scale ) / bf.PixelHeight;
            std::vector<TextLine> lines = layoutAt( s );

            if ( t.AutoSize )
            {
                // Shrink toward the floor until the block fits height (and width when not wrapping).
                const float minS = std::max( 0.01f, ( t.MinFontSize * scale ) / bf.PixelHeight );
                for ( int iter = 0; iter < 24; ++iter )
                {
                    float maxLineEm = 0.0f;
                    for ( const auto& ln : lines )
                        maxLineEm = std::max( maxLineEm, ln.width );
                    const float blockH = lines.empty() ? 0.0f
                                                       : ( static_cast<float>( lines.size() - 1 ) * lineHemEm * s +
                                                           ( bf.Ascent - bf.Descent ) * s );
                    const bool  fitsH  = blockH <= availH;
                    const bool  fitsW  = t.Wrap || ( maxLineEm * s <= availW );
                    if ( ( fitsH && fitsW ) || s <= minS )
                        break;
                    s     = std::max( minS, s * 0.92f );
                    lines = layoutAt( s );
                }
            }

            const float lineStep = lineHemEm * s;
            if ( lineStep <= 0.0f )
                return;

            // Overflow: drop the lines that fall past the bottom (Ellipsis/Clip), tagging the tail as truncated.
            bool truncated = false;
            if ( t.Overflow != ECS::UITextOverflow::Overflow )
            {
                const size_t maxVisible =
                     std::max<size_t>( 1, static_cast<size_t>( std::floor( availH / lineStep ) ) );
                if ( lines.size() > maxVisible )
                {
                    lines.resize( maxVisible );
                    truncated = ( t.Overflow == ECS::UITextOverflow::Ellipsis );
                }
            }

            // Ellipsis: trim trailing glyphs off any over-wide line (and the truncated tail) and append "...".
            if ( t.Overflow == ECS::UITextOverflow::Ellipsis )
            {
                const float dotAdv    = advEm( '.' );
                auto        ellipsize = [&]( TextLine& ln )
                {
                    const float dots = 3.0f * dotAdv;
                    while ( !ln.chars.empty() && ( ln.width + dots ) * s > availW )
                    {
                        ln.width -= advEm( ln.chars.back().ch );
                        ln.chars.pop_back();
                    }
                    const glm::vec4 col = ln.chars.empty() ? glm::vec4( t.Color, 1.0f ) : ln.chars.back().color;
                    for ( int k = 0; k < 3; ++k )
                    {
                        ln.chars.push_back( { '.', col, false } );
                        ln.width += dotAdv;
                    }
                };
                if ( dotAdv > 0.0f )
                {
                    if ( !t.Wrap )
                        for ( auto& ln : lines )
                            if ( ln.width * s > availW )
                                ellipsize( ln );
                    if ( truncated && !lines.empty() )
                        ellipsize( lines.back() );
                }
            }

            // Vertical placement of the whole block within the rect.
            const float blockH =
                 lines.empty()
                      ? 0.0f
                      : ( static_cast<float>( lines.size() - 1 ) * lineStep + ( bf.Ascent - bf.Descent ) * s );
            float blockTop = rect.Y + pad; // Top
            if ( t.VerticalAlign == ECS::UITextVAlign::Middle )
                blockTop = rect.Y + ( rect.H - blockH ) * 0.5f;
            else if ( t.VerticalAlign == ECS::UITextVAlign::Bottom )
                blockTop = rect.Y + rect.H - pad - blockH;

            // Position every glyph (per-line horizontal alignment), collecting quads for the draw passes.
            std::vector<PlacedGlyph> placed;
            for ( size_t i = 0; i < lines.size(); ++i )
            {
                const float lineWpx = lines[i].width * s;
                float       penX    = rect.X + pad; // Left
                if ( t.Align == ECS::UITextAlign::Center )
                    penX = rect.X + ( rect.W - lineWpx ) * 0.5f;
                else if ( t.Align == ECS::UITextAlign::Right )
                    penX = rect.X + rect.W - pad - lineWpx;
                const float baselineY = blockTop + static_cast<float>( i ) * lineStep + bf.Ascent * s;

                for ( const StyledChar& sc : lines[i].chars )
                {
                    const Text::Glyph* g = glyph( sc.ch );
                    if ( !g )
                        continue;
                    if ( g->Width > 0.0f && g->Height > 0.0f )
                    {
                        // OffsetY is the glyph top relative to the baseline, Y-down (negative above baseline).
                        const float x0 = penX + g->OffsetX * s;
                        const float y0 = baselineY + g->OffsetY * s;
                        placed.push_back( { x0, y0, x0 + g->Width * s, y0 + g->Height * s, g->U0, g->V0, g->U1,
                                            g->V1, glm::vec4( glm::vec3( sc.color ), sc.color.a ), sc.bold } );
                    }
                    penX += g->Advance * s;
                }
            }

            const bool clip = ( t.Overflow == ECS::UITextOverflow::Clip );
            if ( clip )
                dl.PushClipRect( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } );

            auto quad = [&]( const PlacedGlyph& g, const glm::vec2& off, const glm::vec4& col )
            {
                dl.AddText( atlas, { g.x0 + off.x, g.y0 + off.y }, { g.x1 + off.x, g.y1 + off.y }, { g.u0, g.v0 },
                            { g.u1, g.v1 }, col );
            };

            if ( t.Shadow )
            {
                const glm::vec4 sc = glm::vec4( t.ShadowColor, 1.0f );
                const glm::vec2 so = t.ShadowOffset * scale;
                for ( const auto& g : placed )
                    quad( g, so, sc );
            }
            if ( t.Outline )
            {
                const glm::vec4 oc( t.OutlineColor, 1.0f );
                const float     ow = std::max( 1.0f, scale );
                for ( int ox = -1; ox <= 1; ++ox )
                    for ( int oy = -1; oy <= 1; ++oy )
                        if ( ox != 0 || oy != 0 )
                            for ( const auto& g : placed )
                                quad( g, { ox * ow, oy * ow }, oc );
            }
            for ( const auto& g : placed )
            {
                quad( g, { 0.0f, 0.0f }, g.color );
                if ( g.bold ) // faux-bold: a second pass nudged in X thickens the stroke
                    quad( g, { std::max( 0.5f, 0.6f * scale ), 0.0f }, g.color );
            }

            if ( clip )
                dl.PopClipRect();
        }

        // Width in px of `text` at `fontSizePx` in the default font (for the input caret). 0 if no font.
        float MeasureTextPx( const std::string& text, float fontSizePx )
        {
            auto* fs = Runtime::ResourceRegistry::GetFontService();
            if ( !fs )
                return 0.0f;
            Runtime::Font* font = fs->Get( fs->DefaultFontHandle(), Text::kDefaultBakePixelHeight );
            if ( !font || !font->Baked.Valid() )
                return 0.0f;
            const Text::BakedFont& bf = font->Baked;
            const float            s  = bf.PixelHeight > 0.0f ? fontSizePx / bf.PixelHeight : 0.0f;
            float                  w  = 0.0f;
            for ( size_t i = 0; i < text.size(); )
            {
                const auto it = bf.Glyphs.find( Text::Utf8Next( text, i ) );
                if ( it != bf.Glyphs.end() )
                    w += it->second.Advance * s;
            }
            return w;
        }

        // Remove the last UTF-8 codepoint (trailing continuation bytes + the lead/ASCII byte).
        void Utf8PopBack( std::string& s )
        {
            while ( !s.empty() && ( static_cast<unsigned char>( s.back() ) & 0xC0 ) == 0x80 )
                s.pop_back();
            if ( !s.empty() )
                s.pop_back();
        }

        // Maps the canvas to the viewport per its scale mode — mirrors ResolveCanvas in the ImGui renderer so
        // both paths agree on layout. Returns the canvas root rect (screen px) + the uniform scale applied to
        // every element's offsets / min-size.
        struct CanvasFit
        {
            Rect  Root;
            float Scale;
        };
        CanvasFit ResolveCanvas( const ECS::UICanvasData& d, const Rect& viewportPx )
        {
            switch ( d.ScaleMode )
            {
                case ECS::UICanvasScaleMode::ScaleWithScreen:
                {
                    const float sx = d.ReferenceWidth > 0.0f ? viewportPx.W / d.ReferenceWidth : 1.0f;
                    const float sy = d.ReferenceHeight > 0.0f ? viewportPx.H / d.ReferenceHeight : 1.0f;
                    const float m  = std::clamp( d.MatchWidthHeight, 0.0f, 1.0f );
                    return { viewportPx, sx * ( 1.0f - m ) + sy * m };
                }
                case ECS::UICanvasScaleMode::Letterbox:
                {
                    const Rect fit = CanvasRect( d.ReferenceWidth, d.ReferenceHeight, viewportPx.W, viewportPx.H );
                    const float scale = d.ReferenceWidth > 0.0f ? fit.W / d.ReferenceWidth : 1.0f;
                    return { Rect{ viewportPx.X + fit.X, viewportPx.Y + fit.Y, fit.W, fit.H }, scale };
                }
                case ECS::UICanvasScaleMode::Stretch:
                default:
                    return { viewportPx, 1.0f };
            }
        }

        // Draw an icon ASSET centred in `rect`, sized to `sizeFrac` of the shorter side. The .svg was
        // imported into an SDF once (Runtime::IconService), so this is a single quad through the very same
        // shader as text — crisp at any size, and outline/glow/shadow come along for free.
        // @p tint as in DrawText2D: the caller's accumulated element tint, an argument rather than a global.
        void DrawIcon( Graphic::Render2D::DrawList2D& dl, const ECS::UIIconData& ic, const Rect& rect,
                       const glm::vec4& tint )
        {
            auto* icons = Runtime::ResourceRegistry::GetIconService();
            if ( !icons )
                return;
            Runtime::Icon* icon = icons->Get( static_cast<uint64_t>( ic.Icon ) );
            if ( !icon || !icon->Valid() || !icons->Atlas() ) // unset/unreadable: draw nothing, no placeholder
                return;

            const float box = std::min( rect.W, rect.H ) * std::clamp( ic.Scale, 0.1f, 1.0f );
            if ( box <= 0.0f )
                return;
            // Fit the source aspect inside that box so a wide icon isn't stretched.
            const float     w = icon->Aspect >= 1.0f ? box : box * icon->Aspect;
            const float     h = icon->Aspect >= 1.0f ? box / icon->Aspect : box;
            const glm::vec2 c( rect.X + rect.W * 0.5f, rect.Y + rect.H * 0.5f );

            // One quad per colour run, painted back-to-front in document order. A monochrome icon is a
            // single white layer, so Color tints it outright; a multi-colour one keeps the fills the .svg
            // authored and Color multiplies them (white = exactly as drawn).
            const void* atlas = icons->Atlas().get();
            for ( const Runtime::IconLayer& layer : icon->Layers )
            {
                const glm::vec4 fill( static_cast<float>( ( layer.RGBA >> 24 ) & 0xFF ) / 255.0f,
                                      static_cast<float>( ( layer.RGBA >> 16 ) & 0xFF ) / 255.0f,
                                      static_cast<float>( ( layer.RGBA >> 8 ) & 0xFF ) / 255.0f,
                                      static_cast<float>( layer.RGBA & 0xFF ) / 255.0f );
                dl.AddText( atlas, { c.x - w * 0.5f, c.y - h * 0.5f }, { c.x + w * 0.5f, c.y + h * 0.5f },
                            { layer.U0, layer.V0 }, { layer.U1, layer.V1 },
                            glm::vec4( glm::vec3( fill ) * ic.Color, fill.a ) * tint );
            }
        }

        // Content size (px) a layout-group container needs to hug its children — for the Content Size Fitter.
        glm::vec2 GroupContentPx( entt::registry& reg, entt::entity e, float scale )
        {
            if ( !reg.has<ECS::UILayoutGroupComponent>( e ) || !reg.has<ECS::RelationshipComponent>( e ) )
                return { 0.0f, 0.0f };
            const auto&            g = reg.get<ECS::UILayoutGroupComponent>( e ).Data;
            std::vector<glm::vec2> sizes;
            for ( auto c : reg.get<ECS::RelationshipComponent>( e ).Children )
            {
                if ( !reg.valid( c ) || !TakesLayoutSpace( reg, c ) )
                    continue; // a Collapsed child has no slot, so it is not part of the content either
                glm::vec2 pref( 0.0f );
                if ( reg.has<ECS::UILayoutComponent>( c ) )
                {
                    const auto& L = reg.get<ECS::UILayoutComponent>( c ).Data;
                    pref          = glm::max( L.CustomMinimumSize, L.OffsetMax - L.OffsetMin );
                }
                sizes.push_back( pref * scale );
            }
            LayoutGroupParams params;
            params.Type     = g.Type == ECS::UILayoutType::Horizontal ? LayoutGroupType::Horizontal
                              : g.Type == ECS::UILayoutType::Grid     ? LayoutGroupType::Grid
                                                                      : LayoutGroupType::Vertical;
            params.PaddingL = g.Padding.x * scale;
            params.PaddingT = g.Padding.y * scale;
            params.PaddingR = g.Padding.z * scale;
            params.PaddingB = g.Padding.w * scale;
            params.Spacing  = g.Spacing * scale;
            params.CellSize = g.CellSize * scale;
            params.Columns  = g.Columns;
            return MeasureLayoutGroup( params, sizes );
        }

        // Recursively draw one element. `forcedRect` (non-null) is the rect assigned by a parent auto-layout
        // group — it overrides the element's own anchors for position + size.
        void DrawElement( WalkCtx& ctx, entt::registry& reg, entt::entity e, const Rect& parent, float scale,
                          Graphic::Render2D::DrawList2D& dl, const UIInput* input, std::string* outClicked,
                          entt::entity* focused, std::vector<PopupInfo>* popups,
                          std::vector<entt::entity>* focusables, const Graphic::Render2D::ClipRegion2D& clipRegion,
                          HitScope scope, const Rect* forcedRect = nullptr )
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
            if ( !IsElementVisible( reg, e ) )
                return;

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
                    const glm::vec2 content = GroupContentPx( reg, e, scale );
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
            ApplyAnimClip( ctx, reg, e, tween ); // a clip layers on top of the one-shot tween

            // A binding can hide the element outright — skip the sub-tree, input included.
            const BindingSample binding = SampleBinding( reg, e, tween );
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
            const auto ScreenBounds = [&dl]( const Rect& r )
            {
                if ( !dl.HasTransform() )
                    return r;
                glm::vec2 mn, mx;
                Graphic::Render2D::TransformedAABB2D( dl.GetTransform(), { r.X, r.Y }, { r.X + r.W, r.Y + r.H },
                                                      mn, mx );
                return Rect{ mn.x, mn.y, mx.x - mn.x, mx.y - mn.y };
            };

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
            const ECS::UIHitTest hitTest =
                 hasLayout ? reg.get<ECS::UILayoutComponent>( e ).Data.HitTest : ECS::UIHitTest::All;

            // Blocking elects itself precisely so the pointer STOPS here: it is the greyed-out form and the
            // modal dialog, which must swallow the click rather than let it reach what is behind them.
            const bool electsSelf =
                 scope.Elect && ( hitTest == ECS::UIHitTest::All || hitTest == ECS::UIHitTest::Blocking );
            // This element's own value only; the inherited half arrives through `electsSelf` above and is
            // ANDed in by `interactive` below.
            const bool responds = hitTest == ECS::UIHitTest::All || hitTest == ECS::UIHitTest::ChildrenOnly;

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
            const HitScope childScope{
                 scope.Elect && ( hitTest == ECS::UIHitTest::All || hitTest == ECS::UIHitTest::ChildrenOnly ) };

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

                // A drop target outlines itself while a drag it would accept is in flight.
                if ( ctx.View.Drag.Active && reg.has<ECS::UIDropTargetComponent>( e ) )
                {
                    const auto& dt = reg.get<ECS::UIDropTargetComponent>( e ).Data;
                    if ( Accepts( dt, ctx.View.Drag.Payload ) )
                        dl.AddRect( mn, mx, glm::vec4( dt.HighlightColor, hot ? 1.0f : 0.6f ), hot ? 3.0f : 2.0f );
                }

                // Panels and buttons render their sprite (single or 9-slice) tinted by the colour, or a flat
                // box when no sprite is bound. Button hover/press state needs input plumbing (a later slice),
                // so the normal state is drawn for now. Rounding / gradient / effects also come later.
                if ( reg.has<ECS::UIButtonComponent>( e ) )
                {
                    const auto& b     = reg.get<ECS::UIButtonComponent>( e ).Data;
                    // Disabled swallows all pointer/keyboard interaction and rests on the dim colour.
                    const bool hover = !b.Disabled && input && hot;
                    const bool down  = hover && input->MouseDown;
                    // Resting colour is Selected (persistent highlight) or Normal; hover cross-fades toward
                    // HoverColor (eased), press snaps to PressedColor, Disabled overrides everything.
                    const glm::vec3 rest = b.Selected ? b.SelectedColor : b.NormalColor;
                    const float     ht   = HoverEase( ctx, e, hover && !down );
                    const glm::vec3 c    = b.Disabled ? b.DisabledColor
                                           : down     ? b.PressedColor
                                                      : glm::mix( rest, b.HoverColor, ht );

                    // Image can change with state (hover / press), falling back to the normal Sprite.
                    Assets::AssetHandle spr = b.Sprite;
                    if ( down && HandleSet( b.PressedSprite ) )
                        spr = b.PressedSprite;
                    else if ( hover && HandleSet( b.HoverSprite ) )
                        spr = b.HoverSprite;
                    DrawBox( dl, mn, mx, Tinted( ctx, glm::vec4( c, b.Disabled ? 0.6f : 1.0f ) ), spr,
                             b.SpriteBorder, scale, 6.0f * scale );

                    // Selected accent: a rounded bar hugging the left edge (the "you are here" marker).
                    if ( b.Selected && !b.Disabled )
                    {
                        const float barW  = std::max( 2.0f, 3.0f * scale );
                        const float inset = 4.0f * scale;
                        dl.AddRectFilled( { mn.x, mn.y + inset }, { mn.x + barW, mx.y - inset },
                                          glm::vec4( b.SelectedAccent, 1.0f ), barW * 0.5f );
                    }

                    // `focused` is the HOST's, and it survives between frames — so a control that held focus
                    // while it was reachable keeps holding it after an ancestor turns Blocking. Gating the
                    // question itself (rather than the Enter below) is what makes that stale focus inert in
                    // every direction at once: no activation, no focus ring, no caret.
                    const bool isFocused = interactive && focused && *focused == e;
                    if ( outClicked && input && !b.Disabled &&
                         ( ( hover && input->MouseReleased && !ctx.View.Drag.Active ) ||
                           ( isFocused && input->Submit ) ) )
                    {
                        // Encode the structured action into the click message the runtime dispatches (same
                        // encoding as the ImGui renderer, so the host dispatcher is unchanged).
                        switch ( b.Action )
                        {
                            case ECS::UIButtonAction::LoadScene:
                                *outClicked = "scene:" + b.OnClickMessage;
                                break;
                            case ECS::UIButtonAction::QuitGame:
                                *outClicked = "quit";
                                break;
                            case ECS::UIButtonAction::OpenURL:
                                *outClicked = "url:" + b.OnClickMessage;
                                break;
                            case ECS::UIButtonAction::ShowScreen:
                                // Handled inside the canvas — the host never sees a screen switch.
                                RequestScreen( ctx, b.OnClickMessage, false );
                                *outClicked = "screen:" + b.OnClickMessage;
                                break;
                            case ECS::UIButtonAction::BackScreen:
                                RequestScreen( ctx, "", true );
                                *outClicked = "screen:back";
                                break;
                            case ECS::UIButtonAction::SendMessage:
                                *outClicked = b.OnClickMessage;
                                break;
                            case ECS::UIButtonAction::None:
                            default:
                                break;
                        }
                    }
                }
                else if ( reg.has<ECS::UIPanelComponent>( e ) )
                {
                    const auto& p = reg.get<ECS::UIPanelComponent>( e ).Data;

                    // Pulse breathes the whole panel's opacity between PulseMin and full (live dot / CTA glow).
                    const float op =
                         p.Pulse ? p.Opacity * ( p.PulseMin +
                                                 ( 1.0f - p.PulseMin ) *
                                                      ( 0.5f + 0.5f * std::sin( NowSeconds() * p.PulseSpeed ) ) )
                                 : p.Opacity;

                    if ( p.Glow && p.GlowSize > 0.0f )
                    {
                        const int   layers = 6;
                        const float gs     = p.GlowSize * scale;
                        for ( int i = 0; i < layers; ++i ) // large faint -> small; overlap into a soft glow
                        {
                            const float ex = gs * ( 1.0f - static_cast<float>( i ) / layers );
                            dl.AddRectFilled( { mn.x - ex, mn.y - ex }, { mx.x + ex, mx.y + ex },
                                              glm::vec4( p.GlowColor, 0.10f * op ), p.CornerRadius + ex );
                        }
                    }

                    if ( p.Shadow )
                        dl.AddRectFilled( { mn.x + p.ShadowOffset.x * scale, mn.y + p.ShadowOffset.y * scale },
                                          { mx.x + p.ShadowOffset.x * scale, mx.y + p.ShadowOffset.y * scale },
                                          glm::vec4( p.ShadowColor, op ), p.CornerRadius * scale );

                    // Circle forces full rounding (radius = half the shorter side) for avatars / badges / dots.
                    const float rounding = p.Circle ? std::min( rect.W, rect.H ) * 0.5f : p.CornerRadius * scale;

                    // A streamed video fills the panel (its stable texture is updated outside the pass by the
                    // VideoService); it takes precedence over the sprite/gradient fill while a path is set.
                    Graphic::Image2D* video = HandleSet( p.Video )
                                                   ? Runtime::ResourceRegistry::GetVideoService()->Resolve(
                                                          static_cast<uint64_t>( p.Video ) )
                                                   : nullptr;
                    // Frosted glass: the fill IS the blurred scene behind the panel, tinted by Color/Opacity.
                    // Checked before the sprite/video fills — a glass panel is defined by what is behind it,
                    // so an image on top of it would be a different element (draw one as a child).
                    // A UI-domain material IS the fill and is asked first, ahead of glass, video, the
                    // gradient and the sprite: those are the fixed list this replaces, and letting one of
                    // them win would make the material's presence depend on which other field happened to
                    // be set. Resolve() never answers null for a set handle — a slot the UI path cannot
                    // execute comes back as the magenta error entry, named once in the log.
                    const auto* uiMaterial = ResolveUIMaterial( ctx, e, p.Material );
                    if ( uiMaterial )
                        dl.AddMaterialRect( uiMaterial, mn, mx, Tinted( ctx, glm::vec4( p.Color, op ) ) );
                    else if ( p.BackdropBlur > 0.0f && !video && !HandleSet( p.Sprite ) )
                        dl.AddGlassRect( mn, mx, Tinted( ctx, glm::vec4( p.Color, op ) ), rounding,
                                         p.BackdropBlur );
                    else if ( video )
                        dl.AddImage( video, mn, mx, { 0.0f, 0.0f }, { 1.0f, 1.0f },
                                     Tinted( ctx, glm::vec4( p.Color, op ) ) );
                    else if ( p.UseGradient && !HandleSet( p.Sprite ) )
                        dl.AddRectFilledMultiColor( mn, mx, Tinted( ctx, glm::vec4( p.Color, op ) ),
                                                    glm::vec4( p.GradientColor, op ) );
                    else
                        DrawBox( dl, mn, mx, Tinted( ctx, glm::vec4( p.Color, op ) ), p.Sprite, p.SpriteBorder,
                                 scale, rounding );

                    // Gradient ring hugging the edge (avatar / status / progress ring).
                    if ( p.RingWidth > 0.0f )
                    {
                        const glm::vec2 c      = ( mn + mx ) * 0.5f;
                        const float     outerR = std::min( rect.W, rect.H ) * 0.5f;
                        const float     rw     = std::max( 1.0f, p.RingWidth * scale );
                        dl.AddRing( c, outerR, outerR - rw, glm::vec4( p.RingColorA, 1.0f ),
                                    glm::vec4( p.RingColorB, 1.0f ) );
                    }

                    if ( p.BorderWidth > 0.0f )
                        dl.AddRect( mn, mx, glm::vec4( p.BorderColor, 1.0f ), p.BorderWidth * scale );
                }
                else if ( reg.has<ECS::UIProgressBarComponent>( e ) )
                {
                    ECS::UIProgressBarData pb = reg.get<ECS::UIProgressBarComponent>( e ).Data;
                    if ( binding.Value )
                        pb.Value = *binding.Value; // bound: the store drives the fill
                    const float r = pb.CornerRadius * scale;
                    dl.AddRectFilled( mn, mx, Tinted( ctx, glm::vec4( pb.Background, 1.0f ) ), r );
                    const float t = std::clamp( pb.Value, 0.0f, 1.0f );
                    if ( t > 0.0f )
                        dl.AddRectFilled( mn, { mn.x + rect.W * t, mx.y }, glm::vec4( pb.Fill, 1.0f ), r );
                }
                else if ( reg.has<ECS::UIToggleComponent>( e ) )
                {
                    auto&      tg    = reg.get<ECS::UIToggleComponent>( e ).Data;
                    const bool  hover = input && hot;
                    const float r = tg.CornerRadius * scale;
                    dl.AddRectFilled( mn, mx, Tinted( ctx, glm::vec4( tg.BoxColor, 1.0f ) ), r );
                    if ( tg.Value )
                    {
                        const float pad = std::min( rect.W, rect.H ) * 0.22f; // inset "check" fill
                        dl.AddRectFilled( { mn.x + pad, mn.y + pad }, { mx.x - pad, mx.y - pad },
                                          glm::vec4( tg.CheckColor, 1.0f ), r * 0.5f );
                    }
                    const bool isFocused = interactive && focused && *focused == e;
                    if ( input && ( ( hover && input->MouseReleased ) || ( isFocused && input->Submit ) ) )
                        tg.Value = !tg.Value;
                }
                else if ( reg.has<ECS::UISliderComponent>( e ) )
                {
                    auto&       sl    = reg.get<ECS::UISliderComponent>( e ).Data;
                    const float range = std::max( 0.0001f, sl.MaxValue - sl.MinValue );
                    const float t     = std::clamp( ( sl.Value - sl.MinValue ) / range, 0.0f, 1.0f );
                    const float pill  = rect.H * 0.5f; // fully-rounded track ends
                    const float fillX = mn.x + rect.W * t;
                    const float cy    = ( mn.y + mx.y ) * 0.5f;
                    const float hs    = rect.H * 0.6f; // handle half-size (circle via rounding)
                    dl.AddRectFilled( mn, mx, Tinted( ctx, glm::vec4( sl.TrackColor, 1.0f ) ), pill );
                    if ( t > 0.0f )
                        dl.AddRectFilled( mn, { fillX, mx.y }, glm::vec4( sl.FillColor, 1.0f ), pill );
                    dl.AddRectFilled( { fillX - hs, cy - hs }, { fillX + hs, cy + hs },
                                      glm::vec4( sl.HandleColor, 1.0f ), hs );

                    const bool hover = input && hot;
                    if ( hover && input->MouseDown )
                    {
                        // The slider's fraction is measured along ITS OWN track, so it takes the undone
                        // pointer: dragging a rotated slider follows the track you can see rather than
                        // the screen's x axis.
                        const float nt =
                             std::clamp( ( pointerPx.x - mn.x ) / std::max( 1.0f, rect.W ), 0.0f, 1.0f );
                        sl.Value = sl.MinValue + nt * range;
                    }
                }
                else if ( reg.has<ECS::UIInputFieldComponent>( e ) )
                {
                    auto&      f         = reg.get<ECS::UIInputFieldComponent>( e ).Data;
                    const bool isFocused = interactive && focused && *focused == e;
                    const bool hover     = input && hot;

                    dl.AddRectFilled( mn, mx, Tinted( ctx, glm::vec4( f.Background, 1.0f ) ),
                                      f.CornerRadius * scale );
                    if ( isFocused )
                        dl.AddRect( mn, mx, glm::vec4( f.FocusColor, 1.0f ), std::max( 1.0f, 2.0f * scale ) );

                    // Text (or dimmed placeholder), clipped to the field; caret at the end when focused.
                    const bool      showPlaceholder = f.Text.empty() && !isFocused;
                    ECS::UITextData td;
                    // The PLACEHOLDER is authored and therefore localisable; `f.Text` is what the player
                    // typed and is drawn exactly as typed — translating a person's own input would be
                    // absurd, and it is the one string on a canvas that must never go through the table.
                    td.Text     = showPlaceholder ? Localization::Localization::Get().Resolve( f.Placeholder ).Text
                                                  : f.Text;
                    td.FontSize = f.FontSize;
                    td.Color    = showPlaceholder ? f.PlaceholderColor : f.TextColor;
                    td.Align    = ECS::UITextAlign::Left;
                    dl.PushClipRect( mn, mx );
                    DrawText2D( dl, td, rect, scale, ctx.View.Tint );
                    if ( isFocused )
                    {
                        const float caretX = rect.X + 6.0f + MeasureTextPx( f.Text, f.FontSize * scale );
                        dl.AddRectFilled( { caretX, rect.Y + rect.H * 0.2f },
                                          { caretX + std::max( 1.0f, scale ), rect.Y + rect.H * 0.8f },
                                          glm::vec4( f.TextColor, 1.0f ) );
                    }
                    dl.PopClipRect();

                    if ( isFocused && input )
                    {
                        if ( !input->TypedText.empty() )
                            f.Text += input->TypedText;
                        if ( input->Backspace )
                            Utf8PopBack( f.Text );
                    }
                    if ( hover && input && input->MouseReleased && focused )
                        *focused = e; // click to focus
                }
                else if ( reg.has<ECS::UIDropdownComponent>( e ) )
                {
                    auto&      d       = reg.get<ECS::UIDropdownComponent>( e ).Data;
                    const auto options = SplitOptions( d.Options );

                    dl.AddRectFilled( mn, mx, Tinted( ctx, glm::vec4( d.Background, 1.0f ) ),
                                      d.CornerRadius * scale );

                    ECS::UITextData td;
                    td.Text     = ( d.SelectedIndex >= 0 && d.SelectedIndex < (int)options.size() )
                                       ? options[d.SelectedIndex]
                                       : std::string();
                    td.FontSize = d.FontSize;
                    td.Color    = d.TextColor;
                    td.Align    = ECS::UITextAlign::Left;
                    DrawText2D( dl, td, rect, scale, ctx.View.Tint );

                    // Down-arrow on the right edge.
                    const float ax = mx.x - rect.H * 0.5f, ay = ( mn.y + mx.y ) * 0.5f, aw = rect.H * 0.16f;
                    dl.AddTriangleFilled( { ax - aw, ay - aw * 0.7f }, { ax + aw, ay - aw * 0.7f },
                                          { ax, ay + aw * 0.7f }, glm::vec4( d.TextColor, 1.0f ) );

                    const bool hover     = input && hot;
                    const bool isFocused = interactive && focused && *focused == e;
                    if ( input && ( ( hover && input->MouseReleased ) || ( isFocused && input->Submit ) ) )
                        d.Open = !d.Open;
                    if ( d.Open && popups )
                        // Deferred to draw on top of everything — which means it is drawn AFTER the walk,
                        // outside every transform, so what it is anchored to has to be a screen box and
                        // not a rect in a space that no longer exists by then. An open list under a
                        // rotated dropdown therefore hangs straight down from the box's bounds, which is
                        // what a screen-space overlay does everywhere else in this engine.
                        popups->push_back( { e, ScreenBounds( rect ), scale } );
                }

                if ( reg.has<ECS::UITextComponent2D>( e ) )
                {
                    // A bound label draws the store's string, and a keyed one draws its translation, both
                    // without the component ever being touched — the authored text is never written back.
                    ECS::UITextData bound = reg.get<ECS::UITextComponent2D>( e ).Data;
                    bound.Text            = ResolveLabel( bound.Text, binding );
                    DrawText2D( dl, bound, rect, scale, ctx.View.Tint );
                }

                if ( reg.has<ECS::UIIconComponent>( e ) )
                {
                    DrawIcon( dl, reg.get<ECS::UIIconComponent>( e ).Data, rect, ctx.View.Tint );
                }

                if ( reg.has<ECS::UIImageComponent>( e ) )
                {
                    // A sprite block — reuses DrawBox so it gets GIF playback, 9-slice and the static path.
                    // With no sprite bound it draws nothing (an empty Image is invisible, not a solid box).
                    const auto& im = reg.get<ECS::UIImageComponent>( e ).Data;
                    if ( HandleSet( im.Sprite ) )
                        DrawBox( dl, mn, mx, Tinted( ctx, glm::vec4( im.Tint, im.Opacity ) ), im.Sprite,
                                 im.SpriteBorder, scale, 0.0f );
                }

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
                        dl.AddRect( mn, mx, glm::vec4( 0.30f, 0.62f, 0.98f, 1.0f ),
                                    std::max( 1.0f, 2.0f * scale ) );
                }
            }

            if ( reg.has<ECS::RelationshipComponent>( e ) )
            {
                Rect childParent = rect;
                // Clip Contents (RectMask2D) OR a scroll view both scissor children to this element's rect.
                bool clip = reg.has<ECS::UILayoutComponent>( e ) &&
                            reg.get<ECS::UILayoutComponent>( e ).Data.ClipContents;

                float scrollMaxPx = 0.0f; // >0 (scroll view overflowing) => draw a scrollbar afterward
                if ( reg.has<ECS::UIScrollViewComponent>( e ) )
                {
                    auto& sv = reg.get<ECS::UIScrollViewComponent>( e ).Data;
                    dl.AddRectFilled( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H },
                                      glm::vec4( sv.Background, 1.0f ) );

                    const float contentPx = sv.ContentHeight * scale;
                    scrollMaxPx           = std::max( 0.0f, contentPx - rect.H );
                    const bool hover      = input && interactive && e == ctx.View.Hot;
                    if ( hover && input->ScrollDelta != 0.0f )
                        sv.ScrollY -= input->ScrollDelta * 30.0f; // 30 design px per wheel notch
                    const float maxScrollDesign = scale > 0.0f ? scrollMaxPx / scale : 0.0f;
                    sv.ScrollY                  = std::clamp( sv.ScrollY, 0.0f, maxScrollDesign );

                    clip = true;
                    childParent.Y -= sv.ScrollY * scale; // shift children up by the scroll offset
                }

                if ( clip )
                    dl.PushClipRect( { rect.X, rect.Y }, { rect.X + rect.W, rect.Y + rect.H } );

                // Children inherit the clip for hit testing too, so what is scrolled out of view can't be
                // clicked through its viewport. Narrowed by THE SAME FUNCTION DrawList2D::PushClipRect just
                // called, with the same matrix and the same rect — the pointer is refused exactly where the
                // geometry was cut, because there is one implementation of "where" and not two that agree.
                // The return value is ignored on purpose: the draw list has already logged an inexact
                // region, and both halves get the same superset either way.
                Graphic::Render2D::ClipRegion2D childClip = clipRegion;
                if ( clip )
                    (void)Graphic::Render2D::IntersectClipRegion( childClip, dl.GetTransform(), { rect.X, rect.Y },
                                                                  { rect.X + rect.W, rect.Y + rect.H } );

                const auto& children = reg.get<ECS::RelationshipComponent>( e ).Children;
                if ( reg.has<ECS::UILayoutGroupComponent>( e ) )
                {
                    // Auto-layout: the group positions + sizes its children (overriding their anchors). Each
                    // child's preferred size = CustomMinimumSize, else its authored offset size (design px).
                    const auto&               g = reg.get<ECS::UILayoutGroupComponent>( e ).Data;
                    std::vector<entt::entity> kids;
                    std::vector<glm::vec2>    sizes;
                    std::vector<float>        flex;
                    for ( auto c : children )
                    {
                        // THE LAYOUT AXIS, and the only place it does anything: a Collapsed child is not
                        // given a slot, so every sibling after it moves up by that slot's size plus the
                        // spacing. A Hidden one is kept here and stopped at the top of DrawElement, which
                        // is what leaves its hole open.
                        if ( !reg.valid( c ) || !TakesLayoutSpace( reg, c ) )
                            continue;
                        glm::vec2 pref( 0.0f );
                        float     fg = 0.0f;
                        if ( reg.has<ECS::UILayoutComponent>( c ) )
                        {
                            const auto& L = reg.get<ECS::UILayoutComponent>( c ).Data;
                            pref          = glm::max( L.CustomMinimumSize, L.OffsetMax - L.OffsetMin );
                            fg            = L.FlexGrow;
                        }
                        kids.push_back( c );
                        sizes.push_back( pref * scale );
                        flex.push_back( fg );
                    }

                    LayoutGroupParams params;
                    params.Type         = g.Type == ECS::UILayoutType::Horizontal ? LayoutGroupType::Horizontal
                                          : g.Type == ECS::UILayoutType::Grid     ? LayoutGroupType::Grid
                                                                                  : LayoutGroupType::Vertical;
                    params.PaddingL     = g.Padding.x * scale;
                    params.PaddingT     = g.Padding.y * scale;
                    params.PaddingR     = g.Padding.z * scale;
                    params.PaddingB     = g.Padding.w * scale;
                    params.Spacing      = g.Spacing * scale;
                    params.StretchCross = g.StretchCross;
                    params.CellSize     = g.CellSize * scale;
                    params.Columns      = g.Columns;

                    const auto rects = SolveLayoutGroup( childParent, params, sizes, flex );
                    for ( std::size_t i = 0; i < kids.size(); ++i )
                        DrawElement( ctx, reg, kids[i], childParent, scale, dl, input, outClicked, focused, popups,
                                     focusables, childClip, childScope, &rects[i] );
                }
                else
                {
                    for ( auto c : children )
                        if ( reg.valid( c ) )
                            DrawElement( ctx, reg, c, childParent, scale, dl, input, outClicked, focused, popups,
                                         focusables, childClip, childScope );
                }
                if ( clip )
                    dl.PopClipRect();

                // Scroll thumb on the right edge (outside the clip), shown only when the content overflows.
                if ( reg.has<ECS::UIScrollViewComponent>( e ) )
                {
                    const auto& sv = reg.get<ECS::UIScrollViewComponent>( e ).Data;
                    if ( sv.ShowScrollbar && scrollMaxPx > 0.0f )
                    {
                        const float barW      = 6.0f * scale;
                        const float trackX    = rect.X + rect.W - barW;
                        const float contentPx = sv.ContentHeight * scale;
                        const float thumbH    = std::max( barW * 2.0f, rect.H * ( rect.H / contentPx ) );
                        const float t         = ( sv.ScrollY * scale ) / scrollMaxPx;
                        const float thumbY    = rect.Y + t * ( rect.H - thumbH );
                        dl.AddRectFilled( { trackX, thumbY }, { rect.X + rect.W, thumbY + thumbH },
                                          glm::vec4( sv.ScrollbarColor, 1.0f ), barW * 0.5f );
                    }
                }
            }
        }
    } // namespace

    void BeginUIFrame( UIViewContext& view, entt::registry& reg )
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

        // THIS VIEW's frame delta, advanced once per FRAME and not once per canvas. Clamped so a long stall
        // doesn't snap animations; the first frame of a view gets 0 rather than the age of the process.
        const float now    = NowSeconds();
        view.FrameDt       = view.HasDrawn ? std::clamp( now - view.LastFrameTime, 0.0f, 0.1f ) : 0.0f;
        view.LastFrameTime = now;
        view.HasDrawn      = true;
        ++view.FrameIndex; // drives the tween rewind-on-hide check

        // A scene swap leaves the elected entity dangling — drop it rather than matching a recycled id.
        if ( view.Hot != entt::null && !reg.valid( view.Hot ) )
            view.Hot = entt::null;

        // The election is over the whole frame: every canvas of this view writes into it in draw order and
        // the topmost writer wins, which is what lets an overlay canvas take the pointer from the HUD.
        view.HotNext = entt::null;
        view.Focusables.clear();
        view.FrameOpen = true;
    }

    Common::BoolResultStr RenderCanvas2D( UIViewContext& view, entt::registry& reg, entt::entity canvasEntity,
                                          Graphic::Render2D::DrawList2D& dl, const Rect& viewportPx,
                                          const glm::mat4* worldViewProj, const UIInput* input,
                                          std::string* outClicked, entt::entity* focused )
    {
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
        WalkCtx ctx{ view, view.CanvasState( canvasEntity ) };

        const auto& canvasData = reg.get<ECS::UICanvasComponent>( canvasEntity ).Data;
        if ( !canvasData.Visible )
            return Common::MakeSuccess( false ); // a canvas that asked not to be drawn, not a failure

        Rect  canvasRect;
        float scale;
        if ( canvasData.RenderMode == ECS::UICanvasRenderMode::WorldSpace && worldViewProj &&
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
            Graphic::Image2D* bg = ResolveAnimatedFrame( canvasData.Sprite );
            if ( !bg )
                bg = ResolveSpriteImage( canvasData.Sprite );
            if ( bg )
            {
                dl.AddImage( bg, { canvasRect.X, canvasRect.Y },
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
            std::string firstScreen;
            bool        currentExists = false;
            ForEachScreenName( reg, canvasEntity,
                               [&]( const std::string& n )
                               {
                                   if ( firstScreen.empty() )
                                       firstScreen = n;
                                   if ( n == ctx.Canvas.Screen )
                                       currentExists = true;
                               } );
            if ( !firstScreen.empty() && !currentExists )
            {
                ctx.Canvas.Screen = firstScreen;
                ctx.Canvas.ScreenFrom.clear();
                ctx.Canvas.ScreenStack.clear();
                ctx.Canvas.ScreenT = 1.0f;
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
        if ( reg.has<ECS::RelationshipComponent>( canvasEntity ) )
            for ( auto c : reg.get<ECS::RelationshipComponent>( canvasEntity ).Children )
                if ( reg.valid( c ) )
                    DrawElement( ctx, reg, c, childRoot, scale, dl, input, outClicked, focused, &popups,
                                 &ctx.View.Focusables, rootClip, HitScope{} );

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
            const auto  options = SplitOptions( d.Options );
            const float rowH    = pi.Box.H;
            const Rect  popup{ pi.Box.X, pi.Box.Y + pi.Box.H, pi.Box.W,
                              rowH * static_cast<float>( options.size() ) };
            dl.AddRectFilled( { popup.X, popup.Y }, { popup.X + popup.W, popup.Y + popup.H },
                              glm::vec4( d.Background, 1.0f ), d.CornerRadius * pi.Scale );

            bool clickedOption = false;
            for ( std::size_t i = 0; i < options.size(); ++i )
            {
                const Rect row{ popup.X, popup.Y + static_cast<float>( i ) * rowH, popup.W, rowH };
                const bool hover = input && input->MousePx.x >= row.X && input->MousePx.x <= row.X + row.W &&
                                   input->MousePx.y >= row.Y && input->MousePx.y <= row.Y + row.H;
                if ( hover )
                    dl.AddRectFilled( { row.X, row.Y }, { row.X + row.W, row.Y + row.H },
                                      glm::vec4( d.Highlight, 1.0f ) );
                ECS::UITextData td;
                td.Text     = options[i];
                td.FontSize = d.FontSize;
                td.Color    = d.TextColor;
                td.Align    = ECS::UITextAlign::Left;
                DrawText2D( dl, td, row, pi.Scale, ctx.View.Tint );
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
            auto events = [&]( entt::entity e ) -> const ECS::UIPointerEventsData*
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
                            : ECS::UIHitTest::All;
            };

            // May @p e hear a pointer event about ITSELF? Only All. ChildrenOnly is transparent to the
            // pointer, so telling it about a press it cannot receive would contradict the field that says
            // it cannot; Blocking responds to nothing by definition. Either may still sit on the route of a
            // descendant that does respond — being silent is not the same as being absent.
            auto respondsToPointer = [&]( entt::entity e )
            { return e != entt::null && reg.valid( e ) && hitTestOf( e ) == ECS::UIHitTest::All; };

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
            auto step = [&]( entt::entity e, ECS::UIEventPhase phase, std::string ECS::UIPointerEventsData::*msg )
            {
                const auto* ev = events( e );
                if ( ev == nullptr || ev->Phase != phase || !respondsToPointer( e ) )
                    return false;
                emit( ev->*msg );
                return ev->StopPropagation;
            };

            // Tunnel down the chain, then bubble back up it. Two passes over one chain rather than two
            // chains, so an element cannot be reached in one pass and missed in the other.
            auto route = [&]( entt::entity target, std::string ECS::UIPointerEventsData::*msg )
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
                if ( hitTestOf( target ) == ECS::UIHitTest::Blocking )
                    return;

                const std::vector<entt::entity> chain = chainOf( target );
                for ( std::size_t i = 0; i < chain.size(); ++i )
                    if ( step( chain[i], ECS::UIEventPhase::Tunnel, msg ) )
                        return;
                for ( std::size_t i = chain.size(); i-- > 0; )
                    if ( step( chain[i], ECS::UIEventPhase::Bubble, msg ) )
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
                route( view.HotNext, &ECS::UIPointerEventsData::OnDownMessage );

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
                route( view.HotNext, &ECS::UIPointerEventsData::OnUpMessage );

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
            view.PrevDown = input->MouseDown;

            // The ghost rides on top of everything, drawn after the tree so nothing overlaps it.
            if ( view.Drag.Active )
            {
                const glm::vec2 half = view.Drag.Size * 0.5f;
                const glm::vec2 mn   = input->MousePx - half;
                const glm::vec2 mx   = input->MousePx + half;
                dl.AddRectFilled( mn, mx, glm::vec4( 0.35f, 0.55f, 0.85f, view.Drag.Ghost * 0.6f ), 6.0f );
                dl.AddRect( mn, mx, glm::vec4( 0.75f, 0.87f, 1.0f, view.Drag.Ghost ), 2.0f );
            }
        }

        // The election is handed over ONCE per frame, after every canvas of the view has written into it —
        // the topmost writer in draw order wins, across canvases. Doing this per walk gave the answer to
        // whichever canvas happened to be drawn last. HotNext is cleared by the next BeginUIFrame rather
        // than here, so it stays readable between the two.
        view.Hot = view.HotNext;

        // Tab advances keyboard focus to the next focusable control (wraps; effective next frame). The list
        // spans every canvas of the frame, so focus can leave a HUD and enter an overlay.
        if ( focused && input && input->Tab && !view.Focusables.empty() )
        {
            std::size_t idx = 0; // not-found -> focus the first
            for ( std::size_t i = 0; i < view.Focusables.size(); ++i )
                if ( view.Focusables[i] == *focused )
                {
                    idx = i + 1;
                    break;
                }
            *focused = view.Focusables[idx % view.Focusables.size()];
        }

        view.FrameOpen = false;
    }
} // namespace Desert::UI
