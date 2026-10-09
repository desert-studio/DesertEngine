#include <Engine/UI/Widgets/UIWidgets.hpp>

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
#include <Engine/UI/UITextEditState.hpp>

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
    namespace
    {
        // The default face's line box at @p fontPx — the same (Ascent - Descent) * s DrawText2D steps lines by.
        float LineHeightPx( IUICanvasResources& res, float fontPx )
        {
            const FontFace font = res.Font( res.DefaultFontHandle(), Text::kDefaultBakePixelHeight );
            if ( font.Baked == nullptr || !font.Baked->Valid() || font.Baked->PixelHeight <= 0.0f )
                return fontPx;
            return ( font.Baked->Ascent - font.Baked->Descent ) * ( fontPx / font.Baked->PixelHeight );
        }

        // Pixel width of text[from, to) as DRAWN (bullets for a password).
        float SpanPx( IUICanvasResources& res, const std::string& text, std::size_t from, std::size_t to,
                      bool password, float fontPx )
        {
            const std::string shown = UITextDisplayString( text, password );
            const std::size_t a     = UITextDisplayOffset( text, from, password );
            const std::size_t b     = UITextDisplayOffset( text, to, password );
            return b > a ? MeasureTextPx( res, shown.substr( a, b - a ), fontPx ) : 0.0f;
        }

        // The byte offset nearest to @p x (px from the line's left edge) on the line starting at @p lineStart.
        std::size_t OffsetAtX( IUICanvasResources& res, const std::string& text, std::size_t lineStart, float x,
                               bool password, float fontPx )
        {
            const std::size_t lineEnd = std::min( text.find( '\n', lineStart ), text.size() );
            float             pen     = 0.0f;
            for ( std::size_t i = lineStart; i < lineEnd; )
            {
                std::size_t next = i;
                Text::Utf8Next( text, next );
                const float w = SpanPx( res, text, i, next, password, fontPx );
                if ( x < pen + w * 0.5f )
                    return i;
                pen += w;
                i = next;
            }
            return lineEnd;
        }

        std::size_t LineStartAt( const std::string& text, std::size_t line )
        {
            std::size_t start = 0;
            for ( std::size_t l = 0; l < line; ++l )
            {
                const std::size_t nl = text.find( '\n', start );
                if ( nl == std::string::npos )
                    return start;
                start = nl + 1;
            }
            return start;
        }
    } // namespace

    void DrawInputFieldWidget( ElementFrame& frame )
    {
        auto& ctx         = frame.Ctx;
        auto& tree        = frame.Tree;
        auto& e           = frame.E;
        auto& scale       = frame.Scale;
        auto& dl          = frame.Dl;
        auto& input       = frame.Input;
        auto& focused     = frame.Focused;
        auto& st          = frame.St;
        auto& rect        = frame.ElementRect;
        auto& interactive = frame.Interactive;
        auto& mn          = frame.Mn;
        auto& mx          = frame.Mx;
        auto& hot         = frame.Hot;

        auto&                 f     = *tree.GetState<UIInputFieldData>( e );
        UITextEditState&      edit  = ctx.Canvas.TextEdit[e];
        const UITextEditRules rules = UITextEditRules::Of( f );
        auto&                 res   = ctx.View.Resources();
        const bool            hover = input && hot;

        // The field's own slots, resolved once: the text colour is read by the glyphs AND by the
        // caret, and the size by the glyphs AND by the caret's x — two lookups each would be two
        // chances for the caret to sit where the text does not.
        const glm::vec3 fieldText = st.Color( StyleSlot::InputText, f.TextColor );
        const float     fieldSize = st.FontSize( StyleSlot::InputFont, f.FontSize );
        const float     fontPx    = fieldSize * scale;
        const float     pad       = 6.0f; // DrawText2D's inset
        const float     lineH     = LineHeightPx( res, fontPx );

        // Where line @p line's top sits, and the x of byte @p offset, under the current scroll.
        auto lineTop = [&]( std::size_t line )
        { return rect.Y + pad - edit.ScrollY + static_cast<float>( line ) * lineH; };
        auto xOf = [&]( std::size_t offset )
        {
            return rect.X + pad - edit.ScrollX +
                   SpanPx( res, f.Text, UITextLineStart( f.Text, offset ), offset, f.Password, fontPx );
        };
        auto offsetAt = [&]( glm::vec2 p ) -> std::size_t
        {
            std::size_t line = 0;
            if ( f.MultiLine && lineH > 0.0f )
                line = static_cast<std::size_t>( std::max( 0.0f, std::floor( ( p.y - lineTop( 0 ) ) / lineH ) ) );
            return OffsetAtX( res, f.Text, LineStartAt( f.Text, line ), p.x - ( rect.X + pad - edit.ScrollX ),
                              f.Password, fontPx );
        };

        // --- Input first, so the frame shows its own result -------------------------------------------
        edit.Sync( f.Text );
        const bool press = input && input->MouseDown && !ctx.View.PrevDown;
        if ( interactive && focused && hover && press )
        {
            *focused = e; // focus on press, as Slate does, so the same press places the caret
            const std::size_t at = offsetAt( input->MousePx );
            if ( edit.LastClickTime >= 0.0 && ctx.View.Time - edit.LastClickTime < 0.35 )
                edit.SelectWordAt( f.Text, at );
            else
                edit.SetCaret( f.Text, at, false );
            edit.LastClickTime = ctx.View.Time;
            edit.Dragging      = true;
        }
        else if ( edit.Dragging && input && input->MouseDown )
            edit.SetCaret( f.Text, offsetAt( input->MousePx ), true );
        if ( !input || !input->MouseDown )
            edit.Dragging = false;
        if ( hover && input && input->MouseReleased && focused )
            *focused = e; // click to focus

        const bool isFocused = interactive && focused && *focused == e;
        auto       send      = [&]( const std::string& message )
        {
            if ( !message.empty() )
                ctx.View.WalkMessages.push_back( message + "|" + f.Text );
        };
        if ( isFocused && input )
        {
            const UITextEditOutcome outcome =
                 edit.Apply( f.Text, rules, input->TypedText, input->Keys, input->Clipboard );
            if ( outcome.Changed )
                send( f.OnChangedMessage );
            if ( outcome.Committed )
                send( f.OnCommittedMessage );
        }
        if ( edit.HadFocus && !isFocused )
            send( f.OnCommittedMessage ); // UE ETextCommit::OnUserMovedFocus
        edit.HadFocus = isFocused;

        // Keep the caret inside the field (UE: the text layout scrolls to the cursor).
        if ( isFocused )
        {
            const float caretPx  = xOf( edit.Caret() ) - ( rect.X + pad - edit.ScrollX );
            const float visibleW = std::max( 1.0f, rect.W - 2.0f * pad );
            if ( caretPx - edit.ScrollX > visibleW )
                edit.ScrollX = caretPx - visibleW;
            if ( caretPx < edit.ScrollX )
                edit.ScrollX = caretPx;
            if ( f.MultiLine )
            {
                const float caretTop = static_cast<float>( UITextLineOf( f.Text, edit.Caret() ) ) * lineH;
                const float visibleH = std::max( lineH, rect.H - 2.0f * pad );
                if ( caretTop + lineH - edit.ScrollY > visibleH )
                    edit.ScrollY = caretTop + lineH - visibleH;
                if ( caretTop < edit.ScrollY )
                    edit.ScrollY = caretTop;
            }
        }
        edit.ScrollX = std::max( 0.0f, edit.ScrollX );
        edit.ScrollY = f.MultiLine ? std::max( 0.0f, edit.ScrollY ) : 0.0f;

        // --- Draw ----------------------------------------------------------------------------------
        dl.AddRectFilled( mn, mx,
                          Tinted( ctx, glm::vec4( st.Color( StyleSlot::InputBackground, f.Background ), 1.0f ) ),
                          st.Metric( StyleSlot::InputCornerRadius, f.CornerRadius ) * scale );
        if ( isFocused )
            dl.AddRect( mn, mx, glm::vec4( st.Color( StyleSlot::InputFocus, f.FocusColor ), 1.0f ),
                        std::max( 1.0f, 2.0f * scale ) );

        // The vertical band a caret / selection occupies on line @p line.
        auto band = [&]( std::size_t line ) -> std::pair<float, float>
        {
            if ( !f.MultiLine )
                return { rect.Y + rect.H * 0.2f, rect.Y + rect.H * 0.8f };
            const float top = lineTop( line );
            return { top, top + lineH };
        };

        // Text (or dimmed placeholder), clipped to the field.
        const bool showPlaceholder = f.Text.empty() && !isFocused;
        UITextData td;
        // The PLACEHOLDER is authored and therefore localisable; `f.Text` is what the player
        // typed and is drawn exactly as typed — translating a person's own input would be
        // absurd, and it is the one string on a canvas that must never go through the table.
        td.Text     = showPlaceholder ? res.Text().Resolve( f.Placeholder ).Text
                                      : UITextDisplayString( f.Text, f.Password );
        td.FontSize = fieldSize;
        td.Color    = showPlaceholder ? st.Color( StyleSlot::InputPlaceholder, f.PlaceholderColor ) : fieldText;
        // An empty handle is "the built-in face", which is what this synthetic block has always
        // drawn with — so an unthemed field is byte-identical to what it was.
        td.Font          = st.Font( StyleSlot::InputFont, Assets::AssetHandle{} );
        td.Align         = UITextAlign::Left;
        td.RichText      = false; // typed brackets are text, not tags
        td.VerticalAlign = f.MultiLine ? UITextVAlign::Top : UITextVAlign::Middle;
        Rect drawRect    = rect;
        drawRect.X -= edit.ScrollX;
        drawRect.W += edit.ScrollX;
        drawRect.Y -= edit.ScrollY;
        drawRect.H += edit.ScrollY;

        dl.PushClipRect( mn, mx );
        if ( isFocused && edit.HasSelection() )
        {
            const auto [lo, hi] = edit.Selection();
            const glm::vec4 selColor( st.Color( StyleSlot::InputFocus, f.FocusColor ), 0.40f );
            for ( std::size_t from = lo; from < hi; )
            {
                const std::size_t lineEnd = std::min( f.Text.find( '\n', from ), f.Text.size() );
                const std::size_t to      = std::min( hi, lineEnd );
                const auto [y0, y1]       = band( UITextLineOf( f.Text, from ) );
                const float x0            = xOf( from );
                // A selected line break shows as a sliver, so an empty selected line is visible.
                const float x1 = to < hi ? xOf( to ) + fontPx * 0.25f : xOf( to );
                dl.AddRectFilled( { x0, y0 }, { x1, y1 }, selColor );
                from = to < hi ? to + 1 : hi;
            }
        }
        DrawText2D( res, dl, td, drawRect, scale, ctx.View.Tint, ctx.View.Time );
        if ( isFocused )
        {
            const float caretX  = xOf( edit.Caret() );
            const auto [y0, y1] = band( UITextLineOf( f.Text, edit.Caret() ) );
            dl.AddRectFilled( { caretX, y0 }, { caretX + std::max( 1.0f, scale ), y1 }, glm::vec4( fieldText, 1.0f ) );
        }
        dl.PopClipRect();
    }
} // namespace Desert::UI::Walk
