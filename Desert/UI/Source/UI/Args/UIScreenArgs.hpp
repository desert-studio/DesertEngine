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

    // How screens hand over. Lives on the canvas; the current screen and the back-stack are RUNTIME state
    // kept outside the component, so navigating in the editor never rewrites the authored scene.
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
} // namespace Desert::UI
