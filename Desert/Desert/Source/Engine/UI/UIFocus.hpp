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
#include <Engine/UI/UILayout.hpp>
#include <Engine/UI/UITree.hpp>

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

    // What a navigation does when nothing is left in its direction inside the boundary (EUINavigationRule).
    enum class UINavigationRule : uint8_t
    {
        Escape, // leave the boundary; with no outer boundary, focus stays
        Stop,   // focus stays
        Wrap    // continue from the opposite side of the boundary
    };

    // The single request a frame's keys ask for: Tab = Next, Shift+Tab = Previous, arrows (and W/S for Up/
    // Down) are spatial. When several arrive in one frame the LAST event wins — the order the host saw.
    NO_DISCARD UINavigation NavigationOf( const UIInput& input );

    // Where @p dir leads from @p from among @p entries (draw order). @p from not in the list (or Null) lands
    // on the FIRST entry for any request — the top of a menu. Returns NodeId::Null when the request moves
    // nothing (an empty list, or a spatial request that finds no candidate and whose rule is not Wrap).
    // @p boundary is the box Wrap restarts from (the view, unless a navigation rule names a narrower one).
    NO_DISCARD NodeId FindNextFocusable( const std::vector<FocusEntry>& entries, NodeId from, UINavigation dir,
                                         const Rect& boundary, UINavigationRule rule = UINavigationRule::Escape );
} // namespace Desert::UI
