#include <Engine/UI/UIWalkCtx.hpp>

#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIOverlay.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Text/BakedFont.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/Text/Utf8.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UIDataStore.hpp>
#include <Engine/UI/UIPathGeometry.hpp>
#include <Engine/UI/UIRichText.hpp>

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

namespace Desert::UI::Walk
{
    Rect ScreenBoundsOf( Graphic::Render2D::DrawList2D& dl, const Rect& r )
    {
        if ( !dl.HasTransform() )
            return r;
        glm::vec2 mn, mx;
        Graphic::Render2D::TransformedAABB2D( dl.GetTransform(), { r.X, r.Y }, { r.X + r.W, r.Y + r.H }, mn, mx );
        return Rect{ mn.x, mn.y, mx.x - mn.x, mx.y - mn.y };
    }

    bool HandleSet( const Assets::AssetHandle& h )
    {
        return static_cast<uint64_t>( h ) != 0;
    }

    // Resolve every retainer's Mask Element NAME to an element, once per walk. A name that matches no
    // UI element, or more than one, is refused with the reason — the retainer then draws unmasked and
    // says so, rather than guessing which of two "Dune"s was meant.
    void ResolveRetainerMasks( WalkCtx& ctx, IUITree& tree )
    {
        const std::vector<NodeId> elements = RootsOf( tree, ArgKind::Layout );
        for ( const NodeId r : RootsOf( tree, ArgKind::Retainer ) )
        {
            const auto& ret = *tree.Get<UIRetainerData>( r );
            if ( !ret.Mask || ret.MaskElement.empty() )
                continue;
            NodeId found = NodeId::Null;
            int          hits  = 0;
            for ( const NodeId m : elements )
                if ( m != r && tree.Name( m ) == ret.MaskElement )
                {
                    found = m;
                    ++hits;
                }
            if ( hits != 1 )
            {
                if ( ctx.Canvas.RetainerMaskRefused.insert( r ).second )
                    LOG_ERROR( "[UI] retainer mask '{}': {} UI elements carry that name (exactly one must); "
                               "the layer draws unmasked",
                               ret.MaskElement, hits );
                continue;
            }
            ctx.MaskOf.emplace( r, found );
            ctx.MaskTargets.insert( found );
        }
    }

    // THE STYLE ONE ELEMENT RESOLVES THROUGH — the only place an element is paired with a style.
    //
    // An element with no UIStyleComponent takes the theme's default style: theming must not require an
    // edit of every entity, which is the thing themes exist to avoid. `Local` opts out of the THEME and
    // NOT out of accessibility — a player who needs larger text needs it on the elements whose colours
    // an author pinned too, so the font scale travels either way.
    //
    // A style name the theme does not declare is REPORTED, once per name per canvas cell, with the
    // element's tag: a typo there draws the element's own colours, which is indistinguishable by eye
    // from a theme that simply does not cover this element. The fallback is the element's authored
    // values — what its author actually typed — rather than an invented default or nothing drawn.
    ElementStyle StyleFor( WalkCtx& ctx, const IUITree& tree, NodeId e )
    {
        const UIStyleData* authored =
             tree.Has<UIStyleData>( e ) ? tree.Get<UIStyleData>( e ) : nullptr;

        if ( authored != nullptr && authored->Source == UIStyleSource::Local )
            return ElementStyle( nullptr, nullptr, ctx.Style.FontScale(), ctx.Style.HighContrast() );

        // A copy rather than a reference: the alternative binds a reference to a temporary built from
        // the default-name constant. "Default" fits in a std::string's small buffer, so it costs no
        // allocation.
        const std::string name = authored != nullptr ? authored->Style : std::string( kUIThemeDefaultStyle );

        bool               unknown = false;
        const ElementStyle style   = ctx.Style.For( name, unknown );
        if ( unknown && ctx.Canvas.WarnedStyles.insert( name ).second )
        {
            LOG_ERROR( "[UI] Element '{}' asks for the style '{}', which the theme '{}' does not "
                       "declare — it draws its own authored colours instead.",
                       tree.Name( e ).empty() ? std::string_view( "<untagged>" ) : tree.Name( e ),
                       name, ctx.Style.Theme()->Name );
        }
        return style;
    }

    // Applies the four Text slots to a copy of the element's own data. A copy rather than a set of
    // separate locals because DrawText2D reads the block as a whole (wrap, auto-size, effects all
    // depend on the size), so resolving into the block is what keeps ONE path through the text layout
    // instead of a themed one and a local one.
    UITextData Themed( const ElementStyle& st, UITextData t )
    {
        t.Color        = st.Color( StyleSlot::TextColor, t.Color );
        t.ShadowColor  = st.Color( StyleSlot::TextShadow, t.ShadowColor );
        t.OutlineColor = st.Color( StyleSlot::TextOutline, t.OutlineColor );
        t.Font         = st.Font( StyleSlot::TextFont, t.Font );
        t.FontSize     = st.FontSize( StyleSlot::TextFont, t.FontSize );
        // The auto-size floor has no slot of its own and must not: it is a relation to FontSize, not a
        // design decision a theme makes. It is SCALED, though, or it would stop being a floor for the
        // size it is a floor for the moment the accessibility multiplier moved.
        t.MinFontSize = st.ScaleFontSize( t.MinFontSize );
        return t;
    }

    // Per-button hover interpolation (0=rest, 1=hovered), eased each frame toward the target so hover
    // colours cross-fade instead of snapping. The clock is keyed by entity INSIDE the view's context —
    // entt::entity is unique only within a registry, so a map shared between views answered to entity 7
    // of every scene at once.
    float HoverEase( WalkCtx& ctx, NodeId e, bool hovered )
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

    float Ease( UIEasing e, float t )
    {
        t = std::clamp( t, 0.0f, 1.0f );
        switch ( e )
        {
            case UIEasing::QuadIn:
                return t * t;
            case UIEasing::QuadOut:
                return 1.0f - ( 1.0f - t ) * ( 1.0f - t );
            case UIEasing::QuadInOut:
                return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * ( 1.0f - t ) * ( 1.0f - t );
            case UIEasing::CubicIn:
                return t * t * t;
            case UIEasing::CubicOut:
                return 1.0f - std::pow( 1.0f - t, 3.0f );
            case UIEasing::CubicInOut:
                return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow( -2.0f * t + 2.0f, 3.0f ) * 0.5f;
            case UIEasing::BackOut:
            {
                constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
                return 1.0f + c3 * std::pow( t - 1.0f, 3.0f ) + c1 * std::pow( t - 1.0f, 2.0f );
            }
            case UIEasing::ElasticOut:
            {
                if ( t <= 0.0f || t >= 1.0f )
                    return t;
                constexpr float c4 = 2.0f * 3.14159265f / 3.0f;
                return std::pow( 2.0f, -10.0f * t ) * std::sin( ( t * 10.0f - 0.75f ) * c4 ) + 1.0f;
            }
            case UIEasing::BounceOut:
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
            case UIEasing::Linear:
            default:
                return t;
        }
    }

    // A generic from->to animation, evaluated here on the way to the screen and NEVER written back
    // into the authored component — so a tween is safe to run in the editor, previews live in Design
    // mode, and stopping it simply restores the authored look. Its playhead therefore lives in the
    // view's context, which is what lets two views animate the same element independently.
    TweenSample SampleTween( WalkCtx& ctx, IUITree& tree, NodeId e )
    {
        TweenSample out;
        if ( !tree.Has<UITweenData>( e ) )
            return out;
        const auto& tw = *tree.Get<UITweenData>( e );

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
            case UITweenLoop::Loop:
                t = t > 0.0f ? std::fmod( t, 1.0f ) : 0.0f;
                break;
            case UITweenLoop::PingPong:
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
            case UITweenLoop::Once:
            default:
                t = std::clamp( t, 0.0f, 1.0f );
                break;
        }

        const glm::vec4 v = glm::mix( tw.From, tw.To, Ease( tw.Easing, t ) );
        switch ( tw.Property )
        {
            case UITweenProperty::Offset:
                out.Offset = glm::vec2( v );
                break;
            case UITweenProperty::Size:
                out.Size = glm::vec2( v );
                break;
            case UITweenProperty::Opacity:
                out.Tint.a = v.x;
                break;
            case UITweenProperty::Color:
                out.Tint = glm::vec4( glm::vec3( v ), 1.0f );
                break;
        }
        return out;
    }

    // A keyed CLIP (UIAnim) on top of the one-shot tween. The clips were stepped and evaluated once for the
    // whole frame (BeginUIFrame → IUIAnimationSource::Evaluate), because a clip may drive an element other than
    // its own; here the element only folds in what the frame computed for it.
    void ApplyAnimClip( WalkCtx& ctx, NodeId e, TweenSample& out )
    {
        const UIClipSample* clip = ctx.View.Animation().Sample( e );
        if ( clip == nullptr )
            return;
        out.Offset += clip->Offset;
        out.Size += clip->Size;
        out.Tint *= clip->Tint;
    }

    BindingSample SampleBinding( IUITree& tree, NodeId e, TweenSample& tw,
                                 const UICanvasContext& cell )
    {
        BindingSample out;
        if ( !tree.Has<UIBindingData>( e ) )
            return out;
        const auto& b = *tree.Get<UIBindingData>( e );
        if ( b.Key.empty() )
            return out;

        // The cell's locals before the process-wide store — the one question, asked in the one place
        // the layout walk asks it too, so the two cannot disagree about a frame.
        const UIDataStore& store = BindingStore( &cell, b.Key, cell.RowRecord );
        switch ( b.Target )
        {
            case UIBindTarget::Text:
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
            case UIBindTarget::Value:
                if ( const auto n = store.Number( b.Key ) )
                    out.Value = static_cast<float>( *n );
                break;
            case UIBindTarget::Opacity:
                if ( const auto n = store.Number( b.Key ) )
                    tw.Tint.a *= std::clamp( static_cast<float>( *n ), 0.0f, 1.0f );
                break;
            case UIBindTarget::Color:
                if ( const auto c = store.Color( b.Key ) )
                    tw.Tint *= glm::vec4( *c, 1.0f );
                break;
            case UIBindTarget::Visible:
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
    std::string ResolveLabel( IUITextSource& text, const std::string& authored, const BindingSample& binding )
    {

        const std::string_view source =
             binding.Text ? std::string_view( *binding.Text ) : std::string_view( authored );
        auto resolved = text.Resolve( source, binding.Number );

        // A number bound to a LITERAL label replaces it, formatted for the reader's locale — that is
        // what "bind this number to this label" has always meant here, and it is now locale-aware
        // instead of going through a C-locale printf. A number bound to a KEY is the key's `{n}`
        // argument instead, which the resolve above has already spent.
        if ( binding.Number && !binding.Text && resolved.Literal )
        {
            return text.FormatNumber( *binding.Number, 0 );
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
    bool Accepts( const UIDropTargetData& t, const std::string& payload )
    {
        return t.Accepts.empty() || payload.rfind( t.Accepts, 0 ) == 0;
    }

    // Split a ';'-separated option string into its items (empty items skipped).
    //
    // EACH ITEM IS RESOLVED ON ITS OWN (Ю15), not the list: a dropdown mixes translated labels with
    // proper nouns — a server name, a player's own preset — far more often than it is wholly one or
    // the other, and a per-list rule would force the author to choose. The separator is not part of
    // any item, so a key never contains one.
    std::vector<std::string> SplitOptions( IUITextSource& text, const std::string& s )
    {
        std::vector<std::string> out;
        std::string              cur;
        for ( char c : s )
        {
            if ( c == ';' )
            {
                if ( !cur.empty() )
                    out.push_back( text.Resolve( cur ).Text );
                cur.clear();
            }
            else
                cur += c;
        }
        if ( !cur.empty() )
            out.push_back( text.Resolve( cur ).Text );
        return out;
    }

    // Keyboard-focusable controls (Tab cycles between them; Enter activates the focused one).
    bool IsFocusable( IUITree& tree, NodeId e )
    {
        return tree.Has<UIButtonData>( e ) || tree.Has<UIInputFieldData>( e ) ||
               tree.Has<UIToggleData>( e ) || tree.Has<UISliderData>( e ) ||
               tree.Has<UIDropdownData>( e );
    }

    // Resolve a sprite AssetHandle to its runtime GPU Image2D (non-owning; the image service owns it and
    // Render2D keys its per-texture executor by the raw pointer). nullptr when unset / unresolvable.
    TextureRef ResolveSpriteImage( IUICanvasResources& res, const Assets::AssetHandle& handle )
    {
        if ( !HandleSet( handle ) )
            return {};
        return res.SpriteTexture( handle );
    }

    // Resolve an element's UI-material slot to the entry Render2D will draw it with, or nullptr when
    // the slot is unset.
    //
    // THE ONE CASE THAT IS NOT AN ERROR AND IS STILL REPORTED: a walk with no GPU backend behind it
    // (`ctx.View.Materials == nullptr`) — a unit test, or a host that never wired one. The element then
    // draws its ordinary fill, which is a FALLBACK, so it is named. Reported once per view because a
    // per-frame line buries the log and gets the whole message ignored; the picture is what keeps
    // saying it, every frame.
    const void* ResolveUIMaterial( WalkCtx& ctx, NodeId e, const Assets::AssetHandle& handle )
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

    // The offscreen world a render-texture element shows this frame, or nullptr when there is none.
    //
    // THE SIZE IS COMPUTED HERE AND NOWHERE ELSE. `rect` is the element in canvas pixels — anchors,
    // canvas scale and every layout group already applied — so this is the only place that knows how
    // many texels the quad will actually show. The backend asking for it would be a second layout
    // engine. Clamped at both ends because a target is a real allocation: an element mid-tween can
    // pass through zero width, and an author can put ResolutionScale 2 on a full-screen element.
    //
    // THE ONE CASE THAT IS NOT A BACKEND ERROR AND IS STILL REPORTED: a walk with no backend behind it
    // (`ctx.View.RenderTextures == nullptr`) — a unit test, or a host that never wired one. Reported
    // once per view, because a per-frame line buries the log and gets the whole message ignored; the
    // magenta is what keeps saying it, every frame.
    const void* ResolveRenderTexture( WalkCtx& ctx, NodeId e, const UIRenderTextureData& data,
                                      const Rect& rect )
    {
        if ( ctx.View.RenderTextures == nullptr )
        {
            if ( ctx.View.WarnedRenderTexture != e )
            {
                ctx.View.WarnedRenderTexture = e;
                LOG_WARN( "[UI] render-texture element {} names scene '{}' but this view has no 2D "
                          "backend, so it draws the magenta error fill instead",
                          static_cast<uint32_t>( e ), data.ScenePath );
            }
            return nullptr;
        }

        // 16 px is the floor and 4096 the ceiling: below the first a world is not a picture, and above
        // the second one element would cost more memory than the whole editor's viewport.
        constexpr float kMinPx = 16.0f;
        constexpr float kMaxPx = 4096.0f;
        const float     w      = std::clamp( rect.W * data.ResolutionScale, kMinPx, kMaxPx );
        const float     h      = std::clamp( rect.H * data.ResolutionScale, kMinPx, kMaxPx );

        const UIRenderTextureRequest request{ .ScenePath = data.ScenePath,
                                              .WidthPx   = static_cast<uint32_t>( w ),
                                              .HeightPx  = static_cast<uint32_t>( h ) };
        return ctx.View.RenderTextures->ResolveRenderTexture( e, request );
    }

    // An animated (GIF) sprite's current frame — a pure function of wall-clock time. Non-GIF handles
    // resolve to nullptr here, so ordinary textures fall through to ResolveSpriteImage.
    TextureRef ResolveAnimatedFrame( IUICanvasResources& res, const Assets::AssetHandle& handle )
    {
        return res.AnimatedFrame( handle );
    }

    // Draw a filled UI box: a sprite (tinted by `color`) when one is bound + resolvable, else a flat
    // colour. `srcBorder` (L,T,R,B in SOURCE pixels) enables 9-slice — corners stay unstretched (x
    // scale), edges/centre stretch — so image panels/buttons resize without distorting their borders.
    // Mirrors the ImGui DrawBox so both render paths look identical.
    void DrawBox( IUICanvasResources& res, Graphic::Render2D::DrawList2D& dl, const glm::vec2& mn,
                  const glm::vec2& mx, const glm::vec4& color, const Assets::AssetHandle& sprite,
                  const glm::vec4& srcBorder, float scale, float rounding )
    {
        // An animated sprite plays stretched to the box; static sprites / 9-slice keep the path below.
        if ( const TextureRef frame = ResolveAnimatedFrame( res, sprite ) )
        {
            dl.AddImage( frame.Id, mn, mx, { 0.0f, 0.0f }, { 1.0f, 1.0f }, color );
            return;
        }

        const TextureRef img = ResolveSpriteImage( res, sprite );
        if ( !img )
        {
            dl.AddRectFilled( mn, mx, color, rounding );
            return;
        }

        const void* tex  = img.Id;
        const float tw   = static_cast<float>( img.Width );
        const float th   = static_cast<float>( img.Height );
        const bool  nine = tw > 0.0f && th > 0.0f &&
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

    // Remove the last UTF-8 codepoint (trailing continuation bytes + the lead/ASCII byte).
    void Utf8PopBack( std::string& s )
    {
        while ( !s.empty() && ( static_cast<unsigned char>( s.back() ) & 0xC0 ) == 0x80 )
            s.pop_back();
        if ( !s.empty() )
            s.pop_back();
    }

    CanvasFit ResolveCanvas( const UICanvasData& d, const Rect& viewportPx )
    {
        switch ( d.ScaleMode )
        {
            case UICanvasScaleMode::ScaleWithScreen:
            {
                const float sx = d.ReferenceWidth > 0.0f ? viewportPx.W / d.ReferenceWidth : 1.0f;
                const float sy = d.ReferenceHeight > 0.0f ? viewportPx.H / d.ReferenceHeight : 1.0f;
                const float m  = std::clamp( d.MatchWidthHeight, 0.0f, 1.0f );
                return { viewportPx, sx * ( 1.0f - m ) + sy * m };
            }
            case UICanvasScaleMode::Letterbox:
            {
                const Rect  fit   = CanvasRect( d.ReferenceWidth, d.ReferenceHeight, viewportPx.W, viewportPx.H );
                const float scale = d.ReferenceWidth > 0.0f ? fit.W / d.ReferenceWidth : 1.0f;
                return { Rect{ viewportPx.X + fit.X, viewportPx.Y + fit.Y, fit.W, fit.H }, scale };
            }
            case UICanvasScaleMode::Stretch:
            default:
                return { viewportPx, 1.0f };
        }
    }

    // Draw an icon ASSET centred in `rect`, sized to `sizeFrac` of the shorter side. The .svg was
    // imported into an SDF once (Runtime::IconService), so this is a single quad through the very same
    // shader as text — crisp at any size, and outline/glow/shadow come along for free.
    // @p tint as in DrawText2D: the caller's accumulated element tint, an argument rather than a global.
    void DrawIcon( IUICanvasResources& res, Graphic::Render2D::DrawList2D& dl, const UIIconData& ic,
                   const Rect& rect, const glm::vec4& tint )
    {
        const IconRef icon  = res.Icon( static_cast<uint64_t>( ic.Icon ) );
        const void*   atlas = icon.Atlas;
        // unset/unreadable: draw nothing, no placeholder
        if ( icon.Layers.empty() || atlas == nullptr )
            return;

        const float box = std::min( rect.W, rect.H ) * std::clamp( ic.Scale, 0.1f, 1.0f );
        if ( box <= 0.0f )
            return;
        // Fit the source aspect inside that box so a wide icon isn't stretched.
        const float     w = icon.Aspect >= 1.0f ? box : box * icon.Aspect;
        const float     h = icon.Aspect >= 1.0f ? box / icon.Aspect : box;
        const glm::vec2 c( rect.X + rect.W * 0.5f, rect.Y + rect.H * 0.5f );

        // One quad per colour run, painted back-to-front in document order. A monochrome icon is a
        // single white layer, so Color tints it outright; a multi-colour one keeps the fills the .svg
        // authored and Color multiplies them (white = exactly as drawn).
        for ( const Text::IconLayer& layer : icon.Layers )
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

    // ONE STATEMENT OF A LAYOUT GROUP'S GEOMETRY, read by the two places that must agree about it: the
    // Content Size Fitter's MEASURE and the layout that then positions the children. They were two
    // copies of five lines, which was survivable while both read the same component — and stops being
    // the moment a theme can move the padding, because a themed padding applied in one of them and not
    // the other is a container that hugs its children at the wrong size. Same shape as every "two
    // values that must agree" defect in this engine, so there is one value.
    LayoutGroupParams GroupParams( const UILayoutGroupData& g, const ElementStyle& st, float scale )
    {
        LayoutGroupParams params;
        params.Type = g.Type == UILayoutType::Horizontal ? LayoutGroupType::Horizontal
                      : g.Type == UILayoutType::Grid     ? LayoutGroupType::Grid
                                                         : LayoutGroupType::Vertical;

        // A THEMED PADDING IS ONE NUMBER ON ALL FOUR EDGES. A theme says "panels breathe by 12 px",
        // which is a symmetric statement; the asymmetric cases (a title bar with a deeper top inset)
        // are layout rather than livery and stay on the element. Asked through IsThemed rather than
        // through a sentinel, because 0 is a legal padding and a sentinel would make it unreachable.
        const glm::vec4 padding = st.IsThemed( StyleSlot::LayoutGroupPadding )
                                       ? glm::vec4( st.Metric( StyleSlot::LayoutGroupPadding, 0.0f ) )
                                       : g.Padding;
        params.PaddingL         = padding.x * scale;
        params.PaddingT         = padding.y * scale;
        params.PaddingR         = padding.z * scale;
        params.PaddingB         = padding.w * scale;
        params.Spacing          = st.Metric( StyleSlot::LayoutGroupSpacing, g.Spacing ) * scale;
        params.CellSize         = g.CellSize * scale;
        params.Columns          = g.Columns;
        return params;
    }

    // Content size (px) a layout-group container needs to hug its children — for the Content Size Fitter.
    glm::vec2 GroupContentPx( IUITree& tree, NodeId e, const ElementStyle& st, float scale )
    {
        if ( !tree.Has<UILayoutGroupData>( e ) )
            return { 0.0f, 0.0f };
        const auto&            g = *tree.Get<UILayoutGroupData>( e );
        std::vector<glm::vec2> sizes;
        for ( const NodeId c : ChildrenOf( tree, e ) )
        {
            if ( !tree.Valid( c ) || !TakesLayoutSpace( tree, c ) )
                continue; // a Collapsed child has no slot, so it is not part of the content either
            glm::vec2 pref( 0.0f );
            if ( tree.Has<UILayoutData>( c ) )
            {
                const auto& L = *tree.Get<UILayoutData>( c );
                pref          = glm::max( L.CustomMinimumSize, L.OffsetMax - L.OffsetMin );
            }
            sizes.push_back( pref * scale );
        }
        return MeasureLayoutGroup( GroupParams( g, st, scale ), sizes );
    }

} // namespace Desert::UI::Walk
