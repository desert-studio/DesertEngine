#pragma once

// THE EDITOR'S START-UP SPLASH: a small borderless window of its own, up within the first tenth of a
// second and BEFORE the renderer exists, which says what the start is doing while the main thread is busy
// doing it. The main window stays hidden until the editor is ready and the splash closes on the first
// real frame (EditorLayer), so a person never looks at a full-size window that is blank and not answering.
//
// WHY A NATIVE WINDOW AND NOT THE ENGINE'S OWN DRAWING. The two long waits are both ON the main thread —
// the shader preload in OnAttach (seconds, one call) and the staged boot — and Render2D and ImGui both
// draw from that thread through a device that, for the first of them, does not exist yet. So the splash
// cannot be drawn by anything the start is waiting on. Each platform has a way to put pixels on the
// screen that does not need the main thread's cooperation:
//
//   * macOS: AppKit wants windows created on the main thread, so the window is made there at once, and
//     every later change is a Core Animation layer update committed with an explicit
//     CATransaction + flush from the splash's own thread. WindowServer composites committed layers with
//     no help from the application's run loop — probed before this was written: the main thread blocked
//     for six seconds, the window captured at 1 s and 4.5 s showed two different statuses, and the same
//     probe without the explicit commit captured one frozen status twice.
//   * Windows: a popup window created on the splash's own thread, which runs that window's message loop
//     (the arrangement UE's FWindowsPlatformSplash uses), so it paints however long the main thread is
//     away.
//
// ONE INTERFACE, THREE CALLS, and the callers never know which platform answered. `SetProgress` never
// blocks on the window system: it records the newest snapshot and wakes the splash's thread, which applies
// it. What the snapshot says — stage, item, weighted percentage — is `ProgressModel` (SplashProgress.hpp).

#include <Editor/Splash/SplashProgress.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace Desert::Editor::Splash
{
    struct SplashContent
    {
        // The project being opened — the first line of the live text.
        std::string ProjectName;
        // `Common::Version::Base()`: the engine's version, drawn right above the bar.
        std::string Version;
        // The COOKED splash image (`.tex`, BC7). Read and decoded on the splash's thread, after the
        // window is already up. A missing file is not an error the start stops for: the splash draws its
        // live text on a plain dark background and the log says why, once (see SplashImage.hpp).
        std::filesystem::path CookedImage;
    };

    class SplashScreen
    {
    public:
        virtual ~SplashScreen() = default;

        /// What the start is doing now: the stage, the item inside it and the share done. An empty stage
        /// (the plan is not made yet) draws no percentage.
        virtual void SetProgress( const ProgressSnapshot& progress ) = 0;

        /// Starts taking the window down — a kFadeOutSeconds crossfade into whatever is under it, the
        /// editor's window by then — and RETURNS AT ONCE: the editor is on screen and must not freeze for
        /// the length of a fade. The destructor waits for whatever is left of it. Main thread only
        /// (AppKit). Idempotent.
        virtual void Close() = 0;

        /// The platform's splash, already on screen when this returns. Main thread only.
        [[nodiscard]] static std::unique_ptr<SplashScreen> Show( const SplashContent& content );
    };
} // namespace Desert::Editor::Splash
