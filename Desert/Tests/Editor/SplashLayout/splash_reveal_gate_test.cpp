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

// Thumbnails are background work: held while the splash is up, free once it has handed over, and never
// held by a splash that does not exist (headless shots have none).
TEST( SplashRevealGate, BackgroundWorkWaitsForTheHandOver )
{
    Splash::RevealState s;
    s.HasSplash = true;
    EXPECT_FALSE( Splash::BackgroundWorkAllowed( s ) );
    s.Revealed = true;
    EXPECT_TRUE( Splash::BackgroundWorkAllowed( s ) );

    Splash::RevealState headless;
    EXPECT_TRUE( Splash::BackgroundWorkAllowed( headless ) );
}
