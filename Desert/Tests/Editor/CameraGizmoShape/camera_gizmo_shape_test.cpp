// У8-1 — the camera gizmo's shape.
//
// WHAT WAS ON SCREEN, BEFORE ANY OF THIS EXISTED. The frustum was drawn by inverting a projection
// built with `gizmoFar = min(cam.Far, cam.Near + 2.5f)`. That literal predates the decision that a
// world unit is a CENTIMETRE, so with the shipped defaults (Near 10, Far 5 000 000) the drawn volume
// ran from 10 cm to 12.5 cm: an eight-centimetre rectangle with two and a half centimetres behind it,
// and NOTHING drawn between it and the camera's own position. At a working distance of six metres it
// is a fifteen-pixel bracket beside the icon. That is the owner's "the lines are broken".
//
// THE ASSERTIONS ARE RELATIONS, NOT VALUES, and they come in two kinds:
//
//   * the ones the old shape FAILS — the edges start at the camera (ApexIsTheCameraItself), and the
//     drawn size is a constant share of the screen rather than a constant number of world units
//     (ApparentSizeIsConstantInDistance / ...AcrossFieldOfView). These are the defect;
//   * the one it PASSES — RectangleIsTheCamerasOwnCone. It is the negative control. A suite built
//     only from "the rectangle has the right aspect and subtends the right angle" would have been
//     green over a gizmo nobody could read, because both halves of that statement were already true.
//
// Nothing here needs a device, a scene or a frame: the subject is a pure header.

#include <gtest/gtest.h>

#include <Editor/Panels/ViewportPanel/Tools/CameraGizmoMath.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using Desert::Editor::Tools::BuildCameraFrustumGizmo;
using Desert::Editor::Tools::CameraFrustumGizmo;
using Desert::Editor::Tools::GizmoFrustumDepth;
using Desert::Editor::Tools::kGizmoHalfExtentPerDistance;
using Desert::Editor::Tools::kGizmoMinDepth;

namespace
{
    // The shipped defaults, spelled in the units the engine actually uses: 1 unit = 1 cm.
    constexpr float kNearPlane = 10.0f;      // 10 cm
    constexpr float kFarPlane  = 5000000.0f; // 50 km
    constexpr float kAspect    = 16.0f / 9.0f;

    float TanHalf( float fovDegrees )
    {
        return std::tan( glm::radians( fovDegrees ) * 0.5f );
    }

    // Half the distance between the top and the bottom edge of the drawn rectangle.
    float FarHalfHeight( const CameraFrustumGizmo& g )
    {
        return glm::length( g.FarCorners[3] - g.FarCorners[0] ) * 0.5f;
    }

    float FarHalfWidth( const CameraFrustumGizmo& g )
    {
        return glm::length( g.FarCorners[1] - g.FarCorners[0] ) * 0.5f;
    }

    glm::vec3 FarCentre( const CameraFrustumGizmo& g )
    {
        return ( g.FarCorners[0] + g.FarCorners[1] + g.FarCorners[2] + g.FarCorners[3] ) * 0.25f;
    }

    // A camera at the origin looking down -Z, the identity orientation every .desce authors.
    glm::mat4 CameraAt( const glm::vec3& position, float scale = 1.0f )
    {
        return glm::scale( glm::translate( glm::mat4( 1.0f ), position ), glm::vec3( scale ) );
    }
} // namespace

// ── THE DEFECT ──────────────────────────────────────────────────────────────────────────────────

// The edges of the drawn shape start AT the camera. The old construction began at the near plane and
// drew nothing between it and the camera position, so the wireframe and the icon were two unrelated
// marks with a gap between them; this is the assertion that fails on it.
TEST( CameraGizmoShape, ApexIsTheCameraItself )
{
    const glm::vec3 position( 120.0f, -30.0f, 4000.0f );
    const auto      g = BuildCameraFrustumGizmo( CameraAt( position ), kAspect, TanHalf( 45.0f ), kNearPlane, kFarPlane,
                                                 glm::vec3( 120.0f, -30.0f, 4600.0f ) );

    EXPECT_NEAR( g.Apex.x, position.x, 1.0e-3f );
    EXPECT_NEAR( g.Apex.y, position.y, 1.0e-3f );
    EXPECT_NEAR( g.Apex.z, position.z, 1.0e-3f );
}

// The shape's apparent size does not depend on how far away it is looked at from. Doubling the
// viewer's distance doubles the drawn rectangle, so the ratio between them is the invariant — which
// is what "a constant share of the screen" means as an equation. The old shape's size was a fixed
// number of world units, so this ratio fell as 1/distance.
//
// THE LADDER STARTS AT 200 cm BECAUSE THE NEAR CLAMP BINDS BELOW 82.8 cm — `0.05 * d / tan(22.5)`
// falls under a 10 cm near plane there, and the first version of this test went red at 50 cm for
// that reason and no other. The clamped half of the range is asserted by
// DrawnBaseIsNeverInsideTheNearPlane, so it is covered rather than avoided; splitting them is what
// keeps each test's failure meaningful.
TEST( CameraGizmoShape, ApparentSizeIsConstantInDistance )
{
    const glm::mat4 world = CameraAt( glm::vec3( 0.0f ) );

    float previousRatio = 0.0f;
    for ( const float distance : { 200.0f, 800.0f, 3200.0f, 12800.0f, 51200.0f } )
    {
        const auto  g     = BuildCameraFrustumGizmo( world, kAspect, TanHalf( 45.0f ), kNearPlane, kFarPlane,
                                                     glm::vec3( 0.0f, 0.0f, distance ) );
        const float ratio = FarHalfHeight( g ) / distance;

        EXPECT_NEAR( ratio, kGizmoHalfExtentPerDistance, 1.0e-4f ) << "at distance " << distance;
        if ( previousRatio > 0.0f )
            EXPECT_NEAR( ratio, previousRatio, 1.0e-4f );
        previousRatio = ratio;
    }
}

// ... and it does not depend on the camera's own field of view either. A 20-degree lens and a
// 110-degree one draw rectangles of the SAME height; what differs is how far down the axis that
// rectangle sits, which is what makes the field of view read as the pyramid's sharpness. Sizing by
// depth instead — the obvious alternative — makes a wide camera's gizmo swallow the viewport.
TEST( CameraGizmoShape, ApparentSizeIsConstantAcrossFieldOfView )
{
    const glm::mat4 world    = CameraAt( glm::vec3( 0.0f ) );
    const glm::vec3 viewer( 0.0f, 0.0f, 900.0f );

    const auto narrow = BuildCameraFrustumGizmo( world, kAspect, TanHalf( 20.0f ), kNearPlane, kFarPlane, viewer );
    const auto wide   = BuildCameraFrustumGizmo( world, kAspect, TanHalf( 110.0f ), kNearPlane, kFarPlane, viewer );

    EXPECT_NEAR( FarHalfHeight( narrow ), FarHalfHeight( wide ), 1.0e-2f );
    // The field of view has to show up SOMEWHERE, and this is where: the narrow lens reaches further.
    EXPECT_GT( narrow.Depth, wide.Depth * 2.0f );
}

// ── THE NEGATIVE CONTROL ────────────────────────────────────────────────────────────────────────

// The rectangle is a cross-section of the camera's OWN cone: its half-height over its distance is
// tan(fov/2), and its width over its height is the aspect. THE OLD SHAPE SATISFIED BOTH — it was
// built by inverting a real projection matrix — so a suite made only of this test would have passed
// over a gizmo that was unreadable at every distance. It is here to keep the two above honest.
TEST( CameraGizmoShape, RectangleIsTheCamerasOwnCone )
{
    for ( const float fov : { 20.0f, 45.0f, 90.0f } )
    {
        const auto g = BuildCameraFrustumGizmo( CameraAt( glm::vec3( 0.0f ) ), kAspect, TanHalf( fov ), kNearPlane,
                                                kFarPlane, glm::vec3( 0.0f, 0.0f, 600.0f ) );

        EXPECT_NEAR( FarHalfHeight( g ) / g.Depth, TanHalf( fov ), 1.0e-4f ) << "fov " << fov;
        EXPECT_NEAR( FarHalfWidth( g ) / FarHalfHeight( g ), kAspect, 1.0e-3f ) << "fov " << fov;

        // The rectangle is centred on the camera's forward axis at exactly Depth — the other half of
        // "this is the camera's cone", and also true of the old construction.
        const glm::vec3 offset = FarCentre( g ) - g.Apex;
        EXPECT_NEAR( offset.z, -g.Depth, 1.0e-2f ); // identity orientation looks down -Z
        EXPECT_NEAR( offset.x, 0.0f, 1.0e-2f );
        EXPECT_NEAR( offset.y, 0.0f, 1.0e-2f );
    }
}

// ── THE TWO CLAMPS, WHICH ARE WHERE THE INVARIANT IS GIVEN UP ON PURPOSE ────────────────────────

// A gizmo may not claim the camera sees past its own Far. A camera authored with a 30 cm Far draws a
// 30 cm pyramid however far away the viewer stands, and the constant-apparent-size property above is
// deliberately surrendered there — asserted rather than left to be discovered.
TEST( CameraGizmoShape, DepthNeverExceedsTheCamerasFarPlane )
{
    constexpr float kShortFar = 30.0f;

    const auto near_ = BuildCameraFrustumGizmo( CameraAt( glm::vec3( 0.0f ) ), kAspect, TanHalf( 45.0f ), kNearPlane,
                                                kShortFar, glm::vec3( 0.0f, 0.0f, 400.0f ) );
    const auto far_  = BuildCameraFrustumGizmo( CameraAt( glm::vec3( 0.0f ) ), kAspect, TanHalf( 45.0f ), kNearPlane,
                                                kShortFar, glm::vec3( 0.0f, 0.0f, 40000.0f ) );

    EXPECT_LE( near_.Depth, kShortFar );
    EXPECT_LE( far_.Depth, kShortFar );
    EXPECT_NEAR( near_.Depth, far_.Depth, 1.0e-3f ); // both pinned to Far: the clamp binds, as intended
}

// A viewer standing ON the camera must still get a shape. Without a floor the whole wireframe
// collapses to a point, and a gizmo that vanishes when you fly into it reads as a defect rather than
// as a viewpoint. The floor is the camera's own Near.
TEST( CameraGizmoShape, DepthHasAFloorAtZeroDistance )
{
    EXPECT_FLOAT_EQ( GizmoFrustumDepth( 0.0f, kNearPlane, kFarPlane, TanHalf( 45.0f ) ), kNearPlane );

    const auto g = BuildCameraFrustumGizmo( CameraAt( glm::vec3( 7.0f, 7.0f, 7.0f ) ), kAspect,
                                            TanHalf( 45.0f ), kNearPlane, kFarPlane, glm::vec3( 7.0f, 7.0f, 7.0f ) );
    EXPECT_GT( FarHalfHeight( g ), 0.0f );
}

// ── ORIENTATION ─────────────────────────────────────────────────────────────────────────────────

// Roll is invisible without the up marker: a camera rotated 180 degrees about its own forward axis
// has the same position, the same aim and the same rectangle, and only the marker separates the two.
// The apex and the rectangle's centre are asserted UNCHANGED in the same test — otherwise a build
// that simply rotated the whole shape would pass the marker half.
TEST( CameraGizmoShape, UpMarkerFollowsRoll )
{
    const glm::mat4 upright = CameraAt( glm::vec3( 0.0f ) );
    const glm::mat4 rolled  = glm::rotate( upright, glm::pi<float>(), glm::vec3( 0.0f, 0.0f, 1.0f ) );
    const glm::vec3 viewer( 0.0f, 0.0f, 900.0f );

    const auto a = BuildCameraFrustumGizmo( upright, kAspect, TanHalf( 45.0f ), kNearPlane, kFarPlane, viewer );
    const auto b = BuildCameraFrustumGizmo( rolled, kAspect, TanHalf( 45.0f ), kNearPlane, kFarPlane, viewer );

    EXPECT_NEAR( glm::length( a.Apex - b.Apex ), 0.0f, 1.0e-3f );
    EXPECT_NEAR( glm::length( FarCentre( a ) - FarCentre( b ) ), 0.0f, 1.0e-2f );

    // The marker's tip is above the rectangle's centre before the roll and below it after.
    EXPECT_GT( a.UpMarker[2].y - FarCentre( a ).y, 0.0f );
    EXPECT_LT( b.UpMarker[2].y - FarCentre( b ).y, 0.0f );
    // And it clears the top edge, so it cannot be mistaken for a corner.
    EXPECT_GT( a.UpMarker[2].y - a.FarCorners[3].y, 0.0f );
}

// The shipped scenes author cameras at scale 100 (Desert_Sandbox.desce). Scale is not one of a
// camera's optics, so the gizmo must be identical with and without it — the normalisation in
// BuildCameraFrustumGizmo is what makes that true, and dropping it multiplies the whole shape by 100.
TEST( CameraGizmoShape, EntityScaleDoesNotChangeTheShape )
{
    const glm::vec3 viewer( 0.0f, 0.0f, 900.0f );
    const auto      plain  = BuildCameraFrustumGizmo( CameraAt( glm::vec3( 0.0f ), 1.0f ), kAspect,
                                                      TanHalf( 45.0f ), kNearPlane, kFarPlane, viewer );
    const auto      scaled = BuildCameraFrustumGizmo( CameraAt( glm::vec3( 0.0f ), 100.0f ), kAspect,
                                                      TanHalf( 45.0f ), kNearPlane, kFarPlane, viewer );

    EXPECT_NEAR( plain.Depth, scaled.Depth, 1.0e-3f );
    for ( int i = 0; i < 4; ++i )
        EXPECT_NEAR( glm::length( plain.FarCorners[i] - scaled.FarCorners[i] ), 0.0f, 1.0e-2f ) << "corner " << i;
}

// THE GIZMO NEVER CLAIMS THE CAMERA SEES SOMETHING IT CLIPS AWAY, and this test is why the near
// plane became the lower clamp rather than a second rectangle.
//
// It was written to pin the opposite claim — that the near plane is always too small to be worth
// drawing — and it went RED at the first rung of its own ladder: a viewer 100 cm away gets a drawn
// depth of 12.07 cm against a 10 cm near plane, which is 83 % of the way out, and a viewer at 10 cm
// got 1.2 cm, a whole pyramid INSIDE the near plane. The header's paragraph was corrected against
// this ladder rather than the ladder trimmed to the header.
TEST( CameraGizmoShape, DrawnBaseIsNeverInsideTheNearPlane )
{
    for ( const float distance : { 0.0f, 10.0f, 100.0f, 600.0f, 5000.0f, 200000.0f } )
    {
        const auto g = BuildCameraFrustumGizmo( CameraAt( glm::vec3( 0.0f ) ), kAspect, TanHalf( 45.0f ),
                                                kNearPlane, kFarPlane, glm::vec3( 0.0f, 0.0f, distance ) );
        EXPECT_GE( g.Depth, kNearPlane ) << "at distance " << distance;
    }

    // Close in, the clamp BINDS — the base is exactly the near rectangle, which is the behaviour a
    // separately drawn near rectangle would have bought, for one rectangle instead of two.
    const auto close = BuildCameraFrustumGizmo( CameraAt( glm::vec3( 0.0f ) ), kAspect, TanHalf( 45.0f ),
                                                kNearPlane, kFarPlane, glm::vec3( 0.0f, 0.0f, 10.0f ) );
    EXPECT_FLOAT_EQ( close.Depth, kNearPlane );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
