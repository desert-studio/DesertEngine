// GP2a: the playable character's movement model (UE CharacterMovementComponent walking / falling / crouch) and
// the spring arm (UE USpringArmComponent), driven against a device-free PhysicsWorld in centimetres.
#include <Engine/ECS/System/CharacterMovement.hpp>
#include <Engine/ECS/System/SpringArm.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <gtest/gtest.h>

using namespace Desert;

namespace
{
    constexpr float kGravity = 981.0f;
    constexpr float kDt      = 1.0f / 120.0f;

    Physics::BodyHandle AddBox( Physics::PhysicsWorld& world, const glm::vec3& center, const glm::vec3& half )
    {
        Physics::BodyDesc desc;
        desc.Shape       = Physics::ShapeType::Box;
        desc.HalfExtents = half;
        desc.Type        = Physics::BodyType::Static;
        desc.Position    = center;
        return world.CreateBody( desc ).GetValue();
    }

    struct Fixture
    {
        Physics::PhysicsWorld             world;
        ECS::CharacterControllerComponent cc;

        Fixture()
        {
            world.Init( kGravity );
            AddBox( world, { 0.0f, -50.0f, 0.0f }, { 5000.0f, 50.0f, 5000.0f } ); // floor top at y = 0
            ECS::CharacterMovement::CreateCharacter( cc, world, { 0.0f, cc.Data.Height * 0.5f + 1.0f, 0.0f } );
            Run( { 0.0f, 0.0f, 0.0f }, 60 ); // settle on the floor
        }

        void Run( const glm::vec3& input, int frames )
        {
            for ( int i = 0; i < frames; ++i )
            {
                world.Step( kDt );
                ECS::CharacterMovement::Step( cc, world, input, kDt );
            }
        }

        float MeasuredPlanarSpeed( const glm::vec3& input )
        {
            const glm::vec3 a = world.GetCharacterPosition( cc.RuntimeCharacter );
            Run( input, 1 );
            const glm::vec3 b = world.GetCharacterPosition( cc.RuntimeCharacter );
            return glm::length( glm::vec2( b.x - a.x, b.z - a.z ) ) / kDt;
        }
    };
} // namespace

TEST( CharacterMovement, AcceleratesToMaxWalkSpeedAndBrakesToZeroOnFlatGround )
{
    Fixture f;
    ASSERT_TRUE( f.cc.OnGround ) << "the character did not settle on the floor";

    f.Run( { 0.0f, 0.0f, -1.0f }, 6 );
    EXPECT_LT( f.cc.CurrentSpeed, f.cc.Data.MaxWalkSpeed * 0.5f ) << "acceleration is not finite";

    f.Run( { 0.0f, 0.0f, -1.0f }, 120 );
    EXPECT_NEAR( f.MeasuredPlanarSpeed( { 0.0f, 0.0f, -1.0f } ), f.cc.Data.MaxWalkSpeed,
                 f.cc.Data.MaxWalkSpeed * 0.02f )
         << "a full stick must reach Max Walk Speed (cm/s) and no more";

    f.Run( { 0.0f, 0.0f, 0.0f }, 120 );
    EXPECT_EQ( f.cc.CurrentSpeed, 0.0f ) << "braking must stop the character";
    EXPECT_LT( f.MeasuredPlanarSpeed( { 0.0f, 0.0f, 0.0f } ), 1.0f );
}

TEST( CharacterMovement, CrouchedSpeedIsLimitedToMaxWalkSpeedCrouched )
{
    Fixture f;
    f.cc.CrouchRequested = true;
    f.Run( { 1.0f, 0.0f, 0.0f }, 240 );
    ASSERT_TRUE( f.cc.IsCrouched );
    EXPECT_NEAR( f.MeasuredPlanarSpeed( { 1.0f, 0.0f, 0.0f } ), f.cc.Data.MaxWalkSpeedCrouched,
                 f.cc.Data.MaxWalkSpeedCrouched * 0.02f );
    EXPECT_LT( f.cc.Data.MaxWalkSpeedCrouched, f.cc.Data.MaxWalkSpeed * 0.9f )
         << "the test needs the two to differ";
}

TEST( CharacterMovement, JumpApexIsJumpZVelocitySquaredOverTwoG )
{
    Fixture     f;
    const float start  = f.world.GetCharacterPosition( f.cc.RuntimeCharacter ).y;
    f.cc.JumpRequested = true;
    float apex         = start;
    for ( int i = 0; i < 240; ++i )
    {
        f.Run( { 0.0f, 0.0f, 0.0f }, 1 );
        apex = std::max( apex, f.world.GetCharacterPosition( f.cc.RuntimeCharacter ).y );
    }
    const float v        = f.cc.Data.JumpZVelocity;
    const float expected = v * v / ( 2.0f * kGravity * f.cc.Data.GravityScale );
    EXPECT_NEAR( apex - start, expected, expected * 0.03f );
    EXPECT_TRUE( f.cc.OnGround ) << "the character must land again";
}

TEST( CharacterMovement, CannotUncrouchUnderALowCeilingAndCanOnceItIsRemoved )
{
    Fixture f;
    f.cc.CrouchRequested = true;
    f.Run( { 0.0f, 0.0f, 0.0f }, 10 );
    ASSERT_TRUE( f.cc.IsCrouched );

    // A slab whose underside is between the crouched head and the standing head.
    const float underside = ( f.cc.Data.CrouchedHeight + f.cc.Data.Height ) * 0.5f;
    const auto  ceiling   = AddBox( f.world, { 0.0f, underside + 10.0f, 0.0f }, { 500.0f, 10.0f, 500.0f } );

    f.cc.CrouchRequested = false;
    f.Run( { 0.0f, 0.0f, 0.0f }, 30 );
    EXPECT_TRUE( f.cc.IsCrouched ) << "stood up into the ceiling";
    EXPECT_FALSE( ECS::CharacterMovement::CanUncrouch( f.cc, f.world ) );

    f.world.RemoveBody( ceiling );
    EXPECT_TRUE( ECS::CharacterMovement::CanUncrouch( f.cc, f.world ) ) << "the floor counted as an obstacle";
    f.Run( { 0.0f, 0.0f, 0.0f }, 2 );
    EXPECT_FALSE( f.cc.IsCrouched );
    const float feet = f.world.GetCharacterPosition( f.cc.RuntimeCharacter ).y - f.cc.Data.Height * 0.5f;
    EXPECT_NEAR( feet, 0.0f, 3.0f ) << "standing up must keep the feet on the floor";
}

TEST( SpringArm, PullsInAgainstAWallAndReturnsWhenItIsGone )
{
    Physics::PhysicsWorld world;
    world.Init( kGravity );
    ECS::SpringArmComponent arm;
    arm.Data.TargetArmLength = 300.0f;
    arm.Data.ProbeSize       = 12.0f;
    const glm::vec3 origin( 0.0f, 100.0f, 0.0f );
    const glm::quat identity( 1.0f, 0.0f, 0.0f, 0.0f ); // the arm hangs along +Z

    auto free = ECS::SpringArm::Solve( arm, origin, identity, kDt, &world );
    EXPECT_FALSE( free.Blocked );
    EXPECT_NEAR( free.ArmLength, 300.0f, 0.01f );

    // A wall whose near face is 150 cm behind the origin.
    const auto wall = AddBox( world, { 0.0f, 100.0f, 160.0f }, { 500.0f, 500.0f, 10.0f } );
    auto       hit  = ECS::SpringArm::Solve( arm, origin, identity, kDt, &world );
    EXPECT_TRUE( hit.Blocked );
    EXPECT_NEAR( hit.ArmLength, 150.0f - arm.Data.ProbeSize, 1.0f ) << "the probe sphere must stop at the wall";
    EXPECT_LT( hit.CameraPosition.z, 150.0f );

    world.RemoveBody( wall );
    auto back = ECS::SpringArm::Solve( arm, origin, identity, kDt, &world );
    EXPECT_FALSE( back.Blocked );
    EXPECT_NEAR( back.ArmLength, 300.0f, 0.01f );

    arm.Data.DoCollisionTest = false;
    AddBox( world, { 0.0f, 100.0f, 160.0f }, { 500.0f, 500.0f, 10.0f } );
    EXPECT_NEAR( ECS::SpringArm::Solve( arm, origin, identity, kDt, &world ).ArmLength, 300.0f, 0.01f )
         << "Do Collision Test off must not sweep";
}

TEST( SpringArm, CameraLagClosesItsSpeedFractionPerSecondOfTheOrigin )
{
    ECS::SpringArmComponent arm;
    arm.Data.EnableCameraLag = true;
    arm.Data.CameraLagSpeed  = 10.0f;
    arm.Data.TargetArmLength = 0.0f;
    const glm::quat identity( 1.0f, 0.0f, 0.0f, 0.0f );
    ECS::SpringArm::Solve( arm, { 0.0f, 0.0f, 0.0f }, identity, 0.05f, nullptr );
    const auto lagged = ECS::SpringArm::Solve( arm, { 100.0f, 0.0f, 0.0f }, identity, 0.05f, nullptr );
    EXPECT_NEAR( lagged.CameraPosition.x, 50.0f, 0.01f ) << "0.05 s at speed 10 closes half the distance";
}
