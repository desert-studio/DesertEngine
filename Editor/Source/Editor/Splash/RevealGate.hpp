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

#include <cstddef>

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
        bool ThumbnailsUploading = false; // the opening folder's cached thumbnails are still being decoded
    };

    [[nodiscard]] constexpr bool MayReveal( const RevealState& s )
    {
        return s.HasSplash && !s.Revealed && !s.StartupLoading && !s.SceneLoadPending && !s.ContentSettling &&
               s.RealFrameDrawn && !s.ThumbnailsUploading;
    }

    /// Decoding thumbnails that are ALREADY on disk (ThumbnailPrefetch) is allowed in every state, splash
    /// included. It is file reads and stb on a JobSystem worker: no renderer slot, no device, nothing the
    /// settle waits on. Doing it during the splash is the point — the first frame after the hand-over then
    /// only uploads (as UE shows a cached thumbnail at once). A predicate that is always true still earns its
    /// name: the call site says which half of the thumbnail work it is, and the test pins that this half does
    /// not wait.
    [[nodiscard]] constexpr bool ThumbnailDiskDecodeAllowed( const RevealState& /*unused*/ )
    {
        return true;
    }

    /// CAPTURING a thumbnail (a renderer slot, ~370 ms a material) and painting a cloud one wait until the
    /// splash has handed over. Before that they would compete with the settle for the same frames and the
    /// same asset loader, and a preview nobody can see yet would hold the splash on screen.
    [[nodiscard]] constexpr bool ThumbnailCaptureAllowed( const RevealState& s )
    {
        return !s.HasSplash || s.Revealed;
    }

    /// How long the hand-over may wait for the opening folder's CACHED thumbnails once everything else is
    /// ready (THUMB2). The pictures are decoded on workers from the splash's first settle frame and uploaded
    /// as they land, so on a warm disk cache they are done before the settle is; this bound is only for a
    /// cold file cache or a folder of hundreds, where the browser drawing icons for a few frames beats a
    /// splash nobody can explain.
    inline constexpr double kThumbnailUploadBudgetMs = 250.0;

    /// Whether the thumbnails still hold the hand-over. `pending` = pictures of the opening folder still
    /// waiting for or on a worker (a missing or stale picture is not counted: that is a capture, and
    /// ThumbnailCaptureAllowed keeps captures after the hand-over); `msSinceOtherwiseReady` = how long every
    /// other condition of MayReveal has held.
    [[nodiscard]] constexpr bool ThumbnailsHoldReveal( std::size_t pending, double msSinceOtherwiseReady )
    {
        return pending > 0 && msSinceOtherwiseReady < kThumbnailUploadBudgetMs;
    }
} // namespace Desert::Editor::Splash
