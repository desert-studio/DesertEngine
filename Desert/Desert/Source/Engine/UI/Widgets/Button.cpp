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
    void DrawButtonWidget( ElementFrame& frame )
    {
        auto&       ctx         = frame.Ctx;
        auto&       tree        = frame.Tree;
        auto&       e           = frame.E;
        auto&       scale       = frame.Scale;
        auto&       dl          = frame.Dl;
        auto&       input       = frame.Input;
        auto&       outClicked  = frame.OutClicked;
        auto&       focused     = frame.Focused;
        const auto& st          = frame.St;
        const auto& interactive = frame.Interactive;
        auto&       mn          = frame.Mn;
        auto&       mx          = frame.Mx;
        auto&       hot         = frame.Hot;

        const auto& b = *tree.Get<UIButtonData>( e );
        // Disabled swallows all pointer/keyboard interaction and rests on the dim colour.
        const bool hover = !b.Disabled && input != nullptr && hot;
        const bool down  = hover && input->MouseDown;
        // Resting colour is Selected (persistent highlight) or Normal; hover cross-fades toward
        // HoverColor (eased), press snaps to PressedColor, Disabled overrides everything.
        const glm::vec3 rest = b.Selected ? st.Color( StyleSlot::ButtonSelected, b.SelectedColor )
                                          : st.Color( StyleSlot::ButtonNormal, b.NormalColor );
        const float     ht   = HoverEase( ctx, e, hover && !down );
        glm::vec3       c    = glm::mix( rest, st.Color( StyleSlot::ButtonHover, b.HoverColor ), ht );
        if ( down )
            c = st.Color( StyleSlot::ButtonPressed, b.PressedColor );
        if ( b.Disabled )
            c = st.Color( StyleSlot::ButtonDisabled, b.DisabledColor );

        // Image can change with state (hover / press), falling back to the normal Sprite.
        Assets::AssetHandle spr = b.Sprite;
        if ( down && HandleSet( b.PressedSprite ) )
            spr = b.PressedSprite;
        else if ( hover && HandleSet( b.HoverSprite ) )
            spr = b.HoverSprite;
        // A button IS a panel that reacts (UIButtonArgs.hpp), and the walk draws the button INSTEAD of the panel
        // on the same element — so the panel's Opacity is the box's alpha here, or it would be a field the
        // Details panel offers and nothing reads. Opacity 0 is a text-only row (a menu list): no box at all.
        const auto* panel = tree.Get<UIPanelData>( e );
        const float alpha = ( panel != nullptr ? panel->Opacity : 1.0f ) * ( b.Disabled ? 0.6f : 1.0f );
        if ( alpha > 0.0f )
            DrawBox( ctx.View.Resources(), dl, mn, mx, Tinted( ctx, glm::vec4( c, alpha ) ), spr, b.SpriteBorder,
                     scale, 6.0f * scale );

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
        //
        // THE POINTER ENTERING A BUTTON TAKES KEYBOARD FOCUS (CommonUI's hover-selects): a menu has ONE row the
        // player is on, not one for the mouse and another for the arrows. Only on ENTRY — a pointer resting
        // on a row must not pull focus back every frame after Down moved it on.
        if ( interactive && focused != nullptr && hover && ctx.Canvas.PointerFocusOwner != e )
        {
            *focused                     = e;
            ctx.Canvas.PointerFocusOwner = e;
        }
        else if ( !hover && ctx.Canvas.PointerFocusOwner == e )
            ctx.Canvas.PointerFocusOwner = NodeId::Null;
        const bool isFocused = interactive && focused != nullptr && *focused == e;
        // HIGHLIGHTED = the row the player is on. With a focus to follow that is the focused row (hover has
        // just handed it focus above); a host without keyboard focus highlights by the pointer alone. Recorded
        // before the children draw, so their WhileHighlighted tweens (UITweenArgs.hpp) answer this frame.
        if ( !b.Disabled && ( focused != nullptr ? isFocused : hover ) )
            ctx.Canvas.HighlightedOn[e] = ctx.View.FrameIndex;
        if ( outClicked != nullptr && input != nullptr && !b.Disabled &&
             ( ( hover && input->MouseReleased && !ctx.View.Drag.Active ) ||
               ( isFocused && input->Pressed( Common::KeyCode::Enter ) ) ) )
        {
            // Encode the structured action into the click message the runtime dispatches (same
            // encoding as the ImGui renderer, so the host dispatcher is unchanged).
            switch ( b.Action )
            {
                case UIButtonAction::LoadScene:
                    *outClicked = "scene:" + b.OnClickMessage;
                    break;
                case UIButtonAction::QuitGame:
                    *outClicked = "quit";
                    break;
                case UIButtonAction::OpenURL:
                    *outClicked = "url:" + b.OnClickMessage;
                    break;
                case UIButtonAction::ShowScreen:
                    // Handled inside the canvas — the host never sees a screen switch.
                    RequestScreen( ctx, b.OnClickMessage, false );
                    *outClicked = "screen:" + b.OnClickMessage;
                    break;
                case UIButtonAction::BackScreen:
                    RequestScreen( ctx, "", true );
                    *outClicked = "screen:back";
                    break;
                case UIButtonAction::SendEvent:
                    *outClicked = b.OnClickMessage;
                    break;
                case UIButtonAction::None:
                default:
                    break;
            }
        }
    }
} // namespace Desert::UI::Walk
