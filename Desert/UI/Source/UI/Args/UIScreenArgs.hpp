#pragma once

#include <CoreReflection/ReflectionMacros.hpp>
#include <UI/Args/ArgKind.hpp>
#include <UI/Args/UIEasing.hpp>

#include <string>

// Pages of a canvas and the stack that switches between them.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // A screen (page) of a canvas: everything under this element is shown only while it is the current
    // screen. Sibling screens are the states of a small machine — a button with Action = ShowScreen moves
    // between them and BackScreen returns, so a menu with pages needs no scripting.
    struct UIScreenData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Screen;

        PROPERTY( DisplayName( "Screen Name" ), Category( "UI Screen" ) )
        std::string Name; // referenced by a ShowScreen button; empty = never selectable
    };

    // How screens hand over, and WHERE a switcher is: the screens below this element (down to the next
    // element carrying its own stack) are this stack's states — UE's UWidgetSwitcher. On the canvas it is
    // the page machine of the whole menu; on a child it is a nested switcher (the pages of a Settings
    // screen), with its own current screen and back history. The current screen and the history are
    // RUNTIME state kept outside the component (UIScreenMachine.hpp), so navigating in the editor never
    // rewrites the authored scene.
    struct UIScreenStackData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::ScreenStack;

        PROPERTY( DisplayName( "Initial Screen" ), Category( "UI Screens" ) )
        std::string InitialScreen; // empty = the first UIScreen found

        PROPERTY( DisplayName( "Transition" ), Category( "UI Screens" ), Range( 0.0f, 3.0f ) )
        float TransitionTime = 0.25f; // 0 = cut

        PROPERTY( DisplayName( "Slide (px)" ), Category( "UI Screens" ), Range( -1200.0f, 1200.0f ) )
        float SlidePx = 60.0f; // the incoming screen slides in from this far right; out goes the other way

        PROPERTY( DisplayName( "Easing" ), Category( "UI Screens" ) )
        UIEasing Easing = UIEasing::CubicOut;
    };

    // A tab bar — UE's UCommonTabListWidgetBase. Every ShowScreen button below this element is a TAB: it
    // draws Selected exactly while the screen it names is current in the switcher it drives (the nearest
    // one to the button), so the author never keeps Selected in sync by hand.
    struct UITabsData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Tabs;

        // A switcher is not a stack: by default a tab switch replaces the current page without growing the
        // back history, so Back on a tabbed page returns to the page BEFORE it rather than to the last tab.
        PROPERTY( DisplayName( "Tabs Push History" ), Category( "UI Tabs" ) )
        bool PushHistory = false;
    };
} // namespace Desert::UI
