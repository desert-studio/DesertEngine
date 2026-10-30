#include <UI/Retained/WidgetBuilder.hpp>

#include <UI/Text/UICodeLayout.hpp>
#include <UI/UICanvasRenderer2D.hpp>
#include <UI/UIPaint.hpp>
#include <UI/UIRichText.hpp>
#include <UI/UIStyleResolver.hpp>
#include <UI/UITextEditState.hpp>
#include <Render2DCore/Text/Utf8.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Widgets::CodeEditor (WidgetBuilder.hpp): SMultiLineEditableText with a syntax-highlighting marshaller, as a
// paint leaf of the retained tree. Three parts, each with one owner:
//   - the TEXT MODEL is UITextEditState, the InputField's own (caret, selection, undo/redo, clipboard) — a code
//     field and a text field edit by the same rules;
//   - the LINES are UICodeLayout (line starts, the tokenizer's state per line, the visible range) — only the
//     lines in view are cut into runs and drawn;
//   - the LANGUAGE is the IUICodeTokenizer handed in — the widget turns token kinds into theme colours and
//     knows no word of any language.
// The keyboard reaches it only while its view holds the user's keyboard and it holds the view's focus
// (UIPaintArgs::Focused, IUITree::TakesTextInput): the E-3 owner, the one focus path.
namespace Desert::UI::Widgets
{
    namespace
    {
        // DrawText2D draws a block at its rect's corner plus this inset (the InputField's `pad`); every x and y
        // below is a glyph position, so the rect handed to DrawText2D is that position minus the inset.
        constexpr float            kInset      = 6.0f;
        constexpr std::string_view kIndent     = "    "; // Tab: four spaces (Lua style guides; the editor's Lua)
        constexpr float            kWheelLines = 3.0f;

        struct CodeEditorState
        {
            Attribute<std::string>                  Source;
            Delegate<void( const std::string& )>    OnChanged;
            std::shared_ptr<const IUICodeTokenizer> Tokenizer;
            float                                   FontSize = 16.0f;

            std::string              Text; // the text being edited: Source each frame when Source is bound
            UITextEditState          Edit;
            UICodeLayout             Layout;
            bool                     WasDown = false;
            std::vector<UICodeToken> Runs; // one line's runs, reused
        };

        glm::vec3 KindColour( const ElementStyle& st, UICodeTokenKind kind )
        {
            switch ( kind )
            {
                case UICodeTokenKind::Keyword:
                    return st.Color( StyleSlot::CodeKeyword, glm::vec3( 0.34f, 0.61f, 0.84f ) );
                case UICodeTokenKind::String:
                    return st.Color( StyleSlot::CodeString, glm::vec3( 0.81f, 0.57f, 0.47f ) );
                case UICodeTokenKind::Number:
                    return st.Color( StyleSlot::CodeNumber, glm::vec3( 0.71f, 0.80f, 0.66f ) );
                case UICodeTokenKind::Comment:
                    return st.Color( StyleSlot::CodeComment, glm::vec3( 0.42f, 0.60f, 0.33f ) );
                case UICodeTokenKind::Text:
                    break;
            }
            return st.Color( StyleSlot::CodeText, glm::vec3( 0.86f, 0.86f, 0.86f ) );
        }

        // Pixel width of text[from, to) on one line.
        float SpanPx( IUICanvasResources& res, const std::string& text, std::size_t from, std::size_t to,
                      float fontPx )
        {
            return to > from ? Walk::MeasureTextPx( res, text.substr( from, to - from ), fontPx ) : 0.0f;
        }

        // The byte on [lineStart, lineEnd) nearest to @p x px from the line's left edge.
        std::size_t OffsetAtX( IUICanvasResources& res, const std::string& text, std::size_t lineStart,
                               std::size_t lineEnd, float x, float fontPx )
        {
            float before = 0.0f;
            for ( std::size_t i = lineStart; i < lineEnd; )
            {
                std::size_t next = i;
                Desert::Text::Utf8Next( text, next );
                const float after = SpanPx( res, text, lineStart, next, fontPx );
                if ( x < ( before + after ) * 0.5f )
                    return i;
                before = after;
                i      = next;
            }
            return lineEnd;
        }

        void PaintCodeEditor( CodeEditorState& s, const UIPaintArgs& a )
        {
            if ( a.Resources == nullptr )
                return;
            IUICanvasResources& res = *a.Resources;
            const ElementStyle  unthemed;
            const ElementStyle& st = a.Style != nullptr ? *a.Style : unthemed;

            if ( s.Source.IsBound() )
                s.Text = s.Source.Get();
            s.Edit.Sync( s.Text );
            s.Layout.Update( s.Text, s.Tokenizer.get() );

            const float fontSize = st.FontSize( StyleSlot::CodeFont, s.FontSize );
            const float fontPx   = fontSize * a.Scale;
            const float lineH    = std::max( 1.0f, Walk::LineHeightPx( res, fontPx ) );

            // --- Geometry: the gutter fits the longest line number; the text column starts after it ----
            auto geometry = [&]
            {
                const std::string widest( std::to_string( s.Layout.LineCount() ).size(), '0' );
                return Walk::MeasureTextPx( res, widest, fontPx ) + 3.0f * kInset;
            };
            float       gutterW  = geometry();
            const float textLeft = a.Box.X + gutterW + kInset; // x of column 0 before the horizontal scroll
            const float textTop  = a.Box.Y + kInset;           // y of line 0 before the vertical scroll
            const float viewW    = std::max( 1.0f, a.Box.W - gutterW - 2.0f * kInset );
            const float viewH    = std::max( lineH, a.Box.H - 2.0f * kInset );

            auto offsetAt = [&]( glm::vec2 p )
            {
                const float       row  = ( p.y - textTop + s.Edit.ScrollY ) / lineH;
                const std::size_t line = std::min( static_cast<std::size_t>( std::max( 0.0f, std::floor( row ) ) ),
                                                   s.Layout.LineCount() - 1 );
                return OffsetAtX( res, s.Text, s.Layout.LineStart( line ), s.Layout.LineEnd( line ),
                                  p.x - textLeft + s.Edit.ScrollX, fontPx );
            };

            // --- Pointer: a press places the caret (the walk focused this node on the same press) -------
            const UIInput* in          = a.Input;
            const bool     down        = in != nullptr && in->MouseDown;
            const bool     press       = down && !s.WasDown && a.Hot;
            bool           caretActive = false; // the caret moved or the text changed: keep it in view
            s.WasDown                  = down;
            if ( a.Focused && press && a.HasPointer )
            {
                const std::size_t at     = offsetAt( a.Pointer );
                const bool        shift  = HasMod( in->Mods, UIKeyMods::Shift );
                const bool        second = s.Edit.LastClickTime >= 0.0 &&
                                    a.Time - s.Edit.LastClickTime < static_cast<double>( in->DoubleClickSeconds );
                if ( second && !shift )
                    s.Edit.SelectWordAt( s.Text, at );
                else
                    s.Edit.SetCaret( s.Text, at, shift );
                s.Edit.LastClickTime = a.Time;
                s.Edit.Dragging      = true;
                caretActive          = true;
            }
            else if ( s.Edit.Dragging && down && a.HasPointer )
            {
                s.Edit.SetCaret( s.Text, offsetAt( a.Pointer ), true );
                caretActive = true;
            }
            if ( !down )
                s.Edit.Dragging = false;

            // --- Keys: the typed text first, then every key in arrival order (UITextEditState::Apply) ----
            if ( a.Focused && in != nullptr )
            {
                UITextEditRules rules;
                rules.MultiLine          = true;
                rules.NewLine            = UITextNewLineKey::Enter; // Enter breaks the line; nothing commits
                const std::string before = s.Text;
                (void)s.Edit.Apply( s.Text, rules, in->TypedText, {}, in->Clipboard );
                for ( const UIKeyEvent& k : in->Keys )
                {
                    if ( k.Key == Common::KeyCode::Escape )
                        continue; // the overlay's (closes the innermost popup), never the text's
                    const bool plainTab = k.Key == Common::KeyCode::Tab && !HasMod( k.Mods, UIKeyMods::Shift ) &&
                                          !HasMod( k.Mods, UIKeyMods::Ctrl ) && !HasMod( k.Mods, UIKeyMods::Alt );
                    if ( plainTab )
                        (void)s.Edit.Insert( s.Text, kIndent, rules );
                    else if ( k.Key == Common::KeyCode::Tab )
                        continue; // Shift+Tab / Ctrl+Tab move focus on
                    else
                        (void)s.Edit.Apply( s.Text, rules, {}, std::span<const UIKeyEvent>( &k, 1 ),
                                            in->Clipboard );
                    if ( a.HandledKeys != nullptr )
                        a.HandledKeys->push_back( k.Key );
                }
                caretActive = caretActive || !in->Keys.empty() || !in->TypedText.empty();
                if ( s.Text != before )
                {
                    s.Layout.Update( s.Text, s.Tokenizer.get() );
                    gutterW = geometry(); // a tenth line widens the gutter from this frame on
                    if ( s.OnChanged.IsBound() )
                        s.OnChanged.Execute( s.Text );
                }
            }

            // --- Scroll: the wheel over the field, then the caret kept in view when it moved -------------
            if ( in != nullptr && a.Hot && in->ScrollDelta != 0.0f )
                s.Edit.ScrollY -= in->ScrollDelta * kWheelLines * lineH;
            const std::size_t caretLine  = s.Layout.LineOf( s.Edit.Caret() );
            const std::size_t caretStart = s.Layout.LineStart( caretLine );
            if ( caretActive )
            {
                const float caretY = static_cast<float>( caretLine ) * lineH;
                if ( caretY + lineH - s.Edit.ScrollY > viewH )
                    s.Edit.ScrollY = caretY + lineH - viewH;
                if ( caretY < s.Edit.ScrollY )
                    s.Edit.ScrollY = caretY;
                const float caretX = SpanPx( res, s.Text, caretStart, s.Edit.Caret(), fontPx );
                if ( caretX - s.Edit.ScrollX > viewW )
                    s.Edit.ScrollX = caretX - viewW;
                if ( caretX < s.Edit.ScrollX )
                    s.Edit.ScrollX = caretX;
            }
            const float contentH = static_cast<float>( s.Layout.LineCount() ) * lineH;
            s.Edit.ScrollY       = std::clamp( s.Edit.ScrollY, 0.0f, std::max( 0.0f, contentH - viewH ) );
            s.Edit.ScrollX       = std::max( 0.0f, s.Edit.ScrollX );

            // --- Draw: field, gutter, then only the lines in view ----------------------------------------
            const glm::vec2 boxMin( a.Box.X, a.Box.Y );
            const glm::vec2 boxMax( a.Box.X + a.Box.W, a.Box.Y + a.Box.H );
            const float     column = a.Box.X + gutterW + kInset; // textLeft after a gutter change
            auto            xOf    = [&]( std::size_t lineStart, std::size_t offset )
            { return column - s.Edit.ScrollX + SpanPx( res, s.Text, lineStart, offset, fontPx ); };
            auto lineTop = [&]( std::size_t line )
            { return textTop + static_cast<float>( line ) * lineH - s.Edit.ScrollY; };

            a.Dl.AddRectFilled(
                 boxMin, boxMax,
                 glm::vec4( st.Color( StyleSlot::CodeBackground, glm::vec3( 0.11f, 0.11f, 0.13f ) ), 1.0f ) );
            a.Dl.AddRectFilled(
                 boxMin, { a.Box.X + gutterW, boxMax.y },
                 glm::vec4( st.Color( StyleSlot::CodeGutter, glm::vec3( 0.15f, 0.15f, 0.18f ) ), 1.0f ) );

            const auto [first, last]       = s.Layout.VisibleLines( s.Edit.ScrollY, viewH, lineH );
            const Common::AssetHandle font = st.Font( StyleSlot::CodeFont, Common::AssetHandle{} );
            const glm::vec4           white( 1.0f );
            UITextData                td;
            td.FontSize      = fontSize;
            td.Font          = font;
            td.VerticalAlign = UITextVAlign::Top;
            td.RichText      = false; // typed brackets are code, not tags
            td.Wrap          = false;

            // Line numbers, right-aligned in the gutter.
            a.Dl.PushClipRect( boxMin, { a.Box.X + gutterW, boxMax.y } );
            td.Align = UITextAlign::Right;
            td.Color = st.Color( StyleSlot::CodeLineNumber, glm::vec3( 0.46f, 0.48f, 0.54f ) );
            for ( std::size_t line = first; line < last; ++line )
            {
                td.Text = std::to_string( line + 1 );
                Walk::DrawText2D( res, a.Dl, td,
                                  Rect{ a.Box.X, lineTop( line ) - kInset, gutterW, lineH + 2.0f * kInset },
                                  a.Scale, white, a.Time );
            }
            a.Dl.PopClipRect();

            a.Dl.PushClipRect( { a.Box.X + gutterW, a.Box.Y }, boxMax );
            // The selection under the text, line by line; a selected line break shows as a sliver.
            if ( a.Focused && s.Edit.HasSelection() )
            {
                const auto [lo, hi] = s.Edit.Selection();
                const glm::vec4 selection( st.Color( StyleSlot::CodeSelection, glm::vec3( 0.20f, 0.36f, 0.60f ) ),
                                           0.75f );
                for ( std::size_t line = first; line < last; ++line )
                {
                    const std::size_t ls   = s.Layout.LineStart( line );
                    const std::size_t le   = s.Layout.LineEnd( line );
                    const std::size_t from = std::max( lo, ls );
                    const std::size_t to   = std::min( hi, le );
                    const bool        eol  = hi > le && lo <= le; // the line's '\n' is selected
                    if ( from >= to && !eol )
                        continue;
                    const float x0 = xOf( ls, std::min( from, le ) );
                    const float x1 = xOf( ls, std::max( from, to ) ) + ( eol ? fontPx * 0.25f : 0.0f );
                    a.Dl.AddRectFilled( { x0, lineTop( line ) }, { x1, lineTop( line ) + lineH }, selection );
                }
            }
            // The runs of each visible line, each in its kind's colour.
            td.Align = UITextAlign::Left;
            for ( std::size_t line = first; line < last; ++line )
            {
                const std::size_t ls = s.Layout.LineStart( line );
                s.Runs.clear();
                s.Layout.Runs( line, s.Runs );
                for ( const UICodeToken& run : s.Runs )
                {
                    const float x = xOf( ls, run.Begin );
                    if ( x > boxMax.x )
                        break; // the rest of the line is right of the view
                    td.Text       = s.Text.substr( run.Begin, run.End - run.Begin );
                    td.Color      = KindColour( st, run.Kind );
                    const float w = Walk::MeasureTextPx( res, td.Text, fontPx );
                    Walk::DrawText2D( res, a.Dl, td,
                                      Rect{ x - kInset, lineTop( line ) - kInset, w + 2.0f * kInset + fontPx,
                                            lineH + 2.0f * kInset },
                                      a.Scale, white, a.Time );
                }
            }
            if ( a.Focused )
            {
                const float x = xOf( caretStart, s.Edit.Caret() );
                a.Dl.AddRectFilled( { x, lineTop( caretLine ) },
                                    { x + std::max( 1.0f, a.Scale ), lineTop( caretLine ) + lineH },
                                    glm::vec4( KindColour( st, UICodeTokenKind::Text ), 1.0f ) );
            }
            a.Dl.PopClipRect();

            if ( a.Focused )
                a.Dl.AddRect(
                     boxMin, boxMax,
                     glm::vec4( st.Color( StyleSlot::InputFocus, glm::vec3( 0.30f, 0.62f, 0.98f ) ), 1.0f ),
                     std::max( 1.0f, 2.0f * a.Scale ) );
        }
    } // namespace

    WidgetDecl CodeEditor( std::string name, Attribute<std::string> text,
                           Delegate<void( const std::string& )>    onChanged,
                           std::shared_ptr<const IUICodeTokenizer> tokenizer, float fontSize )
    {
        auto state       = std::make_shared<CodeEditorState>();
        state->Text      = text.Get();
        state->Source    = std::move( text );
        state->OnChanged = std::move( onChanged );
        state->Tokenizer = std::move( tokenizer );
        state->FontSize  = fontSize;

        WidgetDecl d =
             PaintBox( std::move( name ), [state]( const UIPaintArgs& a ) { PaintCodeEditor( *state, a ); } );
        d.Cursor( UICursorShape::TextBeam ).CapturesPointer().TakesTextInput();
        return d;
    }
} // namespace Desert::UI::Widgets
