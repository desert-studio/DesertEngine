#pragma once

#include <CoreReflection/ReflectionMacros.hpp>
#include <UI/Args/ArgKind.hpp>

#include <string>
#include <glm/glm.hpp>

// Value-holding controls: progress bar, toggle, slider, text input, dropdown.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // A horizontal progress/health bar: a background track with a fill spanning Value (0..1) of the width.
    // Display-only (no interaction).
    struct UIProgressBarData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::ProgressBar;

        PROPERTY( DisplayName( "Value" ), Category( "UI Progress Bar" ), Range( 0.0f, 1.0f ) )
        float Value = 0.5f;

        PROPERTY( DisplayName( "Background" ), Category( "UI Progress Bar" ), Color )
        glm::vec3 Background = glm::vec3( 0.12f, 0.13f, 0.16f );

        PROPERTY( DisplayName( "Fill" ), Category( "UI Progress Bar" ), Color )
        glm::vec3 Fill = glm::vec3( 0.30f, 0.65f, 0.35f );

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Progress Bar" ), Range( 0.0f, 32.0f ) )
        float CornerRadius = 4.0f;
    };

    // A checkbox: a box that fills with the check colour when on. A click (runtime) flips Value.
    struct UIToggleData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Toggle;

        PROPERTY( DisplayName( "Value (on)" ), Category( "UI Toggle" ) )
        bool Value = false;

        PROPERTY( DisplayName( "Box Color" ), Category( "UI Toggle" ), Color )
        glm::vec3 BoxColor = glm::vec3( 0.18f, 0.19f, 0.24f );

        PROPERTY( DisplayName( "Check Color" ), Category( "UI Toggle" ), Color )
        glm::vec3 CheckColor = glm::vec3( 0.30f, 0.60f, 0.90f );

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Toggle" ), Range( 0.0f, 32.0f ) )
        float CornerRadius = 4.0f;
    };

    // A horizontal slider: a track + a fill up to the handle + a draggable handle. Dragging (runtime) sets
    // Value in [MinValue, MaxValue].
    struct UISliderData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Slider;

        PROPERTY( DisplayName( "Value" ), Category( "UI Slider" ) )
        float Value = 0.5f;

        PROPERTY( DisplayName( "Min" ), Category( "UI Slider" ) )
        float MinValue = 0.0f;

        PROPERTY( DisplayName( "Max" ), Category( "UI Slider" ) )
        float MaxValue = 1.0f;

        // USlider::StepSize: one Left / Right press while the slider holds focus moves Value by this much.
        PROPERTY( DisplayName( "Step Size" ), Category( "UI Slider" ), Range( 0.0001f, 1000.0f ),
                  Tooltip( "How far one Left / Right key press moves the value while the slider is focused" ) )
        float StepSize = 0.01f;

        PROPERTY( DisplayName( "Track Color" ), Category( "UI Slider" ), Color )
        glm::vec3 TrackColor = glm::vec3( 0.12f, 0.13f, 0.16f );

        PROPERTY( DisplayName( "Fill Color" ), Category( "UI Slider" ), Color )
        glm::vec3 FillColor = glm::vec3( 0.30f, 0.52f, 0.82f );

        PROPERTY( DisplayName( "Handle Color" ), Category( "UI Slider" ), Color )
        glm::vec3 HandleColor = glm::vec3( 0.90f, 0.92f, 0.96f );
    };

    // Which typed characters an input field accepts (UE: SEditableText's OnIsTypedCharValid, made data).
    // A refused character is dropped, never replaced; a paste goes through the same filter.
    enum class UITextCharFilter
    {
        Any,          // every printable character
        Integer,      // digits, and a minus sign only as the first character
        Decimal,      // Integer plus one decimal point
        Alphanumeric, // letters and digits (any script), nothing else
    };

    // Which Enter makes a new line in a multi-line field (UE: UMultiLineEditableText::ModiferKeyForNewLine).
    // The other Enter commits — so a chat box sends on Enter and breaks the line on Shift+Enter.
    enum class UITextNewLineKey
    {
        ShiftEnter, // Shift+Enter breaks the line, Enter commits (chat)
        Enter,      // Enter breaks the line; the field commits only when focus leaves it (notes)
    };

    // A text input (UE's SEditableText / SMultiLineEditableText, editing ported from
    // FSlateEditableTextLayout). Click focuses it and places the caret; drag selects, double-click selects
    // a word. Keys: arrows (word jumps with Ctrl/Alt), Home/End, Backspace/Delete, Ctrl|Cmd + A/C/X/V/Z/Y.
    // The caret, the selection and the undo history are runtime state of the VIEW (UITextEditState in
    // UICanvasContext), never serialized. Placeholder shows (dimmed) when empty + unfocused.
    struct UIInputFieldData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::InputField;

        PROPERTY( DisplayName( "Text" ), Category( "UI Input Field" ) )
        std::string Text;

        // Localisable on the same terms as UIText::Text — a leading hash is a string-table key. The user's
        // own typed `Text` above is NOT: it is what the player wrote, and translating it would be absurd.
        PROPERTY( DisplayName( "Placeholder" ), Category( "UI Input Field" ),
                  Tooltip( "Shown while empty. A leading hash makes it a string-table key instead" ) )
        std::string Placeholder = "Enter text...";

        PROPERTY( DisplayName( "Font Size" ), Category( "UI Input Field" ), Range( 6.0f, 96.0f ) )
        float FontSize = 20.0f;

        PROPERTY( DisplayName( "Text Color" ), Category( "UI Input Field" ), Color )
        glm::vec3 TextColor = glm::vec3( 0.92f, 0.94f, 0.98f );

        PROPERTY( DisplayName( "Placeholder Color" ), Category( "UI Input Field" ), Color )
        glm::vec3 PlaceholderColor = glm::vec3( 0.45f, 0.47f, 0.52f );

        PROPERTY( DisplayName( "Background" ), Category( "UI Input Field" ), Color )
        glm::vec3 Background = glm::vec3( 0.10f, 0.11f, 0.14f );

        PROPERTY( DisplayName( "Focus Border" ), Category( "UI Input Field" ), Color )
        glm::vec3 FocusColor = glm::vec3( 0.30f, 0.55f, 0.90f );

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Input Field" ), Range( 0.0f, 32.0f ) )
        float CornerRadius = 4.0f;

        PROPERTY( DisplayName( "Password" ), Category( "UI Input Field" ),
                  Tooltip( "Draws every character as a bullet; copy and cut are refused" ) )
        bool Password = false;

        PROPERTY( DisplayName( "Max Length" ), Category( "UI Input Field" ), Range( 0.0f, 100000.0f ),
                  Tooltip( "Most characters the field holds (0 = unlimited); typing and pasting stop there" ) )
        int MaxLength = 0;

        PROPERTY( DisplayName( "Character Filter" ), Category( "UI Input Field" ) )
        UITextCharFilter CharFilter = UITextCharFilter::Any;

        PROPERTY( DisplayName( "Multi Line" ), Category( "UI Input Field" ),
                  Tooltip( "Text may hold line breaks; drawn from the top, scrolled to keep the caret in view" ) )
        bool MultiLine = false;

        PROPERTY( DisplayName( "New Line Key" ), Category( "UI Input Field" ),
                  Tooltip( "Multi-line only: which Enter breaks the line; the other one commits" ) )
        UITextNewLineKey NewLineKey = UITextNewLineKey::ShiftEnter;

        // Messages go out like a button's SendEvent (UI::UIMessageQueue -> Lua OnUIMessage) as
        // "<message>|<text>": OnChanged on every edit, OnCommitted on Enter and when focus leaves the field
        // (UE: SEditableText::OnTextChanged / OnTextCommitted). Empty = not sent.
        PROPERTY( DisplayName( "On Changed Message" ), Category( "UI Input Field" ) )
        std::string OnChangedMessage;

        PROPERTY( DisplayName( "On Committed Message" ), Category( "UI Input Field" ) )
        std::string OnCommittedMessage;
    };

    // A dropdown / combo box. Shows the selected option; a click opens a list of Options (';'-separated) below
    // it, drawn on top of everything. Picking an option sets SelectedIndex and closes.
    struct UIDropdownData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Dropdown;

        // EACH OPTION is localisable on its own — a leading hash on one entry makes that entry a key, and
        // the separator is not part of any of them. Per option rather than per list because a dropdown
        // mixes translated labels with proper nouns (a server name, a player's own preset) far more often
        // than it is wholly one or the other.
        PROPERTY( DisplayName( "Options (';'-separated)" ), Category( "UI Dropdown" ),
                  Tooltip( "One entry per option. A leading hash on an entry makes that entry a "
                           "string-table key" ) )
        std::string Options = "Option A;Option B;Option C";

        PROPERTY( DisplayName( "Selected Index" ), Category( "UI Dropdown" ) )
        int SelectedIndex = 0;

        PROPERTY( DisplayName( "Open" ), Category( "UI Dropdown" ) )
        bool Open = false;

        PROPERTY( DisplayName( "Font Size" ), Category( "UI Dropdown" ), Range( 6.0f, 96.0f ) )
        float FontSize = 20.0f;

        PROPERTY( DisplayName( "Background" ), Category( "UI Dropdown" ), Color )
        glm::vec3 Background = glm::vec3( 0.16f, 0.17f, 0.21f );

        PROPERTY( DisplayName( "Text Color" ), Category( "UI Dropdown" ), Color )
        glm::vec3 TextColor = glm::vec3( 0.92f, 0.94f, 0.98f );

        PROPERTY( DisplayName( "Highlight" ), Category( "UI Dropdown" ), Color )
        glm::vec3 Highlight = glm::vec3( 0.26f, 0.40f, 0.62f );

        PROPERTY( DisplayName( "Corner Radius" ), Category( "UI Dropdown" ), Range( 0.0f, 32.0f ) )
        float CornerRadius = 4.0f;
    };
} // namespace Desert::UI
