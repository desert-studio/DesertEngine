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
// THE TILT IS NOW FIXED AT THE SOURCE AND THIS FILE CHANGED SIDES WITH IT. An axis preset is handed to
// the camera as a BASIS -- forward and up both named, `EditorCamera::SnapToAxisView` -- which the
// orbit's yaw/pitch pair cannot express and the clamp therefore never sees. Top means exactly (0,-1,0).
//
// So the tolerance's job inverted: it used to have to ADMIT the clamp's degree, and now it has to
// REFUSE it, because a camera that reached an axis through the orbit is a plan view that is one degree
// off and must not be named after the plan. `EveryAxisPresetIsHeldExactly` is the load-bearing row --
// it asserts that every parallel row yields a basis, so no axis preset can reach the orbit path -- and
// `AnAxisReachedThroughTheOrbitClampIsNotOnThePreset` is what turns red if one ever does.
// `FiveDegreesOffIsNotOnAnyPreset` is the other control: without it the whole file would pass just
// as well against a tolerance of ninety degrees, which would make every caption say "Top".
//
// This asks the rule about a DIRECTION and never builds a camera — `ViewportCameraPreset.hpp`
// deliberately forward-declares the camera instead of including it, which is what lets a suite that
// links no graphics at all reach the decision.

#include <Editor/Panels/ViewportPanel/ViewportCameraPreset.hpp>

#include <Engine/Core/CameraPitchLimit.hpp>
#include <Engine/Core/EditorCameraBasis.hpp>

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using Desert::Editor::kPresetToleranceDegrees;
using Desert::Editor::kViewportCameraPresets;
using Desert::Editor::PresetOfDirection;
using Desert::Editor::ViewportCameraAimOf;
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

// THE TOLERANCE MUST REFUSE THE CLAMP. Stated at compile time and over the clamp's own constant, not
// over a copy of the number: two literals that agree today are what produced the defect. This is the
// INVERSE of what stood here, and deliberately so -- the axis presets hold their direction exactly now,
// so a camera sitting kCameraPitchClampMissDegrees off straight down did NOT arrive by a preset and
// must not be captioned as one.
static_assert( kPresetToleranceDegrees < Desert::Core::kCameraPitchClampMissDegrees,
               "an axis reached through the orbit clamp is kCameraPitchClampMissDegrees off and must "
               "not be recognised as that axis" );

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

TEST( ViewportCameraPreset, EveryAxisPresetIsHeldExactly )
{
    // THE LOAD-BEARING ROW, AND THE ONE THAT GOES RED IF A PRESET IS ROUTED BACK THROUGH THE ORBIT.
    //
    // Every parallel row must yield an axis BASIS, because that is the only spelling
    // `ApplyViewportCameraPreset` can hand to the camera without passing through `SnapToDirection` and
    // its ±89° clamp. And the basis must be the table's own direction bit for bit: "near enough to
    // recognise" is what the camera already offered and what a plan view cannot use.
    for ( const ViewportCameraPresetRow& row : kViewportCameraPresets )
    {
        const auto aim = ViewportCameraAimOf( row.Preset );
        EXPECT_EQ( aim.Orthographic, row.Orthographic ) << row.Name;

        if ( !row.Orthographic )
        {
            // The one preset that constrains no direction must NOT be pinned to a basis: a perspective
            // viewport orbits by definition, and taking that away is the opposite of what it means.
            EXPECT_FALSE( aim.Axis.has_value() ) << row.Name << " must be left free to orbit";
            continue;
        }

        ASSERT_TRUE( aim.Axis.has_value() )
             << row.Name << " has no axis basis, so it would be aimed through the orbit's pitch clamp";
        EXPECT_FLOAT_EQ( aim.Axis->Forward.x, row.ForwardX ) << row.Name;
        EXPECT_FLOAT_EQ( aim.Axis->Forward.y, row.ForwardY ) << row.Name;
        EXPECT_FLOAT_EQ( aim.Axis->Forward.z, row.ForwardZ ) << row.Name;

        // And the basis has to be one `glm::lookAt` can use — the whole reason Top could not be an orbit
        // angle is that its up vector is parallel to its forward.
        EXPECT_NEAR( glm::length( glm::cross( aim.Axis->Forward, aim.Axis->Up ) ), 1.0f, 1e-5f ) << row.Name;

        // The exact direction names its own preset, which is the caption the pane prints.
        const auto found = PresetOfDirection( aim.Axis->Forward, true );
        ASSERT_TRUE( found.has_value() ) << row.Name << " does not name itself at the angle it holds";
        EXPECT_EQ( *found, row.Preset ) << row.Name;
    }
}

TEST( ViewportCameraPreset, AnAxisReachedThroughTheOrbitClampIsNotOnThePreset )
{
    // THE ROW THE DEFECT USED TO REQUIRE, INVERTED. A camera aimed at Top through the orbit holds 89° of
    // pitch, not 90, and the four-up grid's plan pane looked like that for as long as the preset went
    // that way. It must no longer pass for Top: a plan view one degree off is not a plan, and a caption
    // that says otherwise is the failure this whole file exists to prevent.
    //
    // The miss is taken from the camera's own clamp through `OrbitForwardFor` — the same function
    // `EditorCamera` calls — rather than reconstructed here, so there is no second copy to drift.
    for ( const ViewportCameraPreset preset : { ViewportCameraPreset::Top, ViewportCameraPreset::Bottom } )
    {
        const ViewportCameraPresetRow& row = Desert::Editor::ViewportCameraPresetRowOf( preset );
        EXPECT_FALSE( PresetOfDirection( Desert::Core::OrbitForwardFor( ForwardOf( row ) ), true ).has_value() )
             << row.Name << " still claims a camera that reached it through the orbit's pitch clamp";
    }

    // The four horizontal presets are unaffected by the clamp and must keep naming themselves — the
    // tolerance tightened, and a tolerance tightened too far would break these first.
    for ( const ViewportCameraPresetRow& row : kViewportCameraPresets )
    {
        if ( !row.Orthographic || glm::abs( row.ForwardY ) > 0.5f )
            continue;
        const auto found = PresetOfDirection( Desert::Core::OrbitForwardFor( ForwardOf( row ) ), true );
        ASSERT_TRUE( found.has_value() ) << row.Name;
        EXPECT_EQ( *found, row.Preset ) << row.Name;
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
