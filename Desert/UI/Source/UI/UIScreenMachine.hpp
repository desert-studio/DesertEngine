#pragma once

// THE SCREEN MACHINES OF ONE CANVAS — UE's UWidgetSwitcher / UCommonActivatableWidgetStack, nested.
//
// A UIScreen belongs to its NEAREST ancestor that carries a UIScreenStack, or to the canvas when no
// ancestor below it does. Each such owner runs its own machine: which of ITS screens is current, the
// hand-over in flight, and its back history. So a Settings screen of the main menu can hold its own
// General / Video / Audio switcher, and switching a tab moves only the inner machine — the outer one keeps
// showing Settings. Before this, every screen of a canvas was one flat machine, and a tab page and the menu
// page that contained it competed for the same "current" name.
//
// The owner is decided by walking UP from the screen, which is the depth of the tree and not its size, and
// the walk stops at the first stack or canvas — a stack's screens never leak into an outer stack.
//
// Tabs (UITabsData, UE's UCommonTabListWidgetBase): a ShowScreen button under a UITabs element is a TAB. It
// shows as Selected exactly while the screen it targets is current in the machine it targets — the author
// never ticks Selected by hand (CommonTabListWidgetBase.h:108, the list selects the tab whose switcher index
// is active). A tab switch does not grow the back history unless the tab bar asks for it, because a
// switcher is not a stack: Escape on a tabbed page returns to the page before it, not to the previous tab.
//
// Back (Escape, or a BackScreen button) is handled by ONE machine: the button's nearest owner that has
// somewhere to go back to, or — for Escape, which has no element — the deepest ACTIVE machine (every screen
// above it current) that does, topmost canvas first. That is UCommonActivatableWidgetStack's rule: the leaf
// of the active chain takes the back action, and an open overlay takes it before any screen.
//
// Runtime state only, kept in the (canvas x view) cell (UICanvasContext.hpp): navigating in the editor must
// not rewrite the authored scene.

#include <UI/Args/UIEasing.hpp>
#include <UI/UITree.hpp>

#include <string>
#include <vector>

namespace Desert::UI
{
    struct UICanvasContext;
    struct UIViewContext;

    // One owner's machine: its current screen, the one handing over, and the history BackScreen pops.
    struct UIScreenMachine
    {
        std::string              Screen;          // current screen name
        std::string              From;            // the one handing over, while a transition runs
        std::vector<std::string> History;         // what BackScreen returns to
        float                    T       = 1.0f;  // 0..1 progress of the transition (1 = idle)
        float                    SlidePx = 60.0f; // mirrored from the owner's UIScreenStack
        float                    Time    = 0.25f; // mirrored from the owner's UIScreenStack
        UIEasing                 Easing  = UIEasing::CubicOut;
        bool                     Back    = false; // a Back transition slides the other way
    };

    // A named screen of this canvas and the machine it belongs to — rebuilt once per frame by
    // UpdateScreenMachines, before the walk, so a click during the walk resolves against this frame's tree.
    struct UIScreenEntry
    {
        NodeId      Screen = NodeId::Null;
        NodeId      Owner  = NodeId::Null;
        std::string Name;
    };

    // A switch asked for during the walk, applied when the walk ends (so the frame that clicked draws one
    // consistent machine state).
    struct UIScreenRequest
    {
        NodeId      Owner = NodeId::Null;
        std::string Name; // ignored for Back
        bool        Back        = false;
        bool        PushHistory = true; // false for a tab switch (UITabsData::PushHistory)
    };

    // How a screen node stands in its machine this frame.
    struct UIScreenPresence
    {
        bool                   Current = false;
        bool                   Leaving = false; // the outgoing half of a running hand-over
        const UIScreenMachine* Machine = nullptr;
    };

    // The machine @p screen belongs to: the nearest STRICT ancestor carrying UIScreenStackData, else its
    // canvas. NodeId::Null for a screen under no canvas.
    [[nodiscard]] NodeId ScreenOwnerOf( const IUITree& tree, NodeId screen );

    // Current / leaving in its own machine. Both false when the machine has not been seeded yet, which is
    // what an unseeded machine showed before too (no screen is current).
    [[nodiscard]] UIScreenPresence ScreenPresenceOf( const UICanvasContext& cell, const IUITree& tree,
                                                     NodeId screen );

    // Once per canvas per frame, before the walk: index the canvas's screens, seed every machine (the
    // stack's Initial Screen, else its first screen in draw order), drop machines that own nothing any
    // more, and advance running transitions by @p dt.
    void UpdateScreenMachines( UICanvasContext& cell, const IUITree& tree, NodeId canvas, float dt );

    // A ShowScreen action from @p from: the machine owning a screen named @p name that is NEAREST to the
    // button — the first ancestor of @p from whose sub-tree holds that machine. NodeId::Null when the canvas
    // has no screen of that name.
    [[nodiscard]] NodeId ShowScreenTarget( const UICanvasContext& cell, const IUITree& tree, NodeId from,
                                           const std::string& name );

    // The tab bar @p button belongs to (nearest ancestor-or-self with UITabsData, stopping at the canvas),
    // or NodeId::Null.
    [[nodiscard]] NodeId TabsOf( const IUITree& tree, NodeId button );

    // A BackScreen action from @p from: its nearest owner with history, else the deepest active one.
    [[nodiscard]] NodeId BackScreenTarget( const UICanvasContext& cell, const IUITree& tree, NodeId from );

    // Escape with no overlay open: the topmost canvas's deepest active machine with history goes back.
    // Returns true when some machine took it.
    bool BackScreenOnEscape( UIViewContext& view, const IUITree& tree );

    // Applies (and clears) the requests the walk queued.
    void ApplyScreenRequests( UICanvasContext& cell );
} // namespace Desert::UI
