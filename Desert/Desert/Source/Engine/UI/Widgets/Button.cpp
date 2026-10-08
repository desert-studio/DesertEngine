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
    void DrawButtonWidget( ElementFrame& frame )
    {
        auto& ctx         = frame.Ctx;
        auto& reg         = frame.Reg;
        auto& e           = frame.E;
        auto& scale       = frame.Scale;
        auto& dl          = frame.Dl;
        auto& input       = frame.Input;
        auto& outClicked  = frame.OutClicked;
        auto& focused     = frame.Focused;
        auto& st          = frame.St;
        auto& interactive = frame.Interactive;
        auto& mn          = frame.Mn;
        auto& mx          = frame.Mx;
        auto& hot         = frame.Hot;

        const auto& b = reg.get<ECS::UIButtonComponent>( e ).Data;
        // Disabled swallows all pointer/keyboard interaction and rests on the dim colour.
        const bool hover = !b.Disabled && input && hot;
        const bool down  = hover && input->MouseDown;
        // Resting colour is Selected (persistent highlight) or Normal; hover cross-fades toward
        // HoverColor (eased), press snaps to PressedColor, Disabled overrides everything.
        const glm::vec3 rest = b.Selected ? st.Color( StyleSlot::ButtonSelected, b.SelectedColor )
                                          : st.Color( StyleSlot::ButtonNormal, b.NormalColor );
        const float     ht   = HoverEase( ctx, e, hover && !down );
        const glm::vec3 c    = b.Disabled ? st.Color( StyleSlot::ButtonDisabled, b.DisabledColor )
                               : down     ? st.Color( StyleSlot::ButtonPressed, b.PressedColor )
                                          : glm::mix( rest, st.Color( StyleSlot::ButtonHover, b.HoverColor ), ht );

        // Image can change with state (hover / press), falling back to the normal Sprite.
        Assets::AssetHandle spr = b.Sprite;
        if ( down && HandleSet( b.PressedSprite ) )
            spr = b.PressedSprite;
        else if ( hover && HandleSet( b.HoverSprite ) )
            spr = b.HoverSprite;
        DrawBox( ctx.View.Resources(), dl, mn, mx, Tinted( ctx, glm::vec4( c, b.Disabled ? 0.6f : 1.0f ) ), spr,
                 b.SpriteBorder, scale, 6.0f * scale );

        // Selected accent: a rounded bar hugging the left edge (the "you are here" marker).
        if ( b.Selected && !b.Disabled )
        {
            const float barW  = std::max( 2.0f, 3.0f * scale );
            const float inset = 4.0f * scale;
            dl.AddRectFilled( { mn.x, mn.y + inset }, { mn.x + barW, mx.y - inset },
                              glm::vec4( st.Color( StyleSlot::ButtonSelectedAccent, b.SelectedAccent ), 1.0f ),
                              barW * 0.5f );
        }

        // `focused` is the HOST's, and it survives between frames — so a control that held focus
        // while it was reachable keeps holding it after an ancestor turns Blocking. Gating the
        // question itself (rather than the Enter below) is what makes that stale focus inert in
        // every direction at once: no activation, no focus ring, no caret.
        const bool isFocused = interactive && focused && *focused == e;
        if ( outClicked && input && !b.Disabled &&
             ( ( hover && input->MouseReleased && !ctx.View.Drag.Active ) ||
               ( isFocused && input->Pressed( Common::KeyCode::Enter ) ) ) )
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
                case ECS::UIButtonAction::SendEvent:
                    *outClicked = b.OnClickMessage;
                    break;
                case ECS::UIButtonAction::None:
                default:
                    break;
            }
        }
    }
} // namespace Desert::UI::Walk
