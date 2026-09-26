// What a preview does with the mouse (Editor/Widgets/PreviewInput.hpp). Static (the Details skybox ball): the
// camera never moves and double-click opens the asset. Interactive (asset windows and the Details Static Mesh
// row): drag orbits/pans/moves the sun, the wheel zooms, double-click re-frames.
#include <Editor/Widgets/PreviewInput.hpp>
#include <Editor/Widgets/PreviewPaneLayout.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numbers>
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

// The preview claims the wheel exactly when the wheel zooms it; otherwise the panel keeps scrolling. The
// sky dome and an empty pane never eat the wheel; a Static preview never zooms.
TEST( PreviewInput, WheelBelongsToThePreviewOnlyWhenItZooms )
{
    const auto owns = []( PreviewInteraction mode, bool zoomable )
    { return WheelOwner( mode, zoomable, /*hovered=*/true ) == PreviewWheelOwner::Zoom; };
    EXPECT_TRUE( owns( PreviewInteraction::Interactive, /*zoomable=*/true ) );
    EXPECT_FALSE( owns( PreviewInteraction::Interactive, /*zoomable=*/false ) );
    EXPECT_FALSE( owns( PreviewInteraction::Static, /*zoomable=*/true ) );
    EXPECT_FALSE( owns( PreviewInteraction::Static, /*zoomable=*/false ) );

    // The claim and the zoom are one rule: wherever the preview owns the wheel, a notch zooms it, and
    // wherever it does not, a notch zooms nothing. The dome is the non-zoomable pane PreviewInput can see.
    for ( const PreviewInteraction mode : { PreviewInteraction::Interactive, PreviewInteraction::Static } )
        for ( const bool dome : { false, true } )
        {
            PreviewInputEvents events;
            events.Hovered = true;
            events.Dome    = dome;
            events.Wheel   = 1.0f;
            EXPECT_EQ( owns( mode, !dome ), PreviewInput( mode, events ).Wheel != 0.0f );
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
    const auto rule = source.rfind( "WheelOwner(", claim );
    ASSERT_NE( rule, std::string::npos );
    EXPECT_LT( claim - rule, 200u );
    // ...and the rule is told whether the pane can zoom at all: the dome and an empty pane stay out of it.
    const auto zoomable = source.rfind( "const bool zoomable = m_HasContent && m_Fill != Fill::SkyDome;", rule );
    ASSERT_NE( zoomable, std::string::npos );
    EXPECT_LT( rule - zoomable, 200u );
    EXPECT_EQ( source.find( "PreviewOwnsWheel" ), std::string::npos ) << "a second wheel rule is back";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ── The asset document's preview pane (Editor/Widgets/PreviewPaneLayout.hpp) ─────────────────────────────
//
// The Material Editor's preview is a column that fills its half of the document; these pin the arithmetic
// that decides the two columns and the size the picture renders at.
namespace PaneLayout = Desert::Editor::PreviewPane;

TEST( PreviewPaneLayout, DefaultSplitGivesThePreviewSixtyPercent )
{
    const auto split = PaneLayout::SplitWidth( 1000.0f, PaneLayout::kDefaultSplit );
    EXPECT_FLOAT_EQ( split.Preview, 600.0f );
    EXPECT_FLOAT_EQ( split.Details, 400.0f );
}

TEST( PreviewPaneLayout, NeitherColumnGoesUnderItsMinimumWhileBothFit )
{
    const auto allPreview = PaneLayout::SplitWidth( 1000.0f, 1.0f );
    EXPECT_FLOAT_EQ( allPreview.Details, PaneLayout::kMinDetailsWidth );
    EXPECT_FLOAT_EQ( allPreview.Preview, 1000.0f - PaneLayout::kMinDetailsWidth );

    const auto noPreview = PaneLayout::SplitWidth( 1000.0f, 0.0f );
    EXPECT_FLOAT_EQ( noPreview.Preview, PaneLayout::kMinPreviewWidth );
    EXPECT_FLOAT_EQ( noPreview.Details, 1000.0f - PaneLayout::kMinPreviewWidth );
}

TEST( PreviewPaneLayout, ANarrowWindowSharesItsWidthAndCollapsesNeitherColumn )
{
    // Narrower than the two minimums together: both shrink, in their minimums' proportion, and both stay
    // above zero — at any fraction the person left the divider at.
    const float narrow = 200.0f;
    for ( const float fraction : { 0.0f, 0.6f, 1.0f } )
    {
        const auto split = PaneLayout::SplitWidth( narrow, fraction );
        EXPECT_GT( split.Preview, 0.0f );
        EXPECT_GT( split.Details, 0.0f );
        EXPECT_FLOAT_EQ( split.Preview + split.Details, narrow );
        EXPECT_FLOAT_EQ( split.Preview / split.Details,
                         PaneLayout::kMinPreviewWidth / PaneLayout::kMinDetailsWidth );
    }
    const auto none = PaneLayout::SplitWidth( 0.0f, 0.6f );
    EXPECT_FLOAT_EQ( none.Preview, 0.0f );
    EXPECT_FLOAT_EQ( none.Details, 0.0f );
}

TEST( PreviewPaneLayout, ADragStopsAtTheMinimumInsteadOfStoringTheOvershoot )
{
    // Dragged 2000 px right on a 1000 px document: the divider stands where the details minimum stops it,
    // so a drag back moves it on the first pixel.
    const float pinned = PaneLayout::DragSplit( 1000.0f, 0.6f, 2000.0f );
    EXPECT_NEAR( pinned * 1000.0f, 1000.0f - PaneLayout::kMinDetailsWidth, 1e-3f );
    const float back = PaneLayout::DragSplit( 1000.0f, pinned, -10.0f );
    EXPECT_NEAR( back * 1000.0f, 1000.0f - PaneLayout::kMinDetailsWidth - 10.0f, 1e-3f );

    EXPECT_NEAR( PaneLayout::DragSplit( 1000.0f, 0.6f, 50.0f ), 0.65f, 1e-5f );
}

TEST( PreviewPaneLayout, ThePictureRendersAtThePanesOwnPixels )
{
    // Not a fixed square: a wide pane renders wide, and the framebuffer scale is applied so one texel lands
    // on one screen pixel.
    const auto extent = PaneLayout::RenderExtent( 640.0f, 480.0f, 1.0f );
    EXPECT_EQ( extent.Width, 640u );
    EXPECT_EQ( extent.Height, 480u );

    const auto retina = PaneLayout::RenderExtent( 640.0f, 480.0f, 2.0f );
    EXPECT_EQ( retina.Width, 1280u );
    EXPECT_EQ( retina.Height, 960u );
}

TEST( PreviewPaneLayout, AnUnusablePaneSizeIsTheZeroExtentSoTheResizeIsSkipped )
{
    for ( const auto& [w, h] : { std::pair{ 0.0f, 480.0f }, std::pair{ 640.0f, 0.5f }, std::pair{ -30.0f, 480.0f },
                                 std::pair{ 1e9f, 480.0f } } )
    {
        const auto extent = PaneLayout::RenderExtent( w, h, 1.0f );
        EXPECT_FALSE( Desert::Graphic::IsUsableViewExtent( extent ) ) << w << "x" << h;
    }
}

// ── Framing: the subject fits the pane's NARROWER side, whole, centred, with a margin ───────────────────
//
// Checked INDEPENDENTLY of FitDistance's own arithmetic: the camera is built the way the preview builds it
// (an orbit at the framing yaw/pitch looking at the focus, vertical fov, the pane's aspect) and the subject
// is projected through glm's own perspective. A rule that fitted by the vertical field alone passes the
// square and the wide pane and fails the tall narrow one — the pane the lead's frame showed overflowing.
namespace
{
    constexpr float kPreviewFov = glm::radians( 35.0f ); // PreviewViewport's kFov

    struct Extent2D
    {
        float X = 0.0f; // the largest |NDC x| of the subject's silhouette
        float Y = 0.0f;
    };

    glm::vec3 EyeAt( const float distance )
    {
        const float cp = std::cos( PaneLayout::kFramingPitch );
        return glm::vec3( cp * std::sin( PaneLayout::kFramingYaw ), std::sin( PaneLayout::kFramingPitch ),
                          cp * std::cos( PaneLayout::kFramingYaw ) ) *
               distance;
    }

    Extent2D ProjectBox( const glm::vec3& half, const float distance, const float aspect )
    {
        const glm::mat4 view = glm::lookAt( EyeAt( distance ), glm::vec3( 0.0f ), glm::vec3( 0, 1, 0 ) );
        const glm::mat4 proj = glm::perspective( kPreviewFov, aspect, 1.0f, 100000.0f );
        Extent2D        e;
        for ( int corner = 0; corner < 8; ++corner )
        {
            const glm::vec4 c( ( corner & 1 ) != 0 ? half.x : -half.x, ( corner & 2 ) != 0 ? half.y : -half.y,
                               ( corner & 4 ) != 0 ? half.z : -half.z, 1.0f );
            const glm::vec4 clip = proj * view * c;
            e.X                  = std::max( e.X, std::abs( clip.x / clip.w ) );
            e.Y                  = std::max( e.Y, std::abs( clip.y / clip.w ) );
        }
        return e;
    }

    // A sphere seen from its centre's axis is a circle of angular radius asin(R/d); on screen that is
    // tan(asin(R/d)) against each axis's tan(fov/2).
    Extent2D ProjectBall( const float radius, const float distance, const float aspect )
    {
        const float t    = std::tan( std::asin( radius / distance ) );
        const float tanV = std::tan( kPreviewFov * 0.5f );
        return { t / ( tanV * aspect ), t / tanV };
    }

    constexpr float kFill = 1.0f - 2.0f * PaneLayout::kFrameMargin; // the share of the narrow side it spans
    constexpr float kTol  = 1e-3f;
} // namespace

TEST( PreviewPaneLayout, TheBallFitsTheNarrowerSideWithAMarginAtEveryPaneShape )
{
    const PaneLayout::FramedSubject ball{ true, 50.0f, glm::vec3( 50.0f ) };
    for ( const float aspect : { 0.45f /* tall narrow */, 1.0f /* square */, 2.4f /* wide short */ } )
    {
        const float    d = PaneLayout::FitDistance( ball, PaneLayout::kFramingYaw, PaneLayout::kFramingPitch,
                                                    kPreviewFov, aspect );
        const Extent2D e = ProjectBall( ball.Radius, d, aspect );
        EXPECT_LE( e.X, kFill + kTol ) << "aspect " << aspect << ": the ball overflows the pane sideways";
        EXPECT_LE( e.Y, kFill + kTol ) << "aspect " << aspect << ": the ball overflows the pane vertically";
        // Tight on the binding side: fitted, not merely shrunk to a dot.
        EXPECT_NEAR( std::max( e.X, e.Y ), kFill, kTol ) << "aspect " << aspect;
        EXPECT_NEAR( aspect < 1.0f ? e.X : e.Y, kFill, kTol )
             << "aspect " << aspect << ": the NARROWER side must be the one the ball spans";
    }
}

TEST( PreviewPaneLayout, TheCubeFitsTheNarrowerSideWithAMarginAtEveryPaneShape )
{
    const PaneLayout::FramedSubject cube{ false, 50.0f * std::numbers::sqrt3_v<float>, glm::vec3( 50.0f ) };
    for ( const float aspect : { 0.45f, 1.0f, 2.4f } )
    {
        const float    d = PaneLayout::FitDistance( cube, PaneLayout::kFramingYaw, PaneLayout::kFramingPitch,
                                                    kPreviewFov, aspect );
        const Extent2D e = ProjectBox( cube.HalfExtent, d, aspect );
        EXPECT_LE( e.X, kFill + kTol ) << "aspect " << aspect << ": the cube overflows the pane sideways";
        EXPECT_LE( e.Y, kFill + kTol ) << "aspect " << aspect << ": the cube overflows the pane vertically";
        EXPECT_NEAR( std::max( e.X, e.Y ), kFill, kTol ) << "aspect " << aspect << ": not fitted, only shrunk";
    }
}

TEST( PreviewPaneLayout, ACardIsNeverFramedFromInsideItsOwnSphere )
{
    // A flat card's corners allow a distance shorter than its radius; the camera must not stand in it.
    const PaneLayout::FramedSubject card{ false, 50.0f, glm::vec3( 50.0f, 0.0f, 0.0f ) };
    const float                     d =
         PaneLayout::FitDistance( card, PaneLayout::kFramingYaw, PaneLayout::kFramingPitch, kPreviewFov, 1.0f );
    EXPECT_GE( d, card.Radius );
}

TEST( PreviewPaneLayout, AnUndrawableAspectIsFittedAsASquareNotAtInfinity )
{
    const PaneLayout::FramedSubject ball{ true, 50.0f, glm::vec3( 50.0f ) };
    const float                     square = PaneLayout::FitDistance( ball, 0.0f, 0.0f, kPreviewFov, 1.0f );
    for ( const float aspect : { 0.0f, -1.0f, std::nanf( "" ), INFINITY } )
        EXPECT_FLOAT_EQ( PaneLayout::FitDistance( ball, 0.0f, 0.0f, kPreviewFov, aspect ), square ) << aspect;
}

// ── The viewport toolbar wraps instead of clipping ─────────────────────────────────────────────────────

TEST( PreviewPaneLayout, ToolbarItemsWrapExactlyWhenTheyWouldCrossTheEdge )
{
    // Shape 110 + Lighting 150 + Floor 60 + Reset View 90 with 8 px spacing = 434 px on one line.
    const float items[]   = { 110.0f, 150.0f, 60.0f, 90.0f };
    const auto  lineCount = []( const float( &widths )[4], const float available )
    {
        int   lines = 0;
        float used  = 0.0f;
        for ( const float w : widths )
        {
            if ( used == 0.0f || PaneLayout::WrapsToNextLine( used, w, 8.0f, available ) )
            {
                ++lines;
                used = w;
            }
            else
                used += 8.0f + w;
            EXPECT_LE( used, std::max( available, w ) ) << "an item was placed past the edge at " << available;
        }
        return lines;
    };
    EXPECT_EQ( lineCount( items, 434.0f ), 1 ) << "a toolbar that exactly fits must not wrap";
    EXPECT_EQ( lineCount( items, 433.0f ), 2 );
    EXPECT_EQ( lineCount( items, 268.0f ), 2 ); // Shape + Lighting | Floor + Reset View
    EXPECT_EQ( lineCount( items, 160.0f ), 3 ); // the minimum preview width: Shape | Lighting | Floor + Reset
    EXPECT_FALSE( PaneLayout::WrapsToNextLine( 0.0f, 500.0f, 8.0f, 160.0f ) )
         << "the first item of a line has nothing to wrap away from";
}
