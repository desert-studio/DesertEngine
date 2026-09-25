// What a preview does with the mouse (Editor/Widgets/PreviewInput.hpp). Details rows are Static: the camera
// never moves and double-click opens the asset. Asset windows are Interactive: drag orbits/pans/moves the
// sun, the wheel zooms, double-click re-frames.
#include <Editor/Widgets/PreviewInput.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

using namespace Desert::Editor;

namespace
{
    PreviewInputEvents Drag( float dx, float dy )
    {
        PreviewInputEvents events;
        events.Hovered    = true;
        events.Active     = true;
        events.MouseDelta = { dx, dy };
        return events;
    }

    PreviewInputEvents DoubleClick()
    {
        PreviewInputEvents events;
        events.Hovered       = true;
        events.Active        = true; // the second click of a double-click presses the preview's button too
        events.DoubleClicked = true;
        return events;
    }

    bool Moved( const glm::vec2& delta )
    {
        return delta.x != 0.0f || delta.y != 0.0f;
    }

    // Nothing that would move the camera or the light.
    void ExpectStill( const PreviewInputResult& result )
    {
        EXPECT_FALSE( Moved( result.OrbitDelta ) );
        EXPECT_FALSE( Moved( result.PanDelta ) );
        EXPECT_FALSE( Moved( result.SunDelta ) );
        EXPECT_EQ( result.Wheel, 0.0f );
        EXPECT_FALSE( result.Reframe );
        EXPECT_FALSE( result.Interacting );
    }
} // namespace

// ── Static: the Details row ─────────────────────────────────────────────────────────────────────────────

TEST( PreviewInput, StaticDragDoesNotOrbit )
{
    const PreviewInputResult result = PreviewInput( PreviewInteraction::Static, Drag( 40.0f, -12.0f ) );
    ExpectStill( result );
    EXPECT_FALSE( result.Open );
}

TEST( PreviewInput, StaticRightDragDoesNotPan )
{
    PreviewInputEvents events = Drag( 40.0f, -12.0f );
    events.RightDown          = true;
    ExpectStill( PreviewInput( PreviewInteraction::Static, events ) );
}

TEST( PreviewInput, StaticLightDragDoesNotMoveTheSun )
{
    PreviewInputEvents events = Drag( 40.0f, -12.0f );
    events.LightKeyDown       = true;
    ExpectStill( PreviewInput( PreviewInteraction::Static, events ) );
}

TEST( PreviewInput, StaticWheelDoesNotZoom )
{
    PreviewInputEvents events;
    events.Hovered = true;
    events.Wheel   = 3.0f;
    ExpectStill( PreviewInput( PreviewInteraction::Static, events ) );
}

TEST( PreviewInput, StaticDoubleClickOpensAndDoesNotReframe )
{
    const PreviewInputResult result = PreviewInput( PreviewInteraction::Static, DoubleClick() );
    EXPECT_TRUE( result.Open );
    ExpectStill( result );
}

TEST( PreviewInput, StaticDoubleClickElsewhereOpensNothing )
{
    // ImGui reports a double-click wherever it happened; only one over the preview is about this asset.
    PreviewInputEvents events = DoubleClick();
    events.Hovered            = false;
    events.Active             = false;
    EXPECT_FALSE( PreviewInput( PreviewInteraction::Static, events ).Open );
}

// ── Interactive: the asset's own window ─────────────────────────────────────────────────────────────────

TEST( PreviewInput, InteractiveDoubleClickReframesAndDoesNotOpen )
{
    const PreviewInputResult result = PreviewInput( PreviewInteraction::Interactive, DoubleClick() );
    EXPECT_TRUE( result.Reframe );
    EXPECT_FALSE( result.Open );
    EXPECT_TRUE( result.Interacting );
}

TEST( PreviewInput, InteractiveDragOrbitsByTheMouseDelta )
{
    const PreviewInputResult result = PreviewInput( PreviewInteraction::Interactive, Drag( 40.0f, -12.0f ) );
    EXPECT_EQ( result.OrbitDelta, glm::vec2( 40.0f, -12.0f ) );
    EXPECT_FALSE( Moved( result.PanDelta ) );
    EXPECT_FALSE( Moved( result.SunDelta ) );
    EXPECT_TRUE( result.Interacting );
}

TEST( PreviewInput, InteractiveRightDragPansInsteadOfOrbiting )
{
    PreviewInputEvents events       = Drag( 5.0f, 7.0f );
    events.RightDown                = true;
    const PreviewInputResult result = PreviewInput( PreviewInteraction::Interactive, events );
    EXPECT_EQ( result.PanDelta, glm::vec2( 5.0f, 7.0f ) );
    EXPECT_FALSE( Moved( result.OrbitDelta ) );
}

TEST( PreviewInput, InteractiveLightDragMovesOnlyTheSun )
{
    PreviewInputEvents events       = Drag( 5.0f, 7.0f );
    events.LightKeyDown             = true;
    events.RightDown                = true; // L wins over either button
    const PreviewInputResult result = PreviewInput( PreviewInteraction::Interactive, events );
    EXPECT_EQ( result.SunDelta, glm::vec2( 5.0f, 7.0f ) );
    EXPECT_FALSE( Moved( result.OrbitDelta ) );
    EXPECT_FALSE( Moved( result.PanDelta ) );
}

TEST( PreviewInput, InteractiveWheelZoomsExceptInTheDome )
{
    PreviewInputEvents events;
    events.Hovered = true;
    events.Wheel   = 2.0f;
    EXPECT_EQ( PreviewInput( PreviewInteraction::Interactive, events ).Wheel, 2.0f );

    events.Dome = true;
    EXPECT_EQ( PreviewInput( PreviewInteraction::Interactive, events ).Wheel, 0.0f );
}

// ── The Details route is the Static one ─────────────────────────────────────────────────────────────────
//
// Every Details preview goes through ComponentEditContext::DrawPreview (Skybox and Static Mesh rows), so the
// mode is decided in exactly one place; pin it there, and pin that its Open reaches the asset-field queue
// EditorLayer answers with RequestOpenAsset.
namespace
{
    std::string ReadRepoFile( const char* relative )
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up, prefix += "../" )
        {
            const std::ifstream in( prefix + relative );
            if ( !in )
                continue;
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }
        return {};
    }
} // namespace

TEST( PreviewInput, DetailsPreviewIsStaticAndOpensThroughTheAssetFieldQueue )
{
    const std::string source =
         ReadRepoFile( "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.cpp" );
    ASSERT_FALSE( source.empty() ) << "ComponentWidgetRegistry.cpp not found from the test's working directory";

    const auto body = source.find( "ComponentEditContext::DrawPreview" );
    ASSERT_NE( body, std::string::npos );
    const std::string drawPreview = source.substr( body, 1200 );
    EXPECT_NE( drawPreview.find( "PreviewInteraction::Static" ), std::string::npos );
    EXPECT_EQ( drawPreview.find( "PreviewInteraction::Interactive" ), std::string::npos );
    EXPECT_NE( drawPreview.find( "AssetFieldRequests::Request" ), std::string::npos );
    EXPECT_NE( drawPreview.find( "AssetFieldAction::Open" ), std::string::npos );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
