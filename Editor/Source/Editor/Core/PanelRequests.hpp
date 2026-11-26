#pragma once

#include <string>
#include <unordered_map>

namespace Desert::Editor::Core
{
    // "Show that panel" / "put it away" as an EXPLICIT request, from anywhere, by panel name.
    //
    // Tool panels used to open themselves off the selection (IPanel::IsContextual): click an animated
    // character and the Anim Graph, the Layers panel and the Sequencer all threw themselves over the
    // editor. Selecting an object is not a request to author its animation — so those panels are opened
    // by a BUTTON now (Details ▸ Animation ▸ "Anim Graph", the toolbar's Browse, the status bar's
    // drawers), and this is the wire between the button and the panel: the requester does not need the
    // panel's type, its instance, or its header.
    //
    // Open vs Toggle: a jump INTO a tool ("open this in the Sequencer") always shows it, because the user
    // is going there. A drawer button is a switch — pressing Browse again puts the browser away, which is
    // what a button that is always on screen has to do.
    //
    // The editor's panel loop consumes each request once per frame.
    class PanelRequests
    {
    public:
        enum class Action
        {
            None,
            Open,
            Toggle
        };

        // Name as registered with the panel (IPanel::GetName()).
        static void Open( const std::string& panelName )
        {
            Pending()[panelName] = Action::Open;
        }

        static void Toggle( const std::string& panelName )
        {
            // An explicit Open already queued this frame wins: it says where the user is GOING.
            auto& pending = Pending();
            if ( pending.find( panelName ) == pending.end() )
                pending[panelName] = Action::Toggle;
        }

        // The request for a name, if any; clears it.
        static Action Consume( const std::string& panelName )
        {
            auto&      pending = Pending();
            const auto it      = pending.find( panelName );
            if ( it == pending.end() )
                return Action::None;

            const Action action = it->second;
            pending.erase( it );
            return action;
        }

    private:
        static std::unordered_map<std::string, Action>& Pending();
    };
} // namespace Desert::Editor::Core
