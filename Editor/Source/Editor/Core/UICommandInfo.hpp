#pragma once

#include <string_view>

namespace Desert::Editor
{
    /**
     * @brief UE's FUICommandInfo: the identity of ONE editor action — the context it belongs to and the words
     *        a person reads. Every surface that offers the action (a context menu, a toolbar button, the
     *        command palette, the control channel's `run`) takes its words from here and calls the ONE
     *        executor bound to it, so there is no action a mouse reaches and a script cannot.
     *
     *        The palette / control-channel address of a command is {Context, Label} (CommandAddress), which
     *        is why both are stable names and not decoration.
     */
    struct UICommandInfo
    {
        std::string_view Context;
        std::string_view Label;
        std::string_view Shortcut; // shown beside a menu item or in a tooltip; empty when the action has none
    };
} // namespace Desert::Editor
