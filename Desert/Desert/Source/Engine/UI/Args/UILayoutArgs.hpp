#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <glm/glm.hpp>

// Where an element sits and whether it is seen or hit: the anchor/offset rect and the auto-layout container.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // Auto-layout container type. A UILayoutGroup on an element positions + sizes its DIRECT children
    // automatically (overriding their anchors) — the Unity/Godot "layout group" model.
    enum class UILayoutType
    {
        Vertical,   // VBox: children top -> bottom
        Horizontal, // HBox: children left -> right
        Grid,        // fixed cells, wrapping into rows
        Wrap,        // UE WrapBox: children at their preferred size in lines that wrap
        Overlay,     // UE Overlay: every child over the same rect, later ones on top (a Z stack)
        UniformGrid, // UE UniformGridPanel: equal cells, as large as the largest child, sharing the rect
        SizeBox,     // UE SizeBox: the content's desired size clamped by Min/Max or replaced by Override
        ScaleBox     // UE ScaleBox: the content keeps its desired size and is scaled to fit the rect
    };

    // ScaleBox stretch rule (UE EStretch). Appended-only: the scene stores the enumerator.
    enum class UIScaleStretch
    {
        None,
        Fill,
        ScaleToFit,
        ScaleToFitX,
        ScaleToFitY,
        ScaleToFill,
        UserSpecified
    };

    // Which way a ScaleBox may scale (UE EStretchDirection).
    enum class UIScaleDirection
    {
        Both,
        DownOnly,
        UpOnly
    };

    // Add to an element to auto-arrange its children. Children keep their UILayout for appearance + preferred
    // size (from CustomMinimumSize, else the offset size), but their POSITION/size comes from the group.
    struct UILayoutGroupData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::LayoutGroup;

        PROPERTY( DisplayName( "Type" ), Category( "UI Layout Group" ) )
        UILayoutType Type = UILayoutType::Vertical;

        PROPERTY( DisplayName( "Padding L/T/R/B" ), Category( "UI Layout Group" ) )
        glm::vec4 Padding = glm::vec4( 8.0f );

        PROPERTY( DisplayName( "Spacing" ), Category( "UI Layout Group" ), Range( 0.0f, 128.0f ) )
        float Spacing = 6.0f;

        PROPERTY( DisplayName( "Stretch Children (cross axis)" ), Category( "UI Layout Group" ) )
        bool StretchCross = true;

        PROPERTY( DisplayName( "Grid Cell Size" ), Category( "UI Layout Group" ) )
        glm::vec2 CellSize = glm::vec2( 100.0f, 100.0f );

        PROPERTY( DisplayName( "Grid Columns (0 = auto)" ), Category( "UI Layout Group" ), Range( 0.0f, 64.0f ) )
        int Columns = 0;

        // --- Wrap -----------------------------------------------------------------------------------
        PROPERTY( DisplayName( "Wrap Size (0 = container)" ), Category( "UI Layout Group" ), Range( 0.0f, 4096.0f ) )
        float WrapSize = 0.0f;

        PROPERTY( DisplayName( "Wrap Vertically" ), Category( "UI Layout Group" ) )
        bool WrapVertical = false;

        // --- Uniform Grid ---------------------------------------------------------------------------
        PROPERTY( DisplayName( "Min Slot Size" ), Category( "UI Layout Group" ) )
        glm::vec2 MinSlotSize = glm::vec2( 0.0f, 0.0f );

        // --- Size Box: per axis, NEGATIVE = not set (0 is a legal size) -------------------------------
        PROPERTY( DisplayName( "Size Box Min (-1 = unset)" ), Category( "UI Layout Group" ) )
        glm::vec2 SizeMin = glm::vec2( -1.0f, -1.0f );

        PROPERTY( DisplayName( "Size Box Max (-1 = unset)" ), Category( "UI Layout Group" ) )
        glm::vec2 SizeMax = glm::vec2( -1.0f, -1.0f );

        PROPERTY( DisplayName( "Size Box Override (-1 = unset)" ), Category( "UI Layout Group" ) )
        glm::vec2 SizeOverride = glm::vec2( -1.0f, -1.0f );

        // --- Scale Box ------------------------------------------------------------------------------
        PROPERTY( DisplayName( "Stretch" ), Category( "UI Layout Group" ) )
        UIScaleStretch Stretch = UIScaleStretch::ScaleToFit;

        PROPERTY( DisplayName( "Stretch Direction" ), Category( "UI Layout Group" ) )
        UIScaleDirection StretchDirection = UIScaleDirection::Both;

        PROPERTY( DisplayName( "User Scale" ), Category( "UI Layout Group" ), Range( 0.0f, 16.0f ) )
        float UserScale = 1.0f;
    };

    // Aspect Ratio Fitter mode: which axis is derived from the other to hold the ratio (or off).
    enum class UIAspectMode
    {
        Off,
        HeightControlsWidth, // width = height * ratio
        WidthControlsHeight  // height = width / ratio
    };

    // ------------------------------------------------------------------------------------------------
    // ELEMENT VISIBILITY — TWO AXES, NOT ONE FIVE-VALUED WORD
    //
    // UE spells this as ESlateVisibility, one enum with five values. Decomposed, those five answer two
    // independent questions that were glued together, which is where the awkward names come from:
    //
    //   ESlateVisibility       drawn  keeps layout space  self hit-tests  children hit-test
    //   Visible                 yes         yes                yes              yes
    //   Hidden                  no          yes                no               no
    //   Collapsed               no          no                 no               no
    //   HitTestInvisible        yes         yes                no               no
    //   SelfHitTestInvisible    yes         yes                no               yes
    //
    // The first two columns are one question and the last two are another, so the two enums below are
    // that decomposition. Two fields are simpler to author and to serialize, they are strictly more
    // expressive than the five words (UE exposes five of the products; every product here is reachable),
    // and the second one is exactly the shape the walk already had — per-element flags.
    // ------------------------------------------------------------------------------------------------

    // Is the element on screen, and does it still hold its place in the parent's layout?
    //
    // Collapsed vs Hidden is the whole reason this axis exists and is only observable inside a
    // UILayoutGroup (VBox / HBox / Grid): a Collapsed child gets no slot and its siblings close the gap,
    // a Hidden one keeps its slot and leaves a hole. Under plain anchor layout the two look identical,
    // because siblings there are positioned against the parent and have nothing to close up.
    //
    // Neither is hit-testable: an element nobody can see must not eat clicks, which is also UE's rule.
    // Both take their whole sub-tree with them.
    enum class UIVisibility
    {
        Visible,  // drawn, holds its slot, hit-tested per UIHitTest below
        Hidden,   // not drawn, KEEPS its slot in the parent's layout group, hit-tests nothing
        Collapsed // not drawn, DROPS OUT of the parent's layout group, hit-tests nothing
    };

    // What the pointer sees of this element AND of everything under it.
    //
    // THIS AXIS ABSORBED THE TWO BOOLEANS THAT USED TO SIT HERE, and the mapping is:
    //   RaycastTarget = false  ->  ChildrenOnly   (identical behaviour: the element is transparent to the
    //                                              pointer, its children are not — UE SelfHitTestInvisible)
    //   Interactable  = false  ->  Blocking       (the element stops the pointer and responds to nothing;
    //                                              what is NEW is that its sub-tree is inert too, so
    //                                              greying out a form or a modal dialog is one field
    //                                              instead of one field per descendant)
    // `None` is the value neither boolean could express — UE's HitTestInvisible, where the element and its
    // whole sub-tree are transparent, so a decorative overlay lets every click through to what is behind.
    //
    // WHY FOUR VALUES AND NOT THREE. UE needs a second, separate concept (IsEnabled) for "visible, blocks
    // the pointer, responds to nothing", which is precisely what our Interactable was. Folding it in here
    // rather than leaving it beside this field keeps one source of truth for "what does the pointer do
    // with this element", at the price of one extra enumerator.
    enum class UIHitTest
    {
        All,          // the element and its children take the pointer and respond (the default)
        ChildrenOnly, // the element is transparent to the pointer; its children still take it
        Blocking,     // the element stops the pointer; neither it nor its sub-tree responds to anything
        None          // the element and its whole sub-tree are transparent to the pointer
    };

    // Godot Control-like rect: anchors (fraction of the parent rect, 0..1), offsets (pixels from the anchored
    // edges), a custom minimum size and content clipping. The layout solver turns these into a screen
    // rect each frame. AnchorMin==AnchorMax => fixed-size element positioned by offsets; spread anchors =>
    // element stretches with the parent.
    //
    // PIVOT IS BACK, AND IT IS BACK WITH ITS CONSUMER. Д26 deleted it because the rect was resolved from
    // anchors and offsets alone and nothing in this UI rotated or scaled an element about a point, which
    // made it a knob that moved nothing (§1.3). Ю8 is the rotation, so the three fields below arrive
    // TOGETHER: Pivot on its own would be dead again, and Rotation without Pivot could only ever turn an
    // element about its centre.
    struct UILayoutData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Layout;

        PROPERTY( DisplayName( "Anchor Min" ), Category( "UI Layout" ) )
        glm::vec2 AnchorMin = glm::vec2( 0.0f, 0.0f );

        PROPERTY( DisplayName( "Anchor Max" ), Category( "UI Layout" ) )
        glm::vec2 AnchorMax = glm::vec2( 0.0f, 0.0f );

        PROPERTY( DisplayName( "Offset Min" ), Category( "UI Layout" ) )
        glm::vec2 OffsetMin = glm::vec2( 0.0f, 0.0f ); // px from the AnchorMin edges (left/top)

        PROPERTY( DisplayName( "Offset Max" ), Category( "UI Layout" ) )
        glm::vec2 OffsetMax = glm::vec2( 160.0f, 48.0f ); // px from the AnchorMax edges (right/bottom)

        PROPERTY( DisplayName( "Custom Minimum Size" ), Category( "UI Layout" ) )
        glm::vec2 CustomMinimumSize = glm::vec2( 0.0f, 0.0f );

        // --- Render transform (Ю8) ---------------------------------------------------------------------
        // These do NOT take part in layout: the rect is still resolved from anchors and offsets, and a
        // rotated element occupies exactly the slot it would have occupied straight. They are applied
        // afterwards, to the geometry that rect produces and to the pointer that hits it, and they are
        // INHERITED — a rotated panel carries its whole sub-tree with it, because the transform is pushed
        // before the children are walked and popped after.
        //
        // WHY LAYOUT IS LEFT ALONE. The alternative — feeding the rotated bounds back into the parent's
        // auto-layout — makes a slider that spins an element also resize its siblings, and it makes the
        // layout solution depend on its own output for a nested rotation. Unity and Godot both draw this
        // line in the same place.

        // Degrees, POSITIVE = CLOCKWISE on screen (this space has y pointing down — the CSS `rotate()`
        // and Godot Control.rotation convention). About Pivot.
        PROPERTY( DisplayName( "Rotation" ), Category( "UI Transform" ), Range( -360.0f, 360.0f ) )
        float Rotation = 0.0f;

        // Multiplies the element's own size about Pivot. Non-uniform is allowed and is what an author
        // reaches for to flip a sprite (-1 on one axis). 1,1 = untouched, and an untouched element emits
        // byte-identical geometry to one with no transform at all — the walk skips the matrix entirely.
        PROPERTY( DisplayName( "Scale" ), Category( "UI Transform" ) )
        glm::vec2 Scale = glm::vec2( 1.0f, 1.0f );

        // The point Rotation and Scale act about, as a FRACTION of this element's own resolved rect:
        // 0,0 = its top-left corner, 0.5,0.5 = its centre, 1,1 = its bottom-right. A fraction rather
        // than pixels so it keeps its meaning when the element is resized or the canvas is scaled.
        PROPERTY( DisplayName( "Pivot" ), Category( "UI Transform" ) )
        glm::vec2 Pivot = glm::vec2( 0.5f, 0.5f );

        PROPERTY( DisplayName( "Clip Contents" ), Category( "UI Layout" ) )
        bool ClipContents = false;

        // Is this element on screen, and does it keep its place when it is not? Collapsed is the one that
        // changes the LAYOUT: inside a VBox/HBox/Grid it drops the element's slot and the siblings close
        // up, where Hidden leaves the hole. See UIVisibility.
        PROPERTY( DisplayName( "Visibility" ), Category( "UI Layout" ) )
        UIVisibility Visibility = UIVisibility::Visible;

        // What the pointer sees of this element and of its whole sub-tree. Replaces the Interactable /
        // Raycast Target pair, which said the same things per element and could not say them about a
        // sub-tree at all — see UIHitTest for which old flag became which value.
        PROPERTY( DisplayName( "Hit Test" ), Category( "UI Interaction" ) )
        UIHitTest HitTest = UIHitTest::All;

        // Aspect Ratio Fitter (Phase B): keep this width/height ratio, deriving the free axis about the centre.
        PROPERTY( DisplayName( "Aspect Ratio (W/H)" ), Category( "Fitter" ), Range( 0.0f, 8.0f ) )
        float AspectRatio = 0.0f; // 0 = off
        PROPERTY( DisplayName( "Aspect Mode" ), Category( "Fitter" ) )
        UIAspectMode AspectMode = UIAspectMode::HeightControlsWidth;

        // Layout Element (Phase B): inside a parent VBox/HBox, flexible children share the leftover main-axis
        // space by weight — >0 stretches this child to fill (or acts as a spacer). 0 = fixed preferred size.
        PROPERTY( DisplayName( "Flex Grow" ), Category( "Fitter" ), Range( 0.0f, 8.0f ) )
        float FlexGrow = 0.0f;

        // Slate StretchContent shrink: when a VBox/HBox's children overflow it, this child gives the overflow
        // back in proportion to FlexShrink * its preferred size, never below its Custom Minimum Size.
        // 0 = keeps its preferred size (the children overflow, as before shrink existed).
        PROPERTY( DisplayName( "Flex Shrink" ), Category( "Fitter" ), Range( 0.0f, 8.0f ) )
        float FlexShrink = 0.0f;

        // Content Size Fitter (Phase B): a layout-group container sizes itself to its children (hug content),
        // per axis. Keeps the anchored top-left. No effect on non-group elements.
        PROPERTY( DisplayName( "Fit Width To Content" ), Category( "Fitter" ) )
        bool FitWidth = false;
        PROPERTY( DisplayName( "Fit Height To Content" ), Category( "Fitter" ) )
        bool FitHeight = false;
    };
} // namespace Desert::UI
