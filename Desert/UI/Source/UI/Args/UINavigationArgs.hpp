#pragma once

#include <CoreReflection/ReflectionMacros.hpp>
#include <UI/Args/ArgKind.hpp>

#include <string>

// Keyboard / gamepad navigation metadata (Slate's FNavigationMetaData, UMG's UWidgetNavigation).
// Framework data (Desert::UI): the ECS wraps it in UINavigationComponent (ECS/Components.hpp).

namespace Desert::UI
{
    // What a navigation does when nothing is left in its direction inside the boundary (EUINavigationRule).
    enum class UINavigationRule
    {
        Escape,  // leave this element's box and let an outer one decide; with none, focus stays
        Stop,    // focus stays
        Wrap,    // continue from the opposite side of this element's box
        Explicit // go to the element named by the direction's target, wherever it is
    };

    // Put this on a control or on a container. A focused control's request bubbles up from the control to
    // the FIRST ancestor (itself included) whose rule for that direction is not Escape; that element's box
    // is the boundary the spatial search wraps or stops inside (FHittestGrid::FindNextFocusableWidget,
    // HittestGrid.cpp:516). With none, the view is the boundary and the rule is Escape.
    struct UINavigationData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Navigation;

        PROPERTY( DisplayName( "Up" ), Category( "UI Navigation" ) )
        UINavigationRule Up = UINavigationRule::Escape;

        PROPERTY( DisplayName( "Up Target" ), Category( "UI Navigation" ),
                  Tooltip( "Name of the element Up goes to when the rule is Explicit" ) )
        std::string UpTarget;

        PROPERTY( DisplayName( "Down" ), Category( "UI Navigation" ) )
        UINavigationRule Down = UINavigationRule::Escape;

        PROPERTY( DisplayName( "Down Target" ), Category( "UI Navigation" ),
                  Tooltip( "Name of the element Down goes to when the rule is Explicit" ) )
        std::string DownTarget;

        PROPERTY( DisplayName( "Left" ), Category( "UI Navigation" ) )
        UINavigationRule Left = UINavigationRule::Escape;

        PROPERTY( DisplayName( "Left Target" ), Category( "UI Navigation" ),
                  Tooltip( "Name of the element Left goes to when the rule is Explicit" ) )
        std::string LeftTarget;

        PROPERTY( DisplayName( "Right" ), Category( "UI Navigation" ) )
        UINavigationRule Right = UINavigationRule::Escape;

        PROPERTY( DisplayName( "Right Target" ), Category( "UI Navigation" ),
                  Tooltip( "Name of the element Right goes to when the rule is Explicit" ) )
        std::string RightTarget;

        // UCommonActivatableWidget::GetDesiredFocusTarget (CommonActivatableWidget.h:78): when the screen,
        // overlay or canvas holding this control appears and has no focus of its own to restore, focus
        // lands here. Nothing marked — nothing is focused until a key asks (a menu does not pre-select).
        PROPERTY( DisplayName( "Initial Focus" ), Category( "UI Navigation" ),
                  Tooltip( "Takes focus when its screen, overlay or canvas appears with nothing to restore" ) )
        bool InitialFocus = false;
    };
} // namespace Desert::UI
