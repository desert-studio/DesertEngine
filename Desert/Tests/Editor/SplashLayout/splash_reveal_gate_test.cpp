// THE ORDER A PERSON SEES AT STARTUP: the scene's content settles, THEN the editor window is shown, THEN
// the splash fades. Editor/Splash/RevealGate.hpp is the one place that order is decided.

#include <Editor/Splash/RevealGate.hpp>

#include <gtest/gtest.h>

namespace Splash = Desert::Editor::Splash;

namespace
{
    // Every condition met: the moment the hand-over is allowed.
    Splash::RevealState Ready()
    {
        Splash::RevealState s;
        s.HasSplash      = true;
        s.RealFrameDrawn = true;
        return s;
    }
} // namespace

TEST( SplashRevealGate, RevealsOnceTheContentHasSettledAndARealFrameWasPresented )
{
    EXPECT_TRUE( Splash::MayReveal( Ready() ) );
}

// The rule the task is about: a real frame drawn while the content is still arriving does not show the
// window. The flag used to be the only thing asked.
TEST( SplashRevealGate, ARealFrameOverUnsettledContentDoesNotReveal )
{
    auto s            = Ready();
    s.ContentSettling = true;
    EXPECT_FALSE( Splash::MayReveal( s ) );
}

TEST( SplashRevealGate, AQueuedSceneLoadDoesNotReveal )
{
    auto s             = Ready();
    s.SceneLoadPending = true;
    EXPECT_FALSE( Splash::MayReveal( s ) );
}

TEST( SplashRevealGate, AStartupStageStillToRunDoesNotReveal )
{
    auto s           = Ready();
    s.StartupLoading = true;
    EXPECT_FALSE( Splash::MayReveal( s ) );
}

TEST( SplashRevealGate, SettledContentWithoutARealFrameDoesNotReveal )
{
    auto s           = Ready();
    s.RealFrameDrawn = false;
    EXPECT_FALSE( Splash::MayReveal( s ) );
}

TEST( SplashRevealGate, TheHandOverHappensOnceAndOnlyFromASplash )
{
    auto revealed     = Ready();
    revealed.Revealed = true;
    EXPECT_FALSE( Splash::MayReveal( revealed ) );

    auto headless      = Ready();
    headless.HasSplash = false;
    EXPECT_FALSE( Splash::MayReveal( headless ) );
}

// Thumbnails come in two halves with two gates (TH2). Decoding a PNG already in the disk cache is allowed in
// every state — during the splash is the point, so the first frame after the hand-over only uploads.
// CAPTURING one waits for the hand-over, and is never held by a splash that does not exist (headless shots).
TEST( SplashRevealGate, ACachedThumbnailIsDecodedBeforeTheHandOverAndACaptureIsNot )
{
    Splash::RevealState s;
    s.HasSplash = true;
    EXPECT_TRUE( Splash::ThumbnailDiskDecodeAllowed( s ) ) << "a cached PNG must be decoded during the splash";
    EXPECT_FALSE( Splash::ThumbnailCaptureAllowed( s ) ) << "a capture must not share the settle's frames";
    s.ContentSettling = true;
    EXPECT_TRUE( Splash::ThumbnailDiskDecodeAllowed( s ) );
    EXPECT_FALSE( Splash::ThumbnailCaptureAllowed( s ) );

    s.ContentSettling = false;
    s.Revealed        = true;
    EXPECT_TRUE( Splash::ThumbnailDiskDecodeAllowed( s ) );
    EXPECT_TRUE( Splash::ThumbnailCaptureAllowed( s ) );

    Splash::RevealState headless;
    EXPECT_TRUE( Splash::ThumbnailDiskDecodeAllowed( headless ) );
    EXPECT_TRUE( Splash::ThumbnailCaptureAllowed( headless ) );
}

// THUMB2: the opening folder's CACHED thumbnails hold the hand-over, but only within a budget, and never for a
// capture — capture stays behind the hand-over whatever the upload pass is doing.
TEST( SplashRevealGate, TheOpeningFolderThumbnailsHoldTheHandOverWithinABudget )
{
    Splash::RevealState s;
    s.HasSplash           = true;
    s.RealFrameDrawn      = true;
    s.ThumbnailsUploading = true;
    EXPECT_FALSE( Splash::MayReveal( s ) ) << "the window was shown before its folder's cached pictures were up";
    EXPECT_FALSE( Splash::ThumbnailCaptureAllowed( s ) ) << "the upload pass opened the capture gate";
    s.ThumbnailsUploading = false;
    EXPECT_TRUE( Splash::MayReveal( s ) );

    EXPECT_TRUE( Splash::ThumbnailsHoldReveal( 3, 0.0 ) );
    EXPECT_FALSE( Splash::ThumbnailsHoldReveal( 3, Splash::kThumbnailUploadBudgetMs ) )
         << "a slow disk must not hold the splash past the budget";
    EXPECT_FALSE( Splash::ThumbnailsHoldReveal( 0, 0.0 ) ) << "nothing pending must not hold the splash";
}

// THUMB3: the splash may capture the open scene's materials, once the start-up stages are done and the
// scene is loaded: a capture needs the renderer, and the warm list needs the scene's roots.
TEST( SplashRevealGate, TheScenesCapturesMayRunOnTheSplashOnceTheSceneIsLoaded )
{
    Splash::RevealState s = Ready();
    EXPECT_TRUE( Splash::SceneThumbnailCaptureAllowed( s ) );
    EXPECT_FALSE( Splash::ThumbnailCaptureAllowed( s ) ) << "the folder's captures still wait for the reveal";

    s.StartupLoading = true;
    EXPECT_FALSE( Splash::SceneThumbnailCaptureAllowed( s ) ) << "no renderer before the stages are done";
    s.StartupLoading   = false;
    s.SceneLoadPending = true;
    EXPECT_FALSE( Splash::SceneThumbnailCaptureAllowed( s ) ) << "no scene roots before the scene is loaded";
    s.SceneLoadPending = false;
    s.Revealed         = true;
    EXPECT_FALSE( Splash::SceneThumbnailCaptureAllowed( s ) ) << "after the reveal the whole queue runs instead";
}

TEST( SplashRevealGate, TheScenesCapturesHoldTheHandOverOnlyWithinTheirBudget )
{
    EXPECT_TRUE( Splash::SceneCapturesHoldReveal( 3, 0.0 ) );
    EXPECT_TRUE( Splash::SceneCapturesHoldReveal( 3, Splash::kSceneCaptureBudgetMs - 1.0 ) );
    EXPECT_FALSE( Splash::SceneCapturesHoldReveal( 3, Splash::kSceneCaptureBudgetMs ) );
    EXPECT_FALSE( Splash::SceneCapturesHoldReveal( 0, 0.0 ) );
    // The owner's bound: the splash may grow by a couple of seconds, not more.
    EXPECT_LE( Splash::kSceneCaptureBudgetMs, 2000.0 );
}
