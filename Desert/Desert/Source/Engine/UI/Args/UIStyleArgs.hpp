#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <string>

// Which style of the canvas theme an element resolves through.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // ------------------------------------------------------------------------------------------------
    // WHERE AN ELEMENT'S COLOURS, FONTS AND SPACINGS COME FROM (Ю13)
    //
    // The canvas names a theme; this component names which STYLE of that theme this element resolves
    // through. An element with no UIStyleComponent uses the theme's "Default" style — theming must not
    // require an edit of every entity, which is the thing themes exist to avoid.
    //
    // THE PRECEDENCE IS NOT A RULE, IT IS A TABLE. A style binds some slots and not others; a bound slot
    // comes from the theme, an unbound one from the element's own authored field. There is never a moment
    // when both apply, so nothing can quietly beat anything — and the Details "UI Style" block prints the
    // source of every slot, so an author can see which of the two a colour came from without guessing.
    // ------------------------------------------------------------------------------------------------

    // Does this element resolve through the canvas's theme at all?
    //
    // WHY AN ENUM AND NOT A RESERVED STYLE NAME like "None": a magic string is exactly the silent
    // convention this decision is trying to avoid, and it cannot be seen in the Details panel. Both
    // values are read by UICanvasRenderer2D.cpp.
    enum class UIStyleSource
    {
        Theme, // resolve bound slots from the canvas's theme; unbound ones stay local
        Local  // ignore the theme entirely — every slot is this element's own authored field
    };

    struct UIStyleData
    {
        REFLECT()

        static constexpr ArgKind Arg = ArgKind::Style;

        PROPERTY( DisplayName( "Source" ), Category( "UI Style" ),
                  Tooltip( "Theme: bound slots come from the canvas's theme, the rest from this element. "
                           "Local: every slot is this element's own value." ) )
        UIStyleSource Source = UIStyleSource::Theme;

        // The style's name inside the theme. A name the theme does not declare is REPORTED with the
        // element's tag and this name (once per canvas), and the element falls back to fully local — the
        // values its author actually typed — rather than to an invented default or to nothing drawn.
        PROPERTY( DisplayName( "Style" ), Category( "UI Style" ),
                  Tooltip( "A style declared by the canvas's theme, e.g. \"Default\" or \"Primary\"." ) )
        std::string Style = "Default";
    };
} // namespace Desert::UI
