#pragma once

// THE WALK'S SHARED VOCABULARY — what UICanvasRenderer2D.cpp's DrawElement and the widgets under UI/Widgets/
// both speak. DrawElement resolves one element (style, rect, tween, binding, hit-test permission) and hands
// the result to that element's widget as an ElementFrame; a widget draws from it and from nothing else.
// Split out of UICanvasRenderer2D.cpp (UI-R2D-SPLIT) so the widgets can live in their own files.

#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UICanvasResources.hpp>
#include <Engine/UI/UICanvasLayout.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/UI/UITree.hpp>
#include <Engine/Assets/Common.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Desert::UI::Walk
{
    // ONE CANVAS BEING WALKED BY ONE VIEW — the two coordinates of the key, bound together for the
    // length of the walk. They travel as one argument rather than two so that no recursion step can
    // pick up the wrong half: every ctx.Canvas below is guaranteed to be ctx.View's own cell for the
    // canvas being drawn, because RenderCanvas2D is the only place that builds one and it builds it
    // from view.CanvasState( canvas ).
    struct WalkCtx
    {
        UIViewContext&   View;
        UICanvasContext& Canvas;
        // The canvas's theme and its two accessibility knobs, resolved once per walk. A default one
        // (no theme, scale 1, contrast off) is what every canvas authored before Ю13 gets, and it
        // makes every query below return the element's own authored value.
        CanvasStyle Style;

        // RETAINED LAYERS (UE Retainer Box). Root = the frame's own list (mask layers live there, shared
        // by every canvas of the frame); Retaining = the retainer whose layer is being recorded (so the
        // recursion into it draws instead of retaining again); MaskCapture = the mask element being
        // captured (drawn even when hidden). MaskOf / MaskTargets are resolved once per walk.
        Graphic::Render2D::DrawList2D*     Root        = nullptr;
        NodeId                             Retaining   = NodeId::Null;
        NodeId                             MaskCapture = NodeId::Null;
        std::unordered_map<NodeId, NodeId> MaskOf{};
        std::unordered_set<NodeId>         MaskTargets{};
    };

    // What a tween contributes this frame. Identity when the element has none.
    struct TweenSample
    {
        glm::vec2 Offset{ 0.0f }; // design px
        glm::vec2 Size{ 0.0f };   // design px
        glm::vec4 Tint{ 1.0f };   // multiplies colour + alpha
    };

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
        NodeId Entity = NodeId::Null;
        Rect   Box;          // the dropdown's box rect (screen px)
        float  Scale = 1.0f; // canvas scale for its text
        // The style the BOX was drawn with, carried here rather than re-resolved after the walk. The
        // open list is the same control as the closed box and must be the same colours; resolving it a
        // second time would be a second answer that happens to agree today.
        ElementStyle Style;
    };

    // Maps the canvas to the viewport per its scale mode — mirrors ResolveCanvas in the ImGui renderer so
    // both paths agree on layout. Returns the canvas root rect (screen px) + the uniform scale applied to
    // every element's offsets / min-size.
    struct CanvasFit
    {
        Rect  Root;
        float Scale = 1.0f;
    };

    // ONE ELEMENT, RESOLVED. DrawElement computes every value below exactly once — the style, the final
    // rect, the tween and binding samples, the pointer brought back through the render transform, and the
    // one interaction predicate both input paths read — and the element's widget draws from this. Mn / Mx /
    // Hot exist only for an element with a rect (a layout or a layout-group slot); DrawChildren does not read
    // them.
    struct ElementFrame
    {
        WalkCtx&                               Ctx;
        IUITree&                               Tree;
        NodeId                                 E;
        float                                  Scale;
        Graphic::Render2D::DrawList2D&         Dl;
        const UIInput*                         Input;
        std::string*                           OutClicked;
        NodeId*                                Focused;
        std::vector<PopupInfo>*                Popups;
        std::vector<FocusEntry>*               Focusables;
        const Graphic::Render2D::ClipRegion2D& ClipRegion;
        const HitScope                         ChildScope;
        const ElementStyle&                    St;
        Rect&                                  ElementRect;
        TweenSample&                           Tween;
        const BindingSample&                   Binding;
        const glm::vec2                        PointerPx;
        const bool                             Interactive;
        glm::vec2                              Mn{ 0.0f };
        glm::vec2                              Mx{ 0.0f };
        bool                                   Hot = false;
    };

    // The element's on-screen bounding box under @p dl's accumulated render transform (the scissor is axis
    // aligned, so the drawn clip and the pointer's clip are both this box).
    Rect ScreenBoundsOf( Graphic::Render2D::DrawList2D& dl, const Rect& r );

    // Helpers of the walk (UIWalkCtx.cpp; each one's contract is stated at its definition).
    bool          HandleSet( const Assets::AssetHandle& h );
    void          ResolveRetainerMasks( WalkCtx& ctx, IUITree& tree );
    ElementStyle  StyleFor( WalkCtx& ctx, const IUITree& tree, NodeId e );
    UITextData    Themed( const ElementStyle& st, UITextData t );
    float         HoverEase( WalkCtx& ctx, NodeId e, bool hovered );
    void          RequestScreen( WalkCtx& ctx, const std::string& name, bool back );
    float         Ease( UIEasing e, float t );
    TweenSample   SampleTween( WalkCtx& ctx, IUITree& tree, NodeId e );
    void          ApplyAnimClip( WalkCtx& ctx, NodeId e, TweenSample& out );
    BindingSample SampleBinding( IUITree& tree, NodeId e, TweenSample& tw, const UICanvasContext& cell );
    std::string   ResolveLabel( IUITextSource& text, const std::string& authored, const BindingSample& binding );
    glm::vec4     Tinted( const WalkCtx& ctx, const glm::vec4& c );
    bool          PointIn( const Rect& r, const glm::vec2& p );
    bool          Accepts( const UIDropTargetData& t, const std::string& payload );
    std::vector<std::string> SplitOptions( IUITextSource& text, const std::string& s );
    bool                     IsFocusable( IUITree& tree, NodeId e );
    TextureRef               ResolveSpriteImage( IUICanvasResources& res, const Assets::AssetHandle& handle );
    const void*              ResolveUIMaterial( WalkCtx& ctx, NodeId e, const Assets::AssetHandle& handle );
    const void* ResolveRenderTexture( WalkCtx& ctx, NodeId e, const UIRenderTextureData& data, const Rect& rect );
    TextureRef  ResolveAnimatedFrame( IUICanvasResources& res, const Assets::AssetHandle& handle );
    void        DrawBox( IUICanvasResources& res, Graphic::Render2D::DrawList2D& dl, const glm::vec2& mn,
                         const glm::vec2& mx, const glm::vec4& color, const Assets::AssetHandle& sprite,
                         const glm::vec4& srcBorder, float scale, float rounding );
    void        Utf8PopBack( std::string& s );
    CanvasFit   ResolveCanvas( const UICanvasData& d, const Rect& viewportPx );
    void        DrawIcon( IUICanvasResources& res, Graphic::Render2D::DrawList2D& dl, const UIIconData& ic,
                          const Rect& rect, const glm::vec4& tint );
    LayoutGroupParams GroupParams( const UILayoutGroupData& g, const ElementStyle& st, float scale );
    glm::vec2         GroupContentPx( IUITree& tree, NodeId e, const ElementStyle& st, float scale );

    // Draw @p e and its sub-tree (UICanvasRenderer2D.cpp): resolves the element, dispatches it to its widget,
    // recurses into the children.
    void DrawElement( WalkCtx& ctx, IUITree& tree, NodeId e, const Rect& parent, float scale,
                      Graphic::Render2D::DrawList2D& dl, const UIInput* input, std::string* outClicked,
                      NodeId* focused, std::vector<PopupInfo>* popups, std::vector<FocusEntry>* focusables,
                      const Graphic::Render2D::ClipRegion2D& clipRegion, HitScope scope,
                      const Rect* forcedRect = nullptr );
} // namespace Desert::UI::Walk
