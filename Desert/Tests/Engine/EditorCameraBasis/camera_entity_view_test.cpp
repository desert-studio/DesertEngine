// CAM1 — what a camera entity sees (Engine/Core/CameraEntityView.hpp) and the roll the editor camera
// keeps while piloting one (RollFreeUp / UpWithRoll / RollOf in EditorCameraBasis.hpp).
//
// THE OWNER'S COMPLAINT, AS A RELATION. "I change the camera's data and the view stays the same": the
// Details button moved the editor camera once, forward only, and kept the editor's own lens. So the
// assertions here are about the PICTURE a camera's parameters produce, read back through the matrices
// the renderer is handed: a point at the top edge of the camera's FOV lands on the top edge of the
// frame, a point at Near lands on the near depth, and a rolled camera sees the world rolled. Changing
// the FOV must change where that point lands — the negative control for "the view did not move".

#include <gtest/gtest.h>

#include <Engine/Core/CameraEntityView.hpp>
#include <Engine/Core/EditorCameraBasis.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace Desert::Core;

namespace
{
    constexpr uint32_t kWidth  = 1600;
    constexpr uint32_t kHeight = 900;

    // Where a world point lands: NDC x/y in [-1, 1], z the (reversed) depth.
    glm::vec3 Project( const CameraEntityView& view, const glm::vec3& world )
    {
        const glm::vec4 clip =
             ProjectionOf( view, kWidth, kHeight ) * ViewMatrixOf( view ) * glm::vec4( world, 1.0f );
        return glm::vec3( clip ) / clip.w;
    }

    // A camera at @p position, yawed then pitched then rolled (radians), scaled — built the way a
    // TransformComponent composes it.
    glm::mat4 CameraWorld( const glm::vec3& position, float yaw, float pitch, float roll, float scale = 1.0f )
    {
        glm::mat4 m = glm::translate( glm::mat4( 1.0f ), position );
        m           = glm::rotate( m, yaw, glm::vec3( 0.0f, 1.0f, 0.0f ) );
        m           = glm::rotate( m, pitch, glm::vec3( 1.0f, 0.0f, 0.0f ) );
        m           = glm::rotate( m, roll, glm::vec3( 0.0f, 0.0f, -1.0f ) );
        return glm::scale( m, glm::vec3( scale ) );
    }
} // namespace

// The top edge of the camera's vertical FOV is the top edge of the frame, at 30 and at 90 degrees — and
// the SAME world point lands in different places at the two, which is the owner's "the view stays the
// same" turned into a red.
TEST( CameraEntityView, FieldOfViewIsTheFramesVerticalExtent )
{
    const glm::vec3 eye( 100.0f, 200.0f, 300.0f );
    const float     distance = 1000.0f;

    for ( const float fov : { 30.0f, 90.0f } )
    {
        const auto      view = CameraEntityViewOf( CameraWorld( eye, 0.0f, 0.0f, 0.0f ), fov, 10.0f, 100000.0f );
        const float     top  = distance * std::tan( glm::radians( fov ) * 0.5f );
        const glm::vec3 ndc  = Project( view, eye + glm::vec3( 0.0f, top, -distance ) );
        EXPECT_NEAR( ndc.x, 0.0f, 1.0e-4f ) << fov;
        EXPECT_NEAR( std::abs( ndc.y ), 1.0f, 1.0e-4f ) << fov;
    }

    const glm::vec3 probe = eye + glm::vec3( 0.0f, 200.0f, -1000.0f );
    const auto      narrow =
         Project( CameraEntityViewOf( CameraWorld( eye, 0.0f, 0.0f, 0.0f ), 30.0f, 10.0f, 1.0e5f ), probe );
    const auto wide =
         Project( CameraEntityViewOf( CameraWorld( eye, 0.0f, 0.0f, 0.0f ), 90.0f, 10.0f, 1.0e5f ), probe );
    EXPECT_GT( std::abs( narrow.y ), 2.5f * std::abs( wide.y ) );
}

// Near and Far are the component's, reversed-Z: Near maps to 1, Far to 0.
TEST( CameraEntityView, NearAndFarAreTheComponents )
{
    const auto view =
         CameraEntityViewOf( CameraWorld( glm::vec3( 0.0f ), 0.0f, 0.0f, 0.0f ), 45.0f, 25.0f, 8000.0f );
    EXPECT_NEAR( Project( view, glm::vec3( 0.0f, 0.0f, -25.0f ) ).z, 1.0f, 1.0e-5f );
    EXPECT_NEAR( Project( view, glm::vec3( 0.0f, 0.0f, -8000.0f ) ).z, 0.0f, 1.0e-5f );
    EXPECT_FLOAT_EQ( view.Near, 25.0f );
    EXPECT_FLOAT_EQ( view.Far, 8000.0f );
}

// Roll reaches the picture. A camera rolled 30 degrees sees a point straight above it (in world) off to
// the side — and the point along its OWN up axis straight up. The old Details button lost exactly this.
TEST( CameraEntityView, RollReachesThePicture )
{
    const float     roll  = glm::radians( 30.0f );
    const glm::mat4 world = CameraWorld( glm::vec3( 0.0f ), 0.4f, -0.2f, roll );
    const auto      view  = CameraEntityViewOf( world, 60.0f, 10.0f, 1.0e5f );

    EXPECT_NEAR( RollOf( view.Basis ), roll, 1.0e-4f );

    const glm::vec3 ownUp   = glm::normalize( glm::vec3( world[1] ) );
    const glm::vec3 ahead   = view.Position + view.Basis.Forward * 1000.0f;
    const glm::vec3 onOwnUp = Project( view, ahead + ownUp * 100.0f );
    EXPECT_NEAR( onOwnUp.x, 0.0f, 1.0e-4f );
    EXPECT_GT( onOwnUp.y, 0.0f );

    // Negative control: the same camera without roll has world-up straight up, and this one does not.
    const auto flat =
         CameraEntityViewOf( CameraWorld( glm::vec3( 0.0f ), 0.4f, -0.2f, 0.0f ), 60.0f, 10.0f, 1.0e5f );
    EXPECT_NEAR( RollOf( flat.Basis ), 0.0f, 1.0e-4f );
    // World-up (roll-free) lands sin(30 deg) of its length to the side: 100 * 0.5 over the half-width of
    // the frame at 1000 cm, which is 1000 * tan(30 deg) * aspect.
    const float aspect   = static_cast<float>( kWidth ) / static_cast<float>( kHeight );
    const float expected = 100.0f * std::sin( roll ) / ( 1000.0f * std::tan( glm::radians( 30.0f ) ) * aspect );
    EXPECT_NEAR( std::abs( Project( view, ahead + RollFreeUp( view.Basis.Forward ) * 100.0f ).x ), expected,
                 1.0e-4f );
    EXPECT_GT( expected, 0.04f );
}

// The shipped scenes author cameras at scale 100. Scale is not an optic.
TEST( CameraEntityView, EntityScaleIsNotAnOptic )
{
    const auto plain =
         CameraEntityViewOf( CameraWorld( glm::vec3( 5.0f ), 1.0f, 0.3f, 0.2f, 1.0f ), 50.0f, 10.0f, 1.0e5f );
    const auto scaled =
         CameraEntityViewOf( CameraWorld( glm::vec3( 5.0f ), 1.0f, 0.3f, 0.2f, 100.0f ), 50.0f, 10.0f, 1.0e5f );
    EXPECT_NEAR( glm::length( plain.Basis.Forward - scaled.Basis.Forward ), 0.0f, 1.0e-5f );
    EXPECT_NEAR( glm::length( plain.Basis.Up - scaled.Basis.Up ), 0.0f, 1.0e-5f );
    EXPECT_NEAR( glm::length( plain.Position - scaled.Position ), 0.0f, 1.0e-4f );
}

// The pilot writes a pose back through CameraWorldTransformFor; reading it again must give the pose
// that was written, roll and all, and must keep the entity's scale.
TEST( CameraEntityView, WriteBackIsTheInverse )
{
    const glm::vec3 position( -300.0f, 150.0f, 42.0f );
    const glm::vec3 forward = glm::normalize( glm::vec3( 0.3f, -0.5f, -0.8f ) );
    const ViewBasis basis{ forward, UpWithRoll( forward, glm::radians( -20.0f ) ) };
    const glm::vec3 scale( 100.0f );

    const glm::mat4 world = CameraWorldTransformFor( position, basis, scale );
    const auto      back  = CameraEntityViewOf( world, 45.0f, 10.0f, 1.0e5f );

    EXPECT_NEAR( glm::length( back.Position - position ), 0.0f, 1.0e-4f );
    EXPECT_NEAR( glm::length( back.Basis.Forward - basis.Forward ), 0.0f, 1.0e-5f );
    EXPECT_NEAR( glm::length( back.Basis.Up - basis.Up ), 0.0f, 1.0e-5f );
    EXPECT_NEAR( glm::length( glm::vec3( world[0] ) ), 100.0f, 1.0e-3f );
}

// Roll's two functions are inverses across forwards and angles, including looking straight down.
TEST( EditorCameraRoll, UpWithRollAndRollOfAreInverses )
{
    const glm::vec3 forwards[] = {
         { 0.0f, 0.0f, -1.0f }, glm::normalize( glm::vec3( 1.0f, -0.4f, 0.2f ) ), { 0.0f, -1.0f, 0.0f } };
    for ( const glm::vec3& f : forwards )
    {
        for ( const float degrees : { -150.0f, -30.0f, 0.0f, 30.0f, 90.0f, 170.0f } )
        {
            const float     roll = glm::radians( degrees );
            const glm::vec3 up   = UpWithRoll( f, roll );
            EXPECT_NEAR( glm::dot( up, f ), 0.0f, 1.0e-5f );
            EXPECT_NEAR( RollOf( ViewBasis{ f, up } ), roll, 1.0e-4f ) << degrees;
        }
    }
}

// Roll zero is the orbit's own up — so the editor camera's orbit with m_Roll == 0 and a roll-carrying
// orbit agree, and a viewport that never pilots sees nothing change. Straight down it is the Top axis
// view's up, so the two spellings meet there too.
TEST( EditorCameraRoll, ZeroRollIsTheOrbitsUp )
{
    const glm::vec3 f = glm::normalize( glm::vec3( 0.6f, -0.3f, -0.7f ) );
    const glm::mat4 a = glm::lookAt( glm::vec3( 0.0f ), f, glm::vec3( 0.0f, 1.0f, 0.0f ) );
    const glm::mat4 b = glm::lookAt( glm::vec3( 0.0f ), f, UpWithRoll( f, 0.0f ) );
    for ( int c = 0; c < 4; ++c )
        EXPECT_NEAR( glm::length( a[c] - b[c] ), 0.0f, 1.0e-5f );

    const auto top = AxisViewBasisOf( glm::vec3( 0.0f, -1.0f, 0.0f ) );
    ASSERT_TRUE( top.has_value() );
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_NEAR( glm::length( RollFreeUp( top->Forward ) - top->Up ), 0.0f, 1.0e-6f );
    // NOLINTEND(bugprone-unchecked-optional-access)
}
