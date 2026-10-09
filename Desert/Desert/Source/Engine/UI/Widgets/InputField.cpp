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
    void DrawInputFieldWidget( ElementFrame& frame )
    {
        auto& ctx         = frame.Ctx;
        auto& tree         = frame.Tree;
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

        auto&      f         = *tree.GetState<UIInputFieldData>( e );
        const bool isFocused = interactive && focused && *focused == e;
        const bool hover     = input && hot;

        // The field's own slots, resolved once: the text colour is read by the glyphs AND by the
        // caret, and the size by the glyphs AND by the caret's x — two lookups each would be two
        // chances for the caret to sit where the text does not.
        const glm::vec3 fieldText = st.Color( StyleSlot::InputText, f.TextColor );
        const float     fieldSize = st.FontSize( StyleSlot::InputFont, f.FontSize );

        dl.AddRectFilled( mn, mx,
                          Tinted( ctx, glm::vec4( st.Color( StyleSlot::InputBackground, f.Background ), 1.0f ) ),
                          st.Metric( StyleSlot::InputCornerRadius, f.CornerRadius ) * scale );
        if ( isFocused )
            dl.AddRect( mn, mx, glm::vec4( st.Color( StyleSlot::InputFocus, f.FocusColor ), 1.0f ),
                        std::max( 1.0f, 2.0f * scale ) );

        // Text (or dimmed placeholder), clipped to the field; caret at the end when focused.
        const bool      showPlaceholder = f.Text.empty() && !isFocused;
        UITextData      td;
        td.Text     = showPlaceholder ? f.Placeholder : f.Text;
        td.FontSize = fieldSize;
        td.Color    = showPlaceholder ? st.Color( StyleSlot::InputPlaceholder, f.PlaceholderColor ) : fieldText;
        // An empty handle is "the built-in face", which is what this synthetic block has always
        // drawn with — so an unthemed field is byte-identical to what it was.
        td.Font  = st.Font( StyleSlot::InputFont, Assets::AssetHandle{} );
        td.Align = UITextAlign::Left;
        // The PLACEHOLDER is authored and therefore localisable; `f.Text` is what the player
        // typed and is drawn exactly as typed — translating a person's own input would be
        // absurd, and it is the one string on a canvas that must never go through the table.
        td.Text     = showPlaceholder ? ctx.View.Resources().Text().Resolve( f.Placeholder ).Text : f.Text;
        td.FontSize = f.FontSize;
        td.Color    = showPlaceholder ? f.PlaceholderColor : f.TextColor;
        td.Align    = UITextAlign::Left;
        dl.PushClipRect( mn, mx );
        DrawText2D( ctx.View.Resources(), dl, td, rect, scale, ctx.View.Tint, ctx.View.Time );
        if ( isFocused )
        {
            const float caretX = rect.X + 6.0f + MeasureTextPx( ctx.View.Resources(), f.Text, fieldSize * scale );
            dl.AddRectFilled( { caretX, rect.Y + rect.H * 0.2f },
                              { caretX + std::max( 1.0f, scale ), rect.Y + rect.H * 0.8f },
                              glm::vec4( fieldText, 1.0f ) );
        }
        dl.PopClipRect();

        if ( isFocused && input )
        {
            if ( !input->TypedText.empty() )
                f.Text += input->TypedText;
            if ( input->Pressed( Common::KeyCode::Backspace ) )
                Utf8PopBack( f.Text );
        }
        if ( hover && input && input->MouseReleased && focused )
            *focused = e; // click to focus
    }
} // namespace Desert::UI::Walk
