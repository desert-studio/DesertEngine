#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>
#include <Engine/UI/Args/UIPanelArgs.hpp>

#include <string>
#include <glm/glm.hpp>

// A clickable panel and the action it hands to the host.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // Interactive button: tints its panel by pointer state, and on click hands its encoded action to the
    // HOST (RenderCanvas2D writes it to `outClicked`) -- the canvas itself knows nothing about scenes,
    // URLs or scripting. LoadScene/QuitGame/OpenURL are executed by the host; everything else, SendEvent
    // included, goes on UI::UIMessageQueue and reaches every Lua script defining OnUIMessage. The word
    // "dispatches ... to Lua" used to stand here and was FALSE: both hosts logged the message and never
    // queued it, so the one action documented as a "gameplay event name" was the one that arrived nowhere.
    //
    // What a UI Button does when clicked. The target/payload is the button's "Action Target" string:
    //  LoadScene   -> Core::OpenLevel(target): project-relative scene path, empty = the default map
    //  SendEvent   -> gameplay event name (Lua/scripts)
    //  QuitGame    -> quit (target ignored)        OpenURL     -> open the URL
    enum class UIButtonAction
    {
        None,
        SendEvent,
        LoadScene,
        QuitGame,
        OpenURL,
        ShowScreen, // switch the canvas to the UIScreen named in On Click Message (pushes onto the stack)
        BackScreen  // return to the screen underneath (does nothing at the bottom of the stack)
    };

    struct UIButtonData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Button;

        PROPERTY( DisplayName( "Normal" ), Category( "UI Button" ), Color )
        glm::vec3 NormalColor = glm::vec3( 0.20f, 0.40f, 0.70f );

        PROPERTY( DisplayName( "Hover" ), Category( "UI Button" ), Color )
        glm::vec3 HoverColor = glm::vec3( 0.30f, 0.52f, 0.82f );

        PROPERTY( DisplayName( "Pressed" ), Category( "UI Button" ), Color )
        glm::vec3 PressedColor = glm::vec3( 0.15f, 0.30f, 0.55f );

        PROPERTY( DisplayName( "Click Action" ), Category( "UI Button" ) )
        UIButtonAction Action = UIButtonAction::SendEvent;

        PROPERTY( DisplayName( "Action Target" ), Category( "UI Button" ) )
        std::string OnClickMessage = ""; // scene path / message name / URL, depending on Action

        // All three carry Asset<TextureAsset> for the reason UIPanelData::Sprite states in full: the
        // annotation is what names the asset TYPE to the serializer, and without it the resolver wrote
        // each of these out as an empty string. UICanvasRenderer2D reads all three (a button picks
        // Pressed, then Hover, then Sprite), so they were live in the draw and dead in the file.
        PROPERTY( DisplayName( "Sprite" ), Category( "UI Button" ), Asset<TextureAsset> )
        Assets::AssetHandle Sprite; // normal-state image, tinted by the state colour. Unset = flat colour.

        PROPERTY( DisplayName( "Hover Sprite" ), Category( "UI Button" ), Asset<TextureAsset> )
        Assets::AssetHandle HoverSprite; // shown on hover (falls back to Sprite if unset)

        PROPERTY( DisplayName( "Pressed Sprite" ), Category( "UI Button" ), Asset<TextureAsset> )
        Assets::AssetHandle PressedSprite; // shown while pressed (falls back to Sprite if unset)

        PROPERTY( DisplayName( "Sprite Border L/T/R/B" ), Category( "UI Button" ) )
        glm::vec4 SpriteBorder = glm::vec4( 0.0f ); // 9-slice: source-px borders kept unstretched (0 = stretch)

        // --- States (Phase D) -----------------------------------------------------------------------------
        PROPERTY( DisplayName( "Selected" ), Category( "State" ) )
        bool Selected = false; // persistent highlight (active menu item / current tab): rests on SelectedColor
                               // and draws an accent bar, until hover/press temporarily override it
        PROPERTY( DisplayName( "Selected Color" ), Category( "State" ), Color )
        glm::vec3 SelectedColor = glm::vec3( 0.85f, 0.42f, 0.18f );
        PROPERTY( DisplayName( "Selected Accent" ), Category( "State" ), Color )
        glm::vec3 SelectedAccent = glm::vec3( 1.0f, 0.55f, 0.2f ); // the left accent bar / ring colour

        PROPERTY( DisplayName( "Disabled" ), Category( "State" ) )
        bool Disabled = false; // greyed + non-interactive (ignores hover/press/click)
        PROPERTY( DisplayName( "Disabled Color" ), Category( "State" ), Color )
        glm::vec3 DisabledColor = glm::vec3( 0.22f, 0.24f, 0.28f );
    };
} // namespace Desert::UI
