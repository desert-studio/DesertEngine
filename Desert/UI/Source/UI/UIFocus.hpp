#pragma once

// KEYBOARD / GAMEPAD FOCUS NAVIGATION — the one place that decides where focus goes next.
//
// The walk records every focusable control it drew, in draw order, together with its on-screen box (a
// FocusEntry). EndUIFrame turns the frame's keys into ONE navigation request (NavigationOf) and asks
// FindNextFocusable where it leads. Nothing else moves focus by key.
//
// Two kinds of request, as in Slate (EUINavigation):
//   * Next / Previous (Tab / Shift+Tab) walk the draw-order list, wrapping at both ends.
//   * Left / Right / Up / Down are SPATIAL: the nearest focusable in that direction whose box overlaps the
//     source's band. Port of FHittestGrid::FindFocusableWidget (SlateCore HittestGrid.cpp:319/366) without
//     the cell grid: the grid only bounds the search, the choice is the same — the candidate whose near
//     edge is closest along the axis, ties (within 0.1 px) broken by centre distance.
//
// What happens at the edge of the boundary is the rule (EUINavigationRule): Escape and Stop leave focus
// where it is, Wrap continues from the opposite side of the boundary.

#include <Common/Core/Core.hpp>
#include <Common/Core/KeyCodes.hpp>
#include <UI/Args/UINavigationArgs.hpp>
#include <UI/UILayout.hpp>
#include <UI/UITree.hpp>

#include <span>
#include <unordered_map>
#include <vector>

namespace Desert::UI
{
    struct UIInput;

    // A focusable control the frame drew, and its axis-aligned box in view pixels.
    struct FocusEntry
    {
        NodeId Node = NodeId::Null;
        Rect   Bounds{};
    };

    // One navigation request (Slate's EUINavigation).
    enum class UINavigation : uint8_t
    {
        None,
        Left,
        Right,
        Up,
        Down,
        Next,
        Previous
    };

    // The single request a frame's keys ask for: Tab = Next, Shift+Tab = Previous, arrows (and W/S for Up/
    // Down) are spatial. When several arrive in one frame the LAST event wins — the order the host saw.
    // @p textEntry: a text field holds focus, so letters are text and W/S navigate nothing. @p consumed: keys
    // a control already used this frame (a focused slider's Left/Right, an open dropdown's Up/Down) — Slate's
    // FReply::Handled, which stops the key before it becomes a navigation.
    NO_DISCARD UINavigation NavigationOf( const UIInput& input, bool textEntry = false,
                                          std::span<const Common::KeyCode> consumed = {} );

    // Where @p dir leads from @p from among @p entries (draw order). @p from not in the list (or Null) lands
    // on the FIRST entry for any request — the top of a menu. Returns NodeId::Null when the request moves
    // nothing (an empty list, or a spatial request that finds no candidate and whose rule is not Wrap).
    // @p boundary is the box Wrap restarts from (the view, unless a navigation rule names a narrower one).
    NO_DISCARD NodeId FindNextFocusable( const std::vector<FocusEntry>& entries, NodeId from, UINavigation dir,
                                         const Rect& boundary, UINavigationRule rule = UINavigationRule::Escape );

    // The box of an element that carries UINavigationData, recorded by the walk so a container that is not
    // itself focusable can still be the boundary its rule applies to.
    using NavigationBox = FocusEntry;

    // Slate's navigation entry point (SWidget::OnNavigation -> FNavigationMetaData): the request bubbles from
    // @p from up its ancestors to the first one whose rule for that direction is not Escape. Explicit goes to
    // the focusable named by that direction's target (nothing when it is not focusable this frame); Stop and
    // Wrap search inside that ancestor's box. No such ancestor: the view is the boundary, the rule Escape.
    // Next / Previous ignore the metadata.
    NO_DISCARD NodeId ResolveNavigation( const IUITree& tree, const std::vector<FocusEntry>& entries,
                                         const std::vector<NavigationBox>& boxes, NodeId from, UINavigation dir,
                                         const Rect& viewport );

    // A scrolling container's visible box this frame, and how many screen pixels one design pixel of its
    // ScrollY is.
    struct ScrollPort
    {
        NodeId Node = NodeId::Null;
        Rect   Screen{};
        float  PxPerDesign = 1.0f;
    };

    // SScrollBox::ScrollDescendantIntoView (ScrollBox.h:321, EDescendantScrollDestination::IntoView): every
    // scrolling ancestor of @p node scrolls by the least that brings its box inside the visible one, the top
    // edge winning when the box is taller than the port. Effective next frame (the walk clamps ScrollY).
    void ScrollIntoView( IUITree& tree, const std::vector<FocusEntry>& entries,
                         const std::vector<ScrollPort>& ports, NodeId node );

    // Per-scope focus memory (UCommonActivatableWidget's bAutoRestoreFocus, CommonActivatableWidget.h:224).
    // A scope is the nearest screen or overlay above a control, else its canvas.
    struct FocusMemory
    {
        std::unordered_map<NodeId, NodeId> Restore; // scope -> the control last focused inside it
        std::vector<NodeId>                Scopes;  // the scopes that had a focusable last frame, draw order
    };

    // WHY THE FOCUSED CONTROL HAS FOCUS (Slate's EFocusCause, SlateCore/Public/Input/Events.h). Recorded with
    // the control it was recorded for, so a host that moves focus itself (the editor, a script) is seen: a
    // record whose Node is not the host's focus is stale and reads as SetDirectly.
    //   * Pointer     — the pointer entered the control (UCommonButtonBase::NativeOnHovered: hover moves focus).
    //   * Navigation  — a key or gamepad step (Tab, arrows; ResolveNavigation).
    //   * SetDirectly — anything else: a screen / overlay handing focus over (UpdateFocusScopes), the host.
    enum class UIFocusCause : uint8_t
    {
        Pointer,
        Navigation,
        SetDirectly,
    };

    struct FocusRecord
    {
        NodeId       Node  = NodeId::Null;
        UIFocusCause Cause = UIFocusCause::SetDirectly;
    };

    // The cause of @p focused per @p record (SetDirectly when the record is for another control).
    NO_DISCARD UIFocusCause FocusCauseOf( const FocusRecord& record, NodeId focused );

    // Is the focus ring drawn for focus held for @p cause? Only for keyboard / gamepad navigation: the pointer
    // already shows where it is, and a screen's initial focus is not something the player did — UE's
    // ERenderFocusRule::NavigationOnly (Engine/Classes/Engine/UserInterfaceSettings.h:18-28). The rule is
    // fixed here until it becomes a setting (owner question 2: canvas field or project UI settings); that
    // setting replaces this function's body, not its callers.
    NO_DISCARD bool ShowsFocusRing( UIFocusCause cause );

    // HOVER TAKES FOCUS (UCommonButtonBase::NativeOnHovered, CommonButtonBase.h:677). The control the pointer
    // ENTERED this frame — @p pointerTarget differs from last frame's @p prevPointerTarget — takes focus when
    // it is a focusable button drawn this frame; a pointer resting still takes nothing, so an arrow key that
    // moved focus off the hovered row is not undone next frame. A text field being edited keeps its focus
    // (the pointer passing over a button must not end typing). Entering the button that already has focus
    // returns it too: focus stays, its cause becomes Pointer. Returns the control to focus, or Null.
    NO_DISCARD NodeId PointerFocusTarget( const IUITree& tree, const std::vector<FocusEntry>& entries,
                                          NodeId pointerTarget, NodeId prevPointerTarget, NodeId focused );

    // The scope @p n belongs to.
    NO_DISCARD NodeId FocusScopeOf( const IUITree& tree, NodeId n );

    // Applied once per frame after navigation. A scope that APPEARED this frame above the focused one takes
    // focus: its remembered control, else its control marked InitialFocus (GetDesiredFocusTarget,
    // CommonActivatableWidget.h:78). A focused control that is gone (its overlay closed) hands focus to the
    // topmost scope's remembered / initial control. Neither found: focus is left as it was — a menu with no
    // InitialFocus mark is not pre-selected. Returns the focus to use and records it for its scope.
    NO_DISCARD NodeId UpdateFocusScopes( const IUITree& tree, const std::vector<FocusEntry>& entries,
                                         NodeId focused, FocusMemory& memory );
} // namespace Desert::UI
