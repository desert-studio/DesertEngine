// ── THE NAMED VIEWPORT ANGLES, AND THE ONE RELATION THAT WAS BROKEN ────────────────────────────────
//
// The four-up grid opens four viewports and puts each on a named angle. Every pane prints the angle it
// is on, and that caption is DERIVED from the camera every frame rather than remembered, so that
// orbiting away from Top turns it into "Ortho" instead of leaving a claim the picture no longer
// supports. Good rule — and for Top and Bottom it never once printed the right thing.
//
// The reason is a relation between two files that both looked correct on their own:
//
//   * `EditorCamera::OnUpdate` clamps pitch to ±89° EVERY FRAME, because glm::lookAt degenerates when
//     the forward direction is parallel to the up vector. So `SnapToDirection( 0, -1, 0 )` is undone
//     one frame later and the camera settles one full degree off straight down.
//   * `PresetOfCamera` accepted half a degree of error.
//
// Neither number is wrong by itself. Together they mean the two views a four-up grid exists for are the
// only two that can never name themselves. Nothing could notice: the view WAS top-down to look at, the
// build was green, and the caption "Ortho" is a true sentence about a camera that is on Top.
//
// So the rows here are relations, and the load-bearing one is `TopSurvivesTheCamerasOwnPitchClamp`.
// `FiveDegreesOffIsNotOnAnyPreset` is its NEGATIVE CONTROL: without it the whole file would pass just
// as well against a tolerance of ninety degrees, which would make every caption say "Top".
//
// This asks the rule about a DIRECTION and never builds a camera — `ViewportCameraPreset.hpp`
// deliberately forward-declares the camera instead of including it, which is what lets a suite that
// links no graphics at all reach the decision.

#include <Editor/Panels/ViewportPanel/ViewportCameraPreset.hpp>

#include <Engine/Core/CameraPitchLimit.hpp>

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using Desert::Editor::kPresetToleranceDegrees;
using Desert::Editor::kViewportCameraPresets;
using Desert::Editor::PresetOfDirection;
using Desert::Editor::ViewportCameraPreset;
using Desert::Editor::ViewportCameraPresetRow;

namespace
{
    glm::vec3 ForwardOf( const ViewportCameraPresetRow& row )
    {
        return glm::vec3( row.ForwardX, row.ForwardY, row.ForwardZ );
    }

    // @p axis must not be parallel to @p v; every call below picks one that is not.
    glm::vec3 TiltedBy( const glm::vec3& v, float degrees, const glm::vec3& axis )
    {
        return glm::vec3( glm::rotate( glm::mat4( 1.0f ), glm::radians( degrees ), axis ) * glm::vec4( v, 0.0f ) );
    }
} // namespace

// THE TOLERANCE MUST ADMIT THE CLAMP. Stated at compile time and over the clamp's own constant, not
// over a copy of the number: two literals that agree today are what produced the defect.
static_assert( kPresetToleranceDegrees > Desert::Core::kCameraPitchClampMissDegrees,
               "a camera on Top settles kCameraPitchClampMissDegrees off-axis and must still be "
               "recognised as Top" );

// AND IT MUST STILL BE A TOLERANCE. Half of the 90° between two neighbouring axis presets: any larger
// and two rows could claim one direction, which would make the caption a coin toss rather than a fact.
static_assert( kPresetToleranceDegrees < 45.0f, "the tolerance must not let two presets claim one direction" );

TEST( ViewportCameraPreset, EveryAxisPresetNamesItsOwnDirection )
{
    for ( const ViewportCameraPresetRow& row : kViewportCameraPresets )
    {
        if ( !row.Orthographic )
            continue;
        const auto found = PresetOfDirection( ForwardOf( row ), true );
        ASSERT_TRUE( found.has_value() ) << row.Name << " does not recognise its own direction";
        EXPECT_EQ( *found, row.Preset ) << row.Name << " is recognised as something else";
    }
}

TEST( ViewportCameraPreset, TopSurvivesTheCamerasOwnPitchClamp )
{
    // THE ROW THE DEFECT FAILED. A camera asked for Top holds 89° of pitch, not 90 — so what the panel
    // is handed to name is this direction, and never the table's own.
    const float clamped = Desert::Core::kCameraPitchClampMissDegrees;

    for ( const ViewportCameraPreset preset : { ViewportCameraPreset::Top, ViewportCameraPreset::Bottom } )
    {
        const ViewportCameraPresetRow& row = Desert::Editor::ViewportCameraPresetRowOf( preset );
        // Tilted about X and about Z: the clamp leaves the residual in whichever plane the yaw happens
        // to be in, so neither one of them may be the only case that works.
        for ( const glm::vec3& axis : { glm::vec3( 1.0f, 0.0f, 0.0f ), glm::vec3( 0.0f, 0.0f, 1.0f ) } )
        {
            const auto found = PresetOfDirection( TiltedBy( ForwardOf( row ), clamped, axis ), true );
            ASSERT_TRUE( found.has_value() )
                 << row.Name << " is unnamed at the " << clamped << " degrees the camera actually holds";
            EXPECT_EQ( *found, preset ) << row.Name << " is misnamed at the camera's clamped pitch";
        }
    }
}

TEST( ViewportCameraPreset, FiveDegreesOffIsNotOnAnyPreset )
{
    // THE NEGATIVE CONTROL. A caption that survives a real orbit is a caption that lies; five degrees is
    // a short mouse drag. Without this row the suite passes against any tolerance at all.
    for ( const ViewportCameraPresetRow& row : kViewportCameraPresets )
    {
        if ( !row.Orthographic )
            continue;
        const glm::vec3 axis =
             ( glm::abs( row.ForwardY ) > 0.5f ) ? glm::vec3( 1.0f, 0.0f, 0.0f ) : glm::vec3( 0.0f, 1.0f, 0.0f );
        EXPECT_FALSE( PresetOfDirection( TiltedBy( ForwardOf( row ), 5.0f, axis ), true ).has_value() )
             << row.Name << " still claims a camera orbited five degrees off it";
    }
}

TEST( ViewportCameraPreset, APerspectiveCameraIsAlwaysPerspective )
{
    // Whatever it is pointing at. That is what UE's perspective viewport means, and it is the one preset
    // that constrains no direction — so a perspective camera aimed exactly down must NOT read as Top.
    const auto found = PresetOfDirection( glm::vec3( 0.0f, -1.0f, 0.0f ), false );
    ASSERT_TRUE( found.has_value() );
    EXPECT_EQ( *found, ViewportCameraPreset::Perspective );
}

TEST( ViewportCameraPreset, ADegenerateDirectionIsRefusedRatherThanGuessed )
{
    // A zero direction is what an uninitialised or torn-down camera hands over. Normalising it would
    // produce NaNs and a dot product that compares false against everything, which happens to give the
    // right answer here for the wrong reason — so it is refused explicitly instead.
    EXPECT_FALSE( PresetOfDirection( glm::vec3( 0.0f ), true ).has_value() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
