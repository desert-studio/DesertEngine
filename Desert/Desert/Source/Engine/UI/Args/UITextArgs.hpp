#pragma once

#include <Engine/Assets/Common.hpp>
#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <string>
#include <glm/glm.hpp>

// A text block and its alignment/overflow policy.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // Horizontal text alignment within a UI element's rect. Reflected enum -> editor combo + serialization.
    enum class UITextAlign
    {
        Left,
        Center,
        Right
    };

    // Vertical placement of the (possibly multi-line) text block within the element rect.
    enum class UITextVAlign
    {
        Top,
        Middle,
        Bottom
    };

    // What to do when the text is wider/taller than the element rect (mutually resolved in this order:
    // AutoSize shrinks first, then Wrap breaks lines, then Ellipsis truncates the overflow).
    enum class UITextOverflow
    {
        Overflow, // draw past the rect (legacy behaviour)
        Ellipsis, // truncate the overflowing tail with "…"
        Clip      // hard-clip to the rect (no ellipsis)
    };

    // Screen-space text label (distinct from the 3D world-space TextComponent).
    struct UITextData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Text;

        // A LEADING '#' MAKES THIS A KEY into the project's string tables ("#menu.play"); anything else is
        // a literal and is never translated. '##' at the start is an escape for a literal '#'. One field,
        // because two (a literal and a key, with a rule about which wins) is a defect class this project
        // has a name for — see Engine/Localization/LocalizedText.hpp for why this shape was chosen over
        // the other two.
        PROPERTY( DisplayName( "Text" ), Category( "UI Text" ),
                  Tooltip( "Shown as typed. A leading hash makes it a string-table key instead" ) )
        std::string Text = "Label";

        PROPERTY( DisplayName( "Font Size" ), Category( "UI Text" ), Range( 6.0f, 200.0f ) )
        float FontSize = 22.0f;

        PROPERTY( DisplayName( "Font" ), Category( "UI Text" ), Asset<FontAsset> )
        Assets::AssetHandle Font; // SDF font asset — drag a .ttf from the Content Browser or pick a preloaded
                                  // one. Unset = the engine's built-in default (Roboto).

        PROPERTY( DisplayName( "Color" ), Category( "UI Text" ), Color )
        glm::vec3 Color = glm::vec3( 1.0f, 1.0f, 1.0f );

        PROPERTY( DisplayName( "Alignment" ), Category( "UI Text" ) )
        UITextAlign Align = UITextAlign::Center;

        PROPERTY( DisplayName( "Vertical Align" ), Category( "UI Text" ) )
        UITextVAlign VerticalAlign = UITextVAlign::Middle;

        // --- Layout (Phase E) ---------------------------------------------------------------------------
        PROPERTY( DisplayName( "Word Wrap" ), Category( "Layout" ) )
        bool Wrap = false; // break long lines at word boundaries to fit the element width

        PROPERTY( DisplayName( "Line Spacing" ), Category( "Layout" ), Range( 0.5f, 3.0f ) )
        float LineSpacing = 1.0f; // multiplier on the font's natural line height

        PROPERTY( DisplayName( "Auto Size" ), Category( "Layout" ) )
        bool AutoSize = false; // shrink the font (down to Min Font Size) until the block fits the rect

        PROPERTY( DisplayName( "Min Font Size" ), Category( "Layout" ), Range( 4.0f, 200.0f ) )
        float MinFontSize = 8.0f; // floor for Auto Size

        PROPERTY( DisplayName( "Overflow" ), Category( "Layout" ) )
        UITextOverflow Overflow = UITextOverflow::Overflow; // what to do when the text still doesn't fit

        PROPERTY( DisplayName( "Rich Text" ), Category( "Layout" ) )
        bool RichText = false; // parse BBCode tags: [color=#rrggbb]..[/color], [b]..[/b]

        // --- Animation (Phase F) --------------------------------------------------------------------------
        PROPERTY( DisplayName( "Marquee" ), Category( "Animation" ) )
        bool Marquee = false; // horizontally scroll the text (single line, clipped) — a news/ticker banner
        PROPERTY( DisplayName( "Marquee Speed" ), Category( "Animation" ), Range( 5.0f, 400.0f ) )
        float MarqueeSpeed = 60.0f; // design px/sec

        // Effects (Phase 4).
        PROPERTY( DisplayName( "Shadow" ), Category( "Effects" ) )
        bool Shadow = false;
        PROPERTY( DisplayName( "Shadow Color" ), Category( "Effects" ), Color )
        glm::vec3 ShadowColor = glm::vec3( 0.0f );
        PROPERTY( DisplayName( "Shadow Offset" ), Category( "Effects" ) )
        glm::vec2 ShadowOffset = glm::vec2( 1.0f, 1.0f );
        PROPERTY( DisplayName( "Outline" ), Category( "Effects" ) )
        bool Outline = false;
        PROPERTY( DisplayName( "Outline Color" ), Category( "Effects" ), Color )
        glm::vec3 OutlineColor = glm::vec3( 0.0f );
        // A soft halo around the glyphs (logo titles over a dark sky): rings of the glyphs pushed out to
        // Glow Radius, fainter with distance, drawn beneath the text.
        PROPERTY( DisplayName( "Glow" ), Category( "Effects" ) )
        bool Glow = false;
        PROPERTY( DisplayName( "Glow Color" ), Category( "Effects" ), Color, EditCondition( "Glow" ) )
        glm::vec3 GlowColor = glm::vec3( 1.0f, 0.70f, 0.35f );
        PROPERTY( DisplayName( "Glow Radius" ), Category( "Effects" ), Range( 0.0f, 32.0f ), Units( "px" ),
                  EditCondition( "Glow" ) )
        float GlowRadius = 6.0f; // design px
        PROPERTY( DisplayName( "Glow Strength" ), Category( "Effects" ), Range( 0.0f, 1.0f ),
                  EditCondition( "Glow" ) )
        float GlowStrength = 0.5f;
    };
} // namespace Desert::UI
