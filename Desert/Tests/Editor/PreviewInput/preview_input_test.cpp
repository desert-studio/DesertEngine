// What a preview does with the mouse (Editor/Widgets/PreviewInput.hpp). Static (the Details skybox ball): the
// camera never moves and double-click opens the asset. Interactive (asset windows and the Details Static Mesh
// row): drag orbits/pans/moves the sun, the wheel zooms, double-click re-frames.
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

// ── Which Details preview gets which mode ───────────────────────────────────────────────────────────────

// The owner's call (AV1d2): a model in Details orbits and zooms — its asset field already has an Open button,
// so a double-click that opened it bought nothing and cost the row its point.
TEST( PreviewInput, DetailsStaticMeshPreviewIsInteractive )
{
    EXPECT_EQ( DetailsPreviewInteraction( DetailsPreviewKind::StaticMesh ), PreviewInteraction::Interactive );

    // And therefore, through the mode, the gestures: a drag orbits, the wheel zooms, a double-click re-frames
    // and opens nothing.
    const PreviewInteraction mode = DetailsPreviewInteraction( DetailsPreviewKind::StaticMesh );
    EXPECT_NE( PreviewInput( mode, Drag( 40.0f, -12.0f ) ).OrbitDelta.x, 0.0f );
    PreviewInputEvents wheel;
    wheel.Hovered = true;
    wheel.Wheel   = 1.0f;
    EXPECT_EQ( PreviewInput( mode, wheel ).Wheel, 1.0f );
    const PreviewInputResult dbl = PreviewInput( mode, DoubleClick() );
    EXPECT_TRUE( dbl.Reframe );
    EXPECT_FALSE( dbl.Open );
}

// The skybox ball keeps the one angle its Rotation slider is read against.
TEST( PreviewInput, DetailsSkyboxPreviewStaysStatic )
{
    EXPECT_EQ( DetailsPreviewInteraction( DetailsPreviewKind::Skybox ), PreviewInteraction::Static );
}

// ── Who owns the wheel ──────────────────────────────────────────────────────────────────────────────────

// The preview claims the wheel exactly when the wheel zooms it; otherwise the panel keeps scrolling.
TEST( PreviewInput, WheelBelongsToThePreviewOnlyWhenItZooms )
{
    EXPECT_TRUE( PreviewOwnsWheel( PreviewInteraction::Interactive, /*dome=*/false ) );
    EXPECT_FALSE( PreviewOwnsWheel( PreviewInteraction::Interactive, /*dome=*/true ) );
    EXPECT_FALSE( PreviewOwnsWheel( PreviewInteraction::Static, /*dome=*/false ) );
    EXPECT_FALSE( PreviewOwnsWheel( PreviewInteraction::Static, /*dome=*/true ) );

    // The claim and the zoom are one rule: wherever the preview owns the wheel, a notch zooms it, and
    // wherever it does not, a notch zooms nothing.
    for ( const PreviewInteraction mode : { PreviewInteraction::Interactive, PreviewInteraction::Static } )
        for ( const bool dome : { false, true } )
        {
            PreviewInputEvents events;
            events.Hovered = true;
            events.Dome    = dome;
            events.Wheel   = 1.0f;
            EXPECT_EQ( PreviewOwnsWheel( mode, dome ), PreviewInput( mode, events ).Wheel != 0.0f );
        }
}

// ── The Details route ───────────────────────────────────────────────────────────────────────────────────
//
// Every Details preview goes through ComponentEditContext::DrawPreview (Skybox and Static Mesh rows), so the
// mode is decided in exactly one place — from the row's kind — and a Static row's Open reaches the asset-field
// queue EditorLayer answers with RequestOpenAsset. Pin that the route asks the kind rather than hard-coding a
// mode, that each row names its own kind, and that the widget claims the wheel on the preview item.
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

TEST( PreviewInput, DetailsPreviewModeComesFromTheRowKind )
{
    const std::string source =
         ReadRepoFile( "Editor/Source/Editor/Panels/PropertyEditor/ComponentWidgetRegistry.cpp" );
    ASSERT_FALSE( source.empty() ) << "ComponentWidgetRegistry.cpp not found from the test's working directory";

    const auto body = source.find( "ComponentEditContext::DrawPreview" );
    ASSERT_NE( body, std::string::npos );
    const std::string drawPreview = source.substr( body, 1200 );
    EXPECT_NE( drawPreview.find( "DetailsPreviewInteraction( kind )" ), std::string::npos );
    EXPECT_EQ( drawPreview.find( "PreviewInteraction::Static" ), std::string::npos );
    EXPECT_EQ( drawPreview.find( "PreviewInteraction::Interactive" ), std::string::npos );
    EXPECT_NE( drawPreview.find( "AssetFieldRequests::Request" ), std::string::npos );
    EXPECT_NE( drawPreview.find( "AssetFieldAction::Open" ), std::string::npos );
}

TEST( PreviewInput, EachDetailsRowNamesItsOwnKind )
{
    const std::string mesh =
         ReadRepoFile( "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/StaticMeshComponent.cpp" );
    const std::string sky =
         ReadRepoFile( "Editor/Source/Editor/Panels/SceneProperties/ComponentWidgets/SkyboxComponent.cpp" );
    ASSERT_FALSE( mesh.empty() );
    ASSERT_FALSE( sky.empty() );
    EXPECT_NE( mesh.find( "DetailsPreviewKind::StaticMesh" ), std::string::npos );
    EXPECT_EQ( mesh.find( "DetailsPreviewKind::Skybox" ), std::string::npos );
    EXPECT_NE( sky.find( "DetailsPreviewKind::Skybox" ), std::string::npos );
    EXPECT_EQ( sky.find( "DetailsPreviewKind::StaticMesh" ), std::string::npos );
}

TEST( PreviewInput, PreviewWidgetClaimsTheWheelThroughTheRule )
{
    const std::string source = ReadRepoFile( "Editor/Source/Editor/Widgets/PreviewViewport.cpp" );
    ASSERT_FALSE( source.empty() );
    const auto claim = source.find( "ImGui::SetItemUsingMouseWheel()" );
    ASSERT_NE( claim, std::string::npos )
         << "the preview no longer claims the wheel: Details scrolls while it zooms";
    // The claim is gated by the rule, not unconditional — a Static row must leave the wheel to the panel.
    const auto rule = source.rfind( "PreviewOwnsWheel(", claim );
    ASSERT_NE( rule, std::string::npos );
    EXPECT_LT( claim - rule, 200u );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
