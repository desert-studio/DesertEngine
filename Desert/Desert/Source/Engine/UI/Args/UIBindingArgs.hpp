#pragma once

#include <Engine/Reflection/ReflectionMacros.hpp>
#include <Engine/UI/Args/ArgKind.hpp>

#include <string>

// A data binding from a named value onto an element property.
// Framework data (Desert::UI): the ECS wraps each *Data in a UI*Component (ECS/Components.hpp); the
// reflected type name is the short one, so the scene format does not see the namespace.

namespace Desert::UI
{
    // What a binding drives on its element.
    enum class UIBindTarget
    {
        Text,    // replaces UIText's string (Format applies)
        Value,   // Slider / ProgressBar value
        Opacity, // multiplies the element (and its children) down
        Color,   // multiplies the element's colour
        Visible  // false hides the element and everything under it
    };

    // MVVM-lite: ties this element to a key in the UI data store, which gameplay (C++ or Lua via
    // ui.set) writes. Nothing is written back into the component — the bound value is applied on the way
    // to the screen — so a binding can never overwrite what the author typed.
    //
    // THERE USED TO BE A `Format` FIELD HERE and it was deleted by Ю15, not deprecated. It held a printf
    // format the author typed in the Details panel and handed straight to `std::snprintf` with a double:
    // typing `%s` in an editor field was undefined behaviour at run time, and every number it printed was
    // formatted in the C locale on every screen in every language. Its job moved into the string table,
    // where a translator can put `{n}` (or `{n:2}`) wherever their language wants it and the number is
    // formatted for the reader's locale. The scene migration to version 19 drops the dead key by name.
    struct UIBindingData
    {
        REFLECT()

        static constexpr ArgKind Kind = ArgKind::Binding;

        // NOTE: the header tool reads a tooltip up to the first quote, so keep literals out of them.
        PROPERTY( DisplayName( "Key" ), Category( "UI Binding" ),
                  Tooltip( "Data-store key, e.g. player.hp — write it from Lua with ui.set( key, value )" ) )
        std::string Key;

        PROPERTY( DisplayName( "Target" ), Category( "UI Binding" ) )
        UIBindTarget Target = UIBindTarget::Text;
    };
} // namespace Desert::UI
