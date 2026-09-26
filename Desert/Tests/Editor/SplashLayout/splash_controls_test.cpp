// The splash's window buttons and its close: where the buttons are, when a press is a click, and that a
// close asked for between stages stops every stage after it (Editor/Splash/SplashControls.hpp).

#include <Editor/Splash/SplashControls.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace Desert::Editor::Splash;

TEST( SplashControls, ButtonsSitInTheTopRightCornerTouchingAndClearOfEveryLine )
{
    const Rect close    = ButtonRect( SplashButton::Close );
    const Rect minimize = ButtonRect( SplashButton::Minimize );
    EXPECT_FLOAT_EQ( close.X + close.W, kWidth );
    EXPECT_FLOAT_EQ( close.Y + close.H, kHeight );
    EXPECT_FLOAT_EQ( minimize.X + minimize.W, close.X ); // touching, as the editor frame's buttons do
    EXPECT_FLOAT_EQ( close.W, Desert::Editor::UI::kWindowButtonWidth );

    // The live text is at the bottom; nothing of it may sit under a button.
    const Layout layout = ComputeLayout( 1.0 );
    for ( const Rect& line : { layout.Project, layout.Stage, layout.Percent, layout.Item, layout.Version } )
        EXPECT_LT( line.Y + line.H, minimize.Y );
}

TEST( SplashControls, HitTestNamesTheButtonUnderThePointAndNothingElse )
{
    const Rect close    = ButtonRect( SplashButton::Close );
    const Rect minimize = ButtonRect( SplashButton::Minimize );
    EXPECT_EQ( HitTestButtons( close.X + 1.0f, close.Y + 1.0f ), SplashButton::Close );
    EXPECT_EQ( HitTestButtons( minimize.X + minimize.W - 1.0f, minimize.Y + 1.0f ), SplashButton::Minimize );
    EXPECT_EQ( HitTestButtons( minimize.X - 1.0f, minimize.Y + 1.0f ), SplashButton::None );
    EXPECT_EQ( HitTestButtons( close.X + 1.0f, close.Y - 1.0f ), SplashButton::None );
    EXPECT_EQ( HitTestButtons( kWidth * 0.5f, kHeight * 0.5f ), SplashButton::None ); // the draggable picture
}

TEST( SplashControls, ScreenPointsMapIntoTheScaledDesign )
{
    // A splash at half size with its top-left at (100, 50): the window's top-right corner maps to the
    // design's top-right, and the close button is found there.
    const float       fit    = 0.5f;
    const DesignPoint corner = ScreenToDesign( 100.0f + kWidth * fit - 1.0f, 50.0f + 1.0f, 100.0f, 50.0f, fit );
    EXPECT_NEAR( corner.X, kWidth - 2.0f, 1e-3f );
    EXPECT_NEAR( corner.Y, kHeight - 2.0f, 1e-3f );
    EXPECT_EQ( HitTestButtons( corner.X, corner.Y ), SplashButton::Close );
}

TEST( SplashControls, APressIsAClickOnlyWhenItStartsAndEndsOnTheSameButton )
{
    ButtonTracker tracker;
    EXPECT_EQ( tracker.Update( SplashButton::Close, false ), SplashButton::None );
    EXPECT_EQ( ButtonFill( SplashButton::Close, tracker ).R, Desert::Editor::UI::kCloseButtonHovered.R );
    EXPECT_EQ( tracker.Update( SplashButton::Close, true ), SplashButton::None );
    EXPECT_EQ( tracker.Pressed(), SplashButton::Close );
    EXPECT_EQ( tracker.Update( SplashButton::Close, false ), SplashButton::Close );

    // Pressed on close, let go over minimize: nothing — the way to change one's mind.
    tracker.Update( SplashButton::Close, true );
    EXPECT_EQ( tracker.Update( SplashButton::Minimize, false ), SplashButton::None );

    // Pressed on the picture (a drag), released over close: not a click either.
    tracker.Update( SplashButton::None, true );
    EXPECT_EQ( tracker.Update( SplashButton::Close, false ), SplashButton::None );

    // A button held down is reported once, on release.
    tracker.Update( SplashButton::Minimize, true );
    EXPECT_EQ( tracker.Update( SplashButton::Minimize, true ), SplashButton::None );
    EXPECT_EQ( tracker.Update( SplashButton::Minimize, false ), SplashButton::Minimize );
    EXPECT_EQ( ButtonFill( SplashButton::Close, tracker ).A, 0.0f ); // at rest: clear
}

// The start as EditorLayer::OnUpdate runs it: one stage per frame, NextStartupStep asked before each.
TEST( SplashControls, CloseAskedBetweenStagesStopsEveryLaterStage )
{
    std::vector<int>                   ran;
    bool                               closeRequested = false;
    bool                               quit           = false;
    std::vector<std::function<void()>> stages;
    stages.reserve( 6 );
    for ( int i = 0; i < 6; ++i )
        stages.emplace_back(
             [&, i]()
             {
                 ran.push_back( i );
                 if ( i == 2 )
                     closeRequested = true; // clicked while stage 2 ran
             } );

    std::size_t next = 0;
    for ( int frame = 0; frame < 20 && !quit; ++frame )
    {
        switch ( NextStartupStep( closeRequested, next, stages.size() ) )
        {
            case StartupStep::Quit:
                quit = true;
                break;
            case StartupStep::RunStage:
                stages[next++]();
                break;
            case StartupStep::Done:
                break;
        }
    }
    EXPECT_TRUE( quit );
    EXPECT_EQ( ran, ( std::vector<int>{ 0, 1, 2 } ) );

    EXPECT_EQ( NextStartupStep( false, 6, 6 ), StartupStep::Done );
    EXPECT_EQ( NextStartupStep( true, 6, 6 ), StartupStep::Quit ); // during the settle, too
    EXPECT_EQ( NextStartupStep( false, 0, 6 ), StartupStep::RunStage );
}

namespace
{
    std::string SplashSource( const std::string& relative )
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up, prefix += "../" )
        {
            std::ifstream in( prefix + "Editor/Source/Editor/Splash/" + relative, std::ios::binary );
            if ( in )
                return { std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
        }
        return {};
    }
} // namespace

// SPL4: the splash comes to the front ONCE as it appears and then stacks like any window. A TOPMOST / floating
// splash stayed above every other window the owner switched to while the editor loaded.
TEST( SplashControls, TheSplashIsNotAlwaysOnTop )
{
    const std::string windows = SplashSource( "Windows/SplashScreenWindows.cpp" );
    const std::string macos   = SplashSource( "MacOS/SplashScreenMacOS.mm" );
    ASSERT_FALSE( windows.empty() );
    ASSERT_FALSE( macos.empty() );
    EXPECT_EQ( windows.find( "WS_EX_TOPMOST" ), std::string::npos );
    EXPECT_EQ( windows.find( "HWND_TOPMOST" ), std::string::npos );
    EXPECT_EQ( macos.find( "NSFloatingWindowLevel" ), std::string::npos );
    EXPECT_NE( macos.find( "orderFrontRegardless" ), std::string::npos ) << "it must still come to the front once";
}
