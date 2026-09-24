#pragma once

// WHEN THE SPLASH MAY HAND OVER TO THE EDITOR WINDOW, as one pure function, so the order a person sees —
// content settled, then the window, then the splash fades — is a rule a test can hold rather than a
// consequence of which early return in OnImGuiRender happens to fire first.
//
// The editor used to reveal on "a real frame was drawn" alone. That flag is only set once the loading
// frames stop returning early, so today it does imply a settled scene — but through a draw path three
// thousand lines away from the reveal, and any frame that sets it for another reason (a second scene
// load queued after the first settled, a load that fails and leaves the gate Ready) would put an editor
// over an unsettled scene on screen. Every condition is asked here, directly.

namespace Desert::Editor::Splash
{
    struct RevealState
    {
        bool HasSplash        = false; // no splash (headless shots): nothing to hand over from
        bool Revealed         = false; // the hand-over happens once
        bool StartupLoading   = false; // a startup stage is still to run
        bool SceneLoadPending = false; // a scene load is queued and has not started its settle wait
        bool ContentSettling  = false; // the loaded scene's content is still arriving
        bool RealFrameDrawn   = false; // a frame of the editor itself (not a loading frame) was presented
    };

    [[nodiscard]] constexpr bool MayReveal( const RevealState& s )
    {
        return s.HasSplash && !s.Revealed && !s.StartupLoading && !s.SceneLoadPending && !s.ContentSettling &&
               s.RealFrameDrawn;
    }

    /// Background work that is NOT the scene's content — asset thumbnails — waits until the splash has
    /// handed over. Before that it would compete with the settle for the same frames and the same asset
    /// loader, and a preview nobody can see yet would hold the splash on screen.
    [[nodiscard]] constexpr bool BackgroundWorkAllowed( const RevealState& s )
    {
        return !s.HasSplash || s.Revealed;
    }
} // namespace Desert::Editor::Splash
