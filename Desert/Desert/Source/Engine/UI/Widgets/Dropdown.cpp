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
    void DrawDropdownWidget( ElementFrame& frame )
    {
        auto&      ctx          = frame.Ctx;
        auto&      tree         = frame.Tree;
        auto&      e            = frame.E;
        auto&      scale        = frame.Scale;
        auto&      dl           = frame.Dl;
        auto&      input        = frame.Input;
        auto&      focused      = frame.Focused;
        auto&      popups       = frame.Popups;
        auto&      st           = frame.St;
        auto&      rect         = frame.ElementRect;
        auto&      interactive  = frame.Interactive;
        auto&      mn           = frame.Mn;
        auto&      mx           = frame.Mx;
        auto&      hot          = frame.Hot;
        const auto ScreenBounds = [&dl]( const Rect& r ) { return ScreenBoundsOf( dl, r ); };

        auto&      d       = *tree.GetState<UIDropdownData>( e );
        const auto options = SplitOptions( ctx.View.Resources().Text(), d.Options );

        // Resolved once: the arrow is the same ink as the label, and two lookups would be two
        // chances for them to stop being.
        const glm::vec3 listText = st.Color( StyleSlot::DropdownText, d.TextColor );

        dl.AddRectFilled(
             mn, mx, Tinted( ctx, glm::vec4( st.Color( StyleSlot::DropdownBackground, d.Background ), 1.0f ) ),
             st.Metric( StyleSlot::DropdownCornerRadius, d.CornerRadius ) * scale );

        UITextData td;
        td.Text     = ( d.SelectedIndex >= 0 && d.SelectedIndex < (int)options.size() ) ? options[d.SelectedIndex]
                                                                                        : std::string();
        td.FontSize = st.FontSize( StyleSlot::DropdownFont, d.FontSize );
        td.Color    = listText;
        td.Font     = st.Font( StyleSlot::DropdownFont, Assets::AssetHandle{} );
        td.Align    = UITextAlign::Left;
        DrawText2D( ctx.View.Resources(), dl, td, rect, scale, ctx.View.Tint, ctx.View.Time );

        // Down-arrow on the right edge.
        const float ax = mx.x - rect.H * 0.5f, ay = ( mn.y + mx.y ) * 0.5f, aw = rect.H * 0.16f;
        dl.AddTriangleFilled( { ax - aw, ay - aw * 0.7f }, { ax + aw, ay - aw * 0.7f }, { ax, ay + aw * 0.7f },
                              glm::vec4( listText, 1.0f ) );

        const bool hover     = input && hot;
        const bool isFocused = interactive && focused && *focused == e;
        // Keyboard on an open list (SComboBox): Up / Down move the highlight, Enter commits it, Escape closes
        // the list and is NOT also the overlay stack's Escape. All four are the dropdown's (consumed).
        const int optionCount = static_cast<int>( options.size() );
        bool      keyHandled  = false;
        if ( input && isFocused && d.Open )
        {
            int& hl = ctx.View.DropdownHighlight.try_emplace( e, d.SelectedIndex ).first->second;
            for ( const UIKeyEvent& k : input->Keys )
            {
                if ( k.Key == Common::KeyCode::Up )
                    hl = std::max( 0, hl - 1 );
                else if ( k.Key == Common::KeyCode::Down )
                    hl = std::min( optionCount - 1, hl + 1 );
                else if ( k.Key == Common::KeyCode::Enter )
                {
                    if ( hl >= 0 && hl < optionCount )
                        d.SelectedIndex = hl;
                    d.Open = false;
                }
                else if ( k.Key == Common::KeyCode::Escape )
                    d.Open = false;
                else
                    continue;
                ctx.View.ConsumeKey( k.Key );
                keyHandled = true;
            }
        }
        if ( !keyHandled && input &&
             ( ( hover && input->MouseReleased ) || ( isFocused && input->Pressed( Common::KeyCode::Enter ) ) ) )
            d.Open = !d.Open;
        if ( !d.Open )
            ctx.View.DropdownHighlight.erase( e );
        if ( d.Open && popups )
            // Deferred to draw on top of everything — which means it is drawn AFTER the walk,
            // outside every transform, so what it is anchored to has to be a screen box and
            // not a rect in a space that no longer exists by then. An open list under a
            // rotated dropdown therefore hangs straight down from the box's bounds, which is
            // what a screen-space overlay does everywhere else in this engine.
            popups->push_back( { e, ScreenBounds( rect ), scale, st } );
    }
} // namespace Desert::UI::Walk
