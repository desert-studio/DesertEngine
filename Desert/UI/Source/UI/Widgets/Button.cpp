#include <UI/Widgets/UIWidgets.hpp>

#include <UI/UICanvasRenderer2D.hpp>
#include <UI/UIOverlay.hpp>
#include <Common/Core/AssetHandle.hpp>
#include <Render2DCore/Text/BakedFont.hpp>
#include <UI/UIStyleResolver.hpp>
#include <Render2DCore/Text/Utf8.hpp>
#include <UI/UICanvasLayout.hpp>
#include <UI/UIDataStore.hpp>
#include <UI/UIPathGeometry.hpp>
#include <UI/UIRichText.hpp>
#include <UI/UIScreenMachine.hpp>

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
        // A ShowScreen button under a tab bar is a TAB, and its Selected is not authored: it is "the screen
        // I name is current in the switcher I drive" (UCommonTabListWidgetBase, CommonTabListWidgetBase.h:108),
        // read from the same resolution a click uses, so the highlight and the switch cannot disagree.
        bool selected = b.Selected;
        if ( b.Action == UIButtonAction::ShowScreen && TabsOf( tree, e ) != NodeId::Null )
        {
            const NodeId switcher = ShowScreenTarget( ctx.Canvas, tree, e, b.OnClickMessage );
            const auto   m        = ctx.Canvas.Screens.find( switcher );
            selected              = m != ctx.Canvas.Screens.end() && m->second.Screen == b.OnClickMessage;
        }
        // Resting colour is Selected (persistent highlight) or Normal; hover cross-fades toward
        // HoverColor (eased), press snaps to PressedColor, Disabled overrides everything.
        const glm::vec3 rest = selected ? st.Color( StyleSlot::ButtonSelected, b.SelectedColor )
                                        : st.Color( StyleSlot::ButtonNormal, b.NormalColor );
        // `focused` is the HOST's, and it survives between frames -- gated by `interactive` so a control that
        // held focus while reachable is inert once an ancestor turns Blocking (no activation, ring, caret).
        const bool isFocused = interactive && focused != nullptr && *focused == e;
        // The plate answers HIGHLIGHT, not only the pointer: a focused row wears the hovered style, as UE's
        // UCommonButtonBase does (NativeOnFocusReceived -> hovered state), so keyboard/gamepad navigation shows
        // the same row the pointer would. Disabled was already folded into `hover`; it gates focus here.
        const float     ht   = HoverEase( ctx, e, ( hover || ( isFocused && !b.Disabled ) ) && !down );
        glm::vec3       c    = glm::mix( rest, st.Color( StyleSlot::ButtonHover, b.HoverColor ), ht );
        if ( down )
            c = st.Color( StyleSlot::ButtonPressed, b.PressedColor );
        if ( b.Disabled )
            c = st.Color( StyleSlot::ButtonDisabled, b.DisabledColor );

        // Image can change with state (hover / press), falling back to the normal Sprite.
        Common::AssetHandle spr = b.Sprite;
        if ( down && HandleSet( b.PressedSprite ) )
            spr = b.PressedSprite;
        else if ( hover && HandleSet( b.HoverSprite ) )
            spr = b.HoverSprite;
        DrawBox( ctx.View.Resources(), dl, mn, mx, Tinted( ctx, glm::vec4( c, b.Disabled ? 0.6f : 1.0f ) ), spr,
                 b.SpriteBorder, scale, 6.0f * scale );

        // Selected accent: a rounded bar hugging the left edge (the "you are here" marker).
        if ( selected && !b.Disabled )
        {
            const float barW  = std::max( 2.0f, 3.0f * scale );
            const float inset = 4.0f * scale;
            dl.AddRectFilled( { mn.x, mn.y + inset }, { mn.x + barW, mx.y - inset },
                              glm::vec4( st.Color( StyleSlot::ButtonSelectedAccent, b.SelectedAccent ), 1.0f ),
                              barW * 0.5f );
        }

        // HIGHLIGHTED = the row the player is on, by pointer or by keyboard/gamepad focus. Recorded before the
        // children draw, so their WhileHighlighted tweens (UITweenArgs.hpp) answer this frame, not the last.
        if ( !b.Disabled && ( hover || isFocused ) )
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
                    RequestScreen( ctx, tree, e, b.OnClickMessage, false );
                    *outClicked = "screen:" + b.OnClickMessage;
                    break;
                case UIButtonAction::BackScreen:
                    RequestScreen( ctx, tree, e, "", true );
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
