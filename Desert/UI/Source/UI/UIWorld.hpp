#pragma once

#include <UI/UIDataStore.hpp>
#include <UI/UIOverlayRequests.hpp>

namespace Desert::UI
{
    // What gameplay tells ONE world's screen, and what that screen tells its gameplay.
    struct UIWorld
    {
        UIDataStore       Data;
        UIMessageQueue    Messages;
        UIOverlayRequests Overlays;

        // Scene::Clear: nothing bound to the old level is shown by the new one.
        void Reset()
        {
            Data.Clear();
            Messages.Clear();
            Overlays.Clear();
        }
    };
} // namespace Desert::UI
