#pragma once

#include <string>
#include <vector>

namespace Desert::UI
{
    // A notification raised from OUTSIDE the UI — gameplay, a Lua script, a tool. Kept here rather than
    // pushed straight into a view because the raiser has no view: it knows the overlay's authored name and
    // a line of text, and nothing else. One per world (UIWorld).
    //
    // WHICH VIEW OF THE WORLD SHOWS IT: the one with UIViewContext::DrivesSceneAnimation, which is the flag that
    // already means "this view is the one that owns the scene's shared clocks" (the viewport / the game, never the
    // authoring preview). Draining it into every view instead would show one notification N times in the
    // editor, and draining it into whichever view happens to run first would make it depend on panel order.
    class UIOverlayRequests
    {
    public:
        struct Request
        {
            std::string Overlay; // UIOverlayData::Name
            std::string Text;
        };

        void Raise( std::string overlay, std::string text );

        // Takes everything queued and clears it — a notification is delivered exactly once.
        [[nodiscard]] std::vector<Request> Drain();

        void Clear();

    private:
        std::vector<Request> m_Pending;
    };
} // namespace Desert::UI
