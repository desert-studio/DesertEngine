#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// EVERY STYLEABLE SLOT OF THE UI ELEMENT SET, NAMED ONCE.
//
// A theme binds slots, not fields. The difference matters in both directions:
//
//   * a theme file names `Button.Normal`, which is a stable thing to author against, while
//     `UIButtonData::NormalColor` is a C++ member that may be renamed, split or moved;
//   * a slot exists only if something DRAWS it. The register below is what makes that checkable —
//     Desert/Tests/Engine/UIStyle reads UICanvasRenderer2D.cpp and asserts that every enumerator here is
//     queried in it, so a slot nobody reads cannot be added and a slot whose read is deleted goes red.
//     That is the §1.3 "dead setting" rule applied to a table instead of to a component.
//
// THE COUNT IS DERIVED, NEVER TYPED. `StyleSlot::Count` is the enum's own end and `kStyleSlotInfo` is
// sized from it, so a new enumerator without a row is a compile error rather than an off-by-one anybody
// can paper over by editing a number (memory: "pin a register, not a count").
namespace Desert::UI
{
    // What KIND of value a slot carries, i.e. which of the theme's three tables its token must be found
    // in. A binding whose token is in the wrong table is refused when the theme is built, with both names
    // in the message — not silently dropped, and not resolved to a zero.
    enum class StyleSlotKind : uint8_t
    {
        Color,  // glm::vec3, linear, as the component's own colour fields are
        Metric, // a float: a corner radius, a border width, a padding or a spacing, in design px
        Font    // a font asset + its size; one slot supplies BOTH, because a "Body" font is a pair
    };

    // The slots, grouped by the element that draws them. Order is presentation order in the theme editor
    // and in the Details style table; nothing depends on the numeric values, and they are not serialized
    // (a theme file carries the NAME below).
    enum class StyleSlot : uint16_t
    {
        // --- Panel ---------------------------------------------------------------------------------
        PanelColor,
        PanelGradient,
        PanelBorder,
        PanelShadow,
        PanelGlow,
        PanelRingA,
        PanelRingB,
        PanelCornerRadius,
        PanelBorderWidth,

        // --- Button --------------------------------------------------------------------------------
        ButtonNormal,
        ButtonHover,
        ButtonPressed,
        ButtonSelected,
        ButtonSelectedAccent,
        ButtonDisabled,

        // --- Text ----------------------------------------------------------------------------------
        TextColor,
        TextShadow,
        TextOutline,
        TextFont,

        // --- Icon / Image --------------------------------------------------------------------------
        IconColor,
        ImageTint,

        // --- Progress bar --------------------------------------------------------------------------
        ProgressBackground,
        ProgressFill,
        ProgressCornerRadius,

        // --- Toggle --------------------------------------------------------------------------------
        ToggleBox,
        ToggleCheck,
        ToggleCornerRadius,

        // --- Slider --------------------------------------------------------------------------------
        SliderTrack,
        SliderFill,
        SliderHandle,

        // --- Scroll view ---------------------------------------------------------------------------
        ScrollViewBackground,
        ScrollViewScrollbar,

        // --- Input field ---------------------------------------------------------------------------
        InputText,
        InputPlaceholder,
        InputBackground,
        InputFocus,
        InputCornerRadius,
        InputFont,

        // --- Dropdown ------------------------------------------------------------------------------
        DropdownBackground,
        DropdownText,
        DropdownHighlight,
        DropdownCornerRadius,
        DropdownFont,

        // --- Drop target ---------------------------------------------------------------------------
        DropTargetHighlight,

        // --- Auto-layout group ---------------------------------------------------------------------
        // Padding is ONE metric applied to all four edges. A per-edge padding stays an authored vec4 on
        // the element: a theme says "panels breathe by 12 px", which is a symmetric statement, and the
        // asymmetric cases (a title bar with a deeper top inset) are layout, not livery.
        LayoutGroupPadding,
        LayoutGroupSpacing,

        // --- The chrome the WALK draws, which no element owns a field for ---------------------------
        //
        // THREE COLOURS THAT WERE LITERALS IN THE MIDDLE OF THE WALK, and that is exactly what this
        // programme's census went looking for: a UI colour nobody can reach. The keyboard focus ring is
        // an ACCESSIBILITY surface — it is the only thing that tells a keyboard user where they are —
        // and it was a `glm::vec4( 0.30f, 0.62f, 0.98f, 1.0f )` no theme, no component and no setting
        // could touch. The drag ghost is the same shape: a decoration every canvas draws and none owns.
        //
        // They resolve like every other slot, with one difference that is a property of the thing and
        // not an exception: their "local" value is the engine's built-in literal rather than a field,
        // because there is no element whose field it could be. A theme that binds them recolours them;
        // one that does not leaves the picture byte-identical to what it was.
        FocusRing,
        DragGhost,
        DragGhostBorder,

        // NOT a slot: how many there are. Everything above is added ABOVE this line.
        Count
    };

    inline constexpr std::size_t kStyleSlotCount = static_cast<std::size_t>( StyleSlot::Count );

    struct StyleSlotInfo
    {
        std::string_view Name; // what a theme file writes, e.g. "Button.Normal"
        StyleSlotKind    Kind;
    };

    // THE REGISTER. One row per enumerator, in the enumerator's own order, so `kStyleSlotInfo[(size_t)s]`
    // is the row for `s` — asserted by the suite rather than trusted, because a table indexed by an enum
    // that has silently shifted is the "two things that must agree" defect this project keeps paying for.
    inline constexpr std::array<StyleSlotInfo, kStyleSlotCount> kStyleSlotInfo = {
         StyleSlotInfo{ "Panel.Color", StyleSlotKind::Color },
         StyleSlotInfo{ "Panel.Gradient", StyleSlotKind::Color },
         StyleSlotInfo{ "Panel.Border", StyleSlotKind::Color },
         StyleSlotInfo{ "Panel.Shadow", StyleSlotKind::Color },
         StyleSlotInfo{ "Panel.Glow", StyleSlotKind::Color },
         StyleSlotInfo{ "Panel.RingA", StyleSlotKind::Color },
         StyleSlotInfo{ "Panel.RingB", StyleSlotKind::Color },
         StyleSlotInfo{ "Panel.CornerRadius", StyleSlotKind::Metric },
         StyleSlotInfo{ "Panel.BorderWidth", StyleSlotKind::Metric },

         StyleSlotInfo{ "Button.Normal", StyleSlotKind::Color },
         StyleSlotInfo{ "Button.Hover", StyleSlotKind::Color },
         StyleSlotInfo{ "Button.Pressed", StyleSlotKind::Color },
         StyleSlotInfo{ "Button.Selected", StyleSlotKind::Color },
         StyleSlotInfo{ "Button.SelectedAccent", StyleSlotKind::Color },
         StyleSlotInfo{ "Button.Disabled", StyleSlotKind::Color },

         StyleSlotInfo{ "Text.Color", StyleSlotKind::Color },
         StyleSlotInfo{ "Text.Shadow", StyleSlotKind::Color },
         StyleSlotInfo{ "Text.Outline", StyleSlotKind::Color },
         StyleSlotInfo{ "Text.Font", StyleSlotKind::Font },

         StyleSlotInfo{ "Icon.Color", StyleSlotKind::Color },
         StyleSlotInfo{ "Image.Tint", StyleSlotKind::Color },

         StyleSlotInfo{ "Progress.Background", StyleSlotKind::Color },
         StyleSlotInfo{ "Progress.Fill", StyleSlotKind::Color },
         StyleSlotInfo{ "Progress.CornerRadius", StyleSlotKind::Metric },

         StyleSlotInfo{ "Toggle.Box", StyleSlotKind::Color },
         StyleSlotInfo{ "Toggle.Check", StyleSlotKind::Color },
         StyleSlotInfo{ "Toggle.CornerRadius", StyleSlotKind::Metric },

         StyleSlotInfo{ "Slider.Track", StyleSlotKind::Color },
         StyleSlotInfo{ "Slider.Fill", StyleSlotKind::Color },
         StyleSlotInfo{ "Slider.Handle", StyleSlotKind::Color },

         StyleSlotInfo{ "ScrollView.Background", StyleSlotKind::Color },
         StyleSlotInfo{ "ScrollView.Scrollbar", StyleSlotKind::Color },

         StyleSlotInfo{ "Input.Text", StyleSlotKind::Color },
         StyleSlotInfo{ "Input.Placeholder", StyleSlotKind::Color },
         StyleSlotInfo{ "Input.Background", StyleSlotKind::Color },
         StyleSlotInfo{ "Input.Focus", StyleSlotKind::Color },
         StyleSlotInfo{ "Input.CornerRadius", StyleSlotKind::Metric },
         StyleSlotInfo{ "Input.Font", StyleSlotKind::Font },

         StyleSlotInfo{ "Dropdown.Background", StyleSlotKind::Color },
         StyleSlotInfo{ "Dropdown.Text", StyleSlotKind::Color },
         StyleSlotInfo{ "Dropdown.Highlight", StyleSlotKind::Color },
         StyleSlotInfo{ "Dropdown.CornerRadius", StyleSlotKind::Metric },
         StyleSlotInfo{ "Dropdown.Font", StyleSlotKind::Font },

         StyleSlotInfo{ "DropTarget.Highlight", StyleSlotKind::Color },

         StyleSlotInfo{ "LayoutGroup.Padding", StyleSlotKind::Metric },
         StyleSlotInfo{ "LayoutGroup.Spacing", StyleSlotKind::Metric },

         StyleSlotInfo{ "Focus.Ring", StyleSlotKind::Color },
         StyleSlotInfo{ "Drag.Ghost", StyleSlotKind::Color },
         StyleSlotInfo{ "Drag.GhostBorder", StyleSlotKind::Color },
    };

    [[nodiscard]] constexpr std::string_view StyleSlotName( StyleSlot slot )
    {
        return kStyleSlotInfo[static_cast<std::size_t>( slot )].Name;
    }

    [[nodiscard]] constexpr StyleSlotKind StyleSlotKindOf( StyleSlot slot )
    {
        return kStyleSlotInfo[static_cast<std::size_t>( slot )].Kind;
    }

    // The slot a theme file's name refers to. `Count` means "no such slot" — a MEANINGFUL answer the
    // theme builder turns into a named refusal, never into a silently dropped binding.
    [[nodiscard]] constexpr StyleSlot StyleSlotFromName( std::string_view name )
    {
        for ( std::size_t i = 0; i < kStyleSlotCount; ++i )
        {
            if ( kStyleSlotInfo[i].Name == name )
                return static_cast<StyleSlot>( i );
        }
        return StyleSlot::Count;
    }
} // namespace Desert::UI
