#include <Engine/UI/Widgets/UIWidgets.hpp>

#include <Engine/UI/UICanvasRenderer2D.hpp>
#include <Engine/UI/UIOverlay.hpp>
#include <Engine/ECS/Components.hpp>
#include <Engine/Assets/Common.hpp>
#include <Engine/Graphic/Texture.hpp>
#include <Engine/Graphic/Image.hpp>
#include <Engine/Runtime/Services/Font/FontService.hpp>
#include <Engine/Runtime/Services/Icon/IconService.hpp>
#include <Engine/Localization/LocalizationService.hpp>
#include <Engine/Text/FontBaker.hpp>
#include <Engine/UI/UIStyleResolver.hpp>
#include <Engine/Runtime/Services/UITheme/UIThemeService.hpp>
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
        auto&      reg          = frame.Reg;
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

        auto&      d       = reg.get<ECS::UIDropdownComponent>( e ).Data;
        const auto options = SplitOptions( d.Options );

        // Resolved once: the arrow is the same ink as the label, and two lookups would be two
        // chances for them to stop being.
        const glm::vec3 listText = st.Color( StyleSlot::DropdownText, d.TextColor );

        dl.AddRectFilled(
             mn, mx, Tinted( ctx, glm::vec4( st.Color( StyleSlot::DropdownBackground, d.Background ), 1.0f ) ),
             st.Metric( StyleSlot::DropdownCornerRadius, d.CornerRadius ) * scale );

        ECS::UITextData td;
        td.Text     = ( d.SelectedIndex >= 0 && d.SelectedIndex < (int)options.size() ) ? options[d.SelectedIndex]
                                                                                        : std::string();
        td.FontSize = st.FontSize( StyleSlot::DropdownFont, d.FontSize );
        td.Color    = listText;
        td.Font     = st.Font( StyleSlot::DropdownFont, Assets::AssetHandle{} );
        td.Align    = ECS::UITextAlign::Left;
        DrawText2D( ctx.View.Resources(), dl, td, rect, scale, ctx.View.Tint, ctx.View.Time );

        // Down-arrow on the right edge.
        const float ax = mx.x - rect.H * 0.5f, ay = ( mn.y + mx.y ) * 0.5f, aw = rect.H * 0.16f;
        dl.AddTriangleFilled( { ax - aw, ay - aw * 0.7f }, { ax + aw, ay - aw * 0.7f }, { ax, ay + aw * 0.7f },
                              glm::vec4( listText, 1.0f ) );

        const bool hover     = input && hot;
        const bool isFocused = interactive && focused && *focused == e;
        if ( input &&
             ( ( hover && input->MouseReleased ) || ( isFocused && input->Pressed( Common::KeyCode::Enter ) ) ) )
            d.Open = !d.Open;
        if ( d.Open && popups )
            // Deferred to draw on top of everything — which means it is drawn AFTER the walk,
            // outside every transform, so what it is anchored to has to be a screen box and
            // not a rect in a space that no longer exists by then. An open list under a
            // rotated dropdown therefore hangs straight down from the box's bounds, which is
            // what a screen-space overlay does everywhere else in this engine.
            popups->push_back( { e, ScreenBounds( rect ), scale, st } );
    }
} // namespace Desert::UI::Walk
