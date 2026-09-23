// ── AN AXIS VIEW THAT IS ONE DEGREE OFF IS NOT AN AXIS VIEW ────────────────────────────────────────
//
// The editor camera orbits on a yaw/pitch pair with world up as its up vector, and `EditorCamera::
// OnUpdate` clamps the pitch to ±89° on EVERY frame because `glm::lookAt` collapses when the forward
// direction is parallel to the up vector. So a camera asked for Top — pitch −90° — was corrected back
// to −89° one frame later and settled there: the four-up grid's plan pane was a high camera, verticals
// leaned, and a measurement read off it was wrong. Nothing could see it. The picture looked top-down,
// every suite was green, and the pane's caption ("Ortho") was a true sentence about a camera on Top.
//
// THE FIX IS NOT A WIDER CLAMP. At exactly ±90° SOMETHING has to be the up vector, and choosing it
// inside the orbit changes orbiting in every viewport in the editor for the sake of two views that do
// not orbit at all. So an axis view stopped being an orbit angle and became a BASIS — forward AND up,
// named per axis — which the orbit's two angles cannot express and the clamp therefore never sees.
//
// WHAT EACH ROW BELOW IS FOR:
//
//   * `EveryAxisViewIsHeldExactly` and `NoAxisViewIsDegenerate` are the claim: ±90° is reachable and
//     safe. The second is the one that would have caught a naive fix — world up as Top's up vector
//     passes "the direction is exact" and produces a view matrix that renders nothing.
//   * `TheOrbitModelStillCannotHoldAnAxisView` is the NEGATIVE CONTROL, and it is the row that makes
//     the file mean something. It asserts that the OTHER path still misses by the clamp's own degree —
//     without it every row here would pass just as well if `AxisViewBasisOf` quietly returned whatever
//     the orbit produces, which is the defect itself.
//   * `TheClampIsUNCHANGED` pins the thing this work deliberately did NOT touch.
//
// No camera, no device, no Input singleton: `Camera.cpp` is compiled by no suite
// (scripts/CI/UnreachedSources.sh), and the camera calls exactly the functions asked here.

#include <Engine/Core/CameraPitchLimit.hpp>
#include <Engine/Core/EditorCameraBasis.hpp>

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>

using Desert::Core::AxisViewBasisOf;
using Desert::Core::ClampOrbitPitch;
using Desert::Core::kCameraPitchClampMissDegrees;
using Desert::Core::kMaxCameraPitch;
using Desert::Core::OrbitForwardFor;
using Desert::Core::ViewBasis;

namespace
{
    // The six world axes, spelled here rather than imported from the viewport's preset table: this file
    // is about the ENGINE's basis and must not pass because the table happens to agree with itself.
    constexpr std::array<glm::vec3, 6> kAxes = {
         glm::vec3{ 0.0f, -1.0f, 0.0f }, glm::vec3{ 0.0f, 1.0f, 0.0f }, glm::vec3{ 0.0f, 0.0f, -1.0f },
         glm::vec3{ 0.0f, 0.0f, 1.0f },  glm::vec3{ 1.0f, 0.0f, 0.0f }, glm::vec3{ -1.0f, 0.0f, 0.0f },
    };

    float AngleBetweenDegrees( const glm::vec3& a, const glm::vec3& b )
    {
        return glm::degrees(
             glm::acos( glm::clamp( glm::dot( glm::normalize( a ), glm::normalize( b ) ), -1.0f, 1.0f ) ) );
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// The claim.
// ---------------------------------------------------------------------------------------------------

TEST( EditorCameraBasis, EveryAxisViewIsHeldExactly )
{
    // EXACTLY, and not "within a tolerance". The whole reason this path exists is that "near enough"
    // was what the camera already offered and what a plan view cannot use — so the assertion is on the
    // components, which is the strongest statement available and the one a regression breaks first.
    for ( const glm::vec3& axis : kAxes )
    {
        const auto basis = AxisViewBasisOf( axis );
        ASSERT_TRUE( basis.has_value() )
             << "(" << axis.x << "," << axis.y << "," << axis.z << ") is not an axis view";
        EXPECT_FLOAT_EQ( basis->Forward.x, axis.x );
        EXPECT_FLOAT_EQ( basis->Forward.y, axis.y );
        EXPECT_FLOAT_EQ( basis->Forward.z, axis.z );
    }
}

TEST( EditorCameraBasis, NoAxisViewIsDegenerate )
{
    // THE ROW A NAIVE FIX FAILS. Widening the clamp to ±90° and leaving world up as the up vector gives
    // an exact direction AND a view matrix that collapses every vertex onto one clip position — a black
    // viewport that reads as a broken renderer. Forward x Up of unit length is exactly the property
    // `glm::lookAt` needs, so it is asserted rather than assumed, and the matrix is built to prove it.
    for ( const glm::vec3& axis : kAxes )
    {
        const auto basis = AxisViewBasisOf( axis );
        ASSERT_TRUE( basis.has_value() );

        EXPECT_NEAR( glm::length( basis->Forward ), 1.0f, 1e-5f );
        EXPECT_NEAR( glm::length( basis->Up ), 1.0f, 1e-5f );
        EXPECT_NEAR( glm::length( glm::cross( basis->Forward, basis->Up ) ), 1.0f, 1e-5f )
             << "forward and up are parallel: glm::lookAt degenerates here";
        EXPECT_NEAR( glm::dot( basis->Forward, basis->Up ), 0.0f, 1e-5f );

        const glm::vec3 eye  = glm::vec3( 0.0f, 200.0f, 0.0f );
        const glm::mat4 view = glm::lookAt( eye, eye + basis->Forward, basis->Up );
        EXPECT_GT( glm::abs( glm::determinant( view ) ), 0.5f ) << "the view matrix is singular";
    }
}

TEST( EditorCameraBasis, TheTwoPoleViewsKeepPlusXToTheRight )
{
    // Top and Bottom are the two that had to CHOOSE an up vector, and the choice is not free: right =
    // forward x up, so picking +Z for one and +Z for the other would mirror the world under the cursor
    // when the user flips between them. Both put +X to the right of the screen.
    const auto top    = AxisViewBasisOf( glm::vec3( 0.0f, -1.0f, 0.0f ) );
    const auto bottom = AxisViewBasisOf( glm::vec3( 0.0f, 1.0f, 0.0f ) );
    ASSERT_TRUE( top.has_value() );
    ASSERT_TRUE( bottom.has_value() );

    const glm::vec3 topRight    = glm::cross( top->Forward, top->Up );
    const glm::vec3 bottomRight = glm::cross( bottom->Forward, bottom->Up );
    EXPECT_NEAR( topRight.x, 1.0f, 1e-5f );
    EXPECT_NEAR( bottomRight.x, 1.0f, 1e-5f );
}

TEST( EditorCameraBasis, ADirectionThatIsNotAnAxisIsRefusedRatherThanRoundedToOne )
{
    // An axis view is a thing you ENTER, never a thing you drift into: five degrees off Top is a short
    // mouse drag, and an orbiting camera that silently became "on Top" again would be the original
    // defect wearing the fix's clothes. The zero vector is what a torn-down camera hands over.
    const glm::vec3 tilted =
         glm::vec3( glm::rotate( glm::mat4( 1.0f ), glm::radians( 5.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) ) *
                    glm::vec4( 0.0f, -1.0f, 0.0f, 0.0f ) );
    EXPECT_FALSE( AxisViewBasisOf( tilted ).has_value() );
    EXPECT_FALSE( AxisViewBasisOf( glm::vec3( 0.0f ) ).has_value() );
}

// ---------------------------------------------------------------------------------------------------
// The negative control, and the thing that was deliberately left alone.
// ---------------------------------------------------------------------------------------------------

TEST( EditorCameraBasis, TheOrbitModelStillCannotHoldAnAxisView )
{
    // THE ROW THAT MAKES THIS FILE MEAN SOMETHING. If `AxisViewBasisOf` ever came to be implemented over
    // the orbit — or if a preset were routed back through `SnapToDirection` — the assertions above would
    // still have to be false, and this states by how much: the clamp's own miss, taken from the clamp's
    // own constant rather than from a second copy of the number.
    for ( const glm::vec3& axis : kAxes )
    {
        const bool  pole    = glm::abs( axis.y ) > 0.5f;
        const float missing = AngleBetweenDegrees( OrbitForwardFor( axis ), axis );
        if ( pole )
        {
            EXPECT_NEAR( missing, kCameraPitchClampMissDegrees, 1e-3f )
                 << "the orbit is expected to miss Top/Bottom by exactly the clamp";
        }
        else
        {
            EXPECT_NEAR( missing, 0.0f, 1e-3f ) << "the clamp must not touch a horizontal axis";
        }
    }
}

TEST( EditorCameraBasis, TheClampIsUNCHANGED )
{
    // Widening it was the obvious fix and it was refused: the price is a second up vector at the poles
    // for every orbiting viewport in the editor. This row is the pin on that decision — the axis views
    // got their exactness by leaving the orbit, not by loosening it.
    EXPECT_FLOAT_EQ( ClampOrbitPitch( glm::radians( 120.0f ) ), kMaxCameraPitch );
    EXPECT_FLOAT_EQ( ClampOrbitPitch( glm::radians( -120.0f ) ), -kMaxCameraPitch );
    EXPECT_FLOAT_EQ( ClampOrbitPitch( 0.25f ), 0.25f );
    EXPECT_FLOAT_EQ( kMaxCameraPitch, glm::radians( 90.0f - kCameraPitchClampMissDegrees ) );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
