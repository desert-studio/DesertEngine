// W-0: the world moves in fixed steps, and forces go through it.
//
//   1. The step is fixed: a hitch takes at most kMaxStepsPerFrame steps, a frame shorter than a step takes none,
//      and the clock is the step count.
//   2. A force held every frame gives the same momentum at every frame rate (30, 60, 144, 240): it acts on
//      every step of the frame's Step, and a frame that took no step does not leave its force to add up with
//      the next frame's.
//   3. A force added in the pre-step callback acts on that one step only.
//   4. AddForceAtPoint off the centre spins the body, at the centre it does not; AddTorque spins without moving;
//      a static body ignores forces.
//   5. The pose to draw lies between the last two steps' poses by the interpolation alpha; a teleport is not
//      swept.
//
// Mutations the lead runs (each must turn this suite red):
//   - PhysicsWorld::Step applies the held forces on the first step only        -> HeldForce... (30 fps)
//   - Impl::HoldOrApply never clears after a stepless Step (HeldForcesWaited)   -> HeldForce... (144, 240 fps)
//   - Impl::Apply passes no point for ForceAtPoint                              -> ForceAtPoint...
//   - Step drops the kMaxStepsPerFrame bound                                    -> AHitch...
//   - GetInterpolatedPosition returns GetPosition                               -> InterpolatedPose...

#include <Engine/Physics/PhysicsWorld.hpp>

#include "../PhysicsFixture.hpp"

#include <glm/geometric.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace Desert;

namespace
{
    constexpr float kMass      = 10.0f;   // kg
    constexpr float kForce     = 1000.0f; // kg*cm/s^2: 100 cm/s^2 on kMass
    constexpr float kHalfSize  = 50.0f;   // cm
    constexpr float kOneSecond = 60.0f;   // steps

    struct StepWorld
    {
        Physics::PhysicsWorld Physics;

        explicit StepWorld( float gravityCmPerS2 = 0.0f )
        {
            EXPECT_TRUE( Physics.Init( gravityCmPerS2, TestSupport::PhysicsTestProfiles() ) );
        }
        ~StepWorld()
        {
            Physics.Shutdown();
        }

        Physics::BodyHandle Box( const glm::vec3& position, Physics::BodyType type = Physics::BodyType::Dynamic )
        {
            Physics::BodyDesc desc;
            desc.HalfExtents = glm::vec3( kHalfSize );
            desc.Mass        = kMass;
            desc.Type        = type;
            desc.Position    = position;
            desc.Profile     = TestSupport::ProfileId( Physics, type == Physics::BodyType::Static ? "BlockAll"
                                                                                                  : "PhysicsActor" );
            auto created     = Physics.CreateBody( desc );
            EXPECT_TRUE( created ) << created.GetError();
            return created ? created.GetValue() : Physics::kInvalidBody;
        }
    };

    /// The X velocity after one simulated second of kForce held every frame at @p framesPerSecond.
    float VelocityAfterHeldForce( float framesPerSecond )
    {
        StepWorld                 world;
        const Physics::BodyHandle body = world.Box( glm::vec3( 0.0f ) );
        while ( world.Physics.GetStepCount() < static_cast<uint64_t>( kOneSecond ) )
        {
            world.Physics.AddForce( body, glm::vec3( kForce, 0.0f, 0.0f ) );
            world.Physics.Step( 1.0f / framesPerSecond );
        }
        EXPECT_EQ( world.Physics.GetStepCount(), static_cast<uint64_t>( kOneSecond ) )
             << framesPerSecond << " fps";
        return world.Physics.GetLinearVelocity( body ).x;
    }
} // namespace

TEST( PhysicsStep, AHitchTakesAtMostTheStepsAllowed )
{
    StepWorld world;
    world.Physics.Step( 1.0f );
    EXPECT_EQ( world.Physics.GetStepCount(), Physics::kMaxStepsPerFrame );
    EXPECT_DOUBLE_EQ( world.Physics.GetSimulatedSeconds(),
                      Physics::kMaxStepsPerFrame * static_cast<double>( Physics::kFixedStepSeconds ) );
    EXPECT_GE( world.Physics.GetInterpolationAlpha(), 0.0f );
    EXPECT_LT( world.Physics.GetInterpolationAlpha(), 1.0f );
}

TEST( PhysicsStep, AFrameShorterThanAStepTakesNone )
{
    StepWorld world;
    world.Physics.Step( Physics::kFixedStepSeconds * 0.5f );
    EXPECT_EQ( world.Physics.GetStepCount(), 0u );
    EXPECT_NEAR( world.Physics.GetInterpolationAlpha(), 0.5f, 1e-4f );
    world.Physics.Step( Physics::kFixedStepSeconds * 0.5f );
    EXPECT_EQ( world.Physics.GetStepCount(), 1u );
}

TEST( PhysicsStep, HeldForceGivesTheSameMomentumAtEveryFrameRate )
{
    const float reference = VelocityAfterHeldForce( 60.0f );
    // a = F/m over one second, less what Jolt's linear damping takes (0.05 per second).
    const float undamped = kForce / kMass;
    EXPECT_LE( reference, undamped );
    EXPECT_GE( reference, undamped * 0.94f );

    for ( const float framesPerSecond : { 30.0f, 144.0f, 240.0f } )
        EXPECT_EQ( VelocityAfterHeldForce( framesPerSecond ), reference ) << framesPerSecond << " fps";
}

TEST( PhysicsStep, AForceFromThePreStepCallbackActsOnThatStepOnly )
{
    // Held from the frame for one step, then nothing...
    StepWorld                 held;
    const Physics::BodyHandle heldBody = held.Box( glm::vec3( 0.0f ) );
    held.Physics.AddForce( heldBody, glm::vec3( kForce, 0.0f, 0.0f ) );
    held.Physics.Step( Physics::kFixedStepSeconds );
    held.Physics.Step( Physics::kFixedStepSeconds );

    // ...is what the callback does when it pushes on the first of two steps of one frame.
    StepWorld                 callback;
    const Physics::BodyHandle body = callback.Box( glm::vec3( 0.0f ) );
    std::vector<float>        steps;
    callback.Physics.SetPreStepCallback(
         [&]( float dt )
         {
             if ( steps.empty() )
                 callback.Physics.AddForce( body, glm::vec3( kForce, 0.0f, 0.0f ) );
             steps.push_back( dt );
         } );
    callback.Physics.Step( Physics::kFixedStepSeconds * 2.0f );
    callback.Physics.SetPreStepCallback( {} );

    ASSERT_EQ( steps.size(), 2u );
    EXPECT_EQ( steps[0], Physics::kFixedStepSeconds );
    EXPECT_EQ( steps[1], Physics::kFixedStepSeconds );
    EXPECT_GT( callback.Physics.GetLinearVelocity( body ).x, 0.0f );
    EXPECT_EQ( callback.Physics.GetLinearVelocity( body ).x, held.Physics.GetLinearVelocity( heldBody ).x );
    EXPECT_EQ( callback.Physics.GetPosition( body ).x, held.Physics.GetPosition( heldBody ).x );
}

TEST( PhysicsStep, ForceAtPointSpinsOffTheCentreAndNotOnIt )
{
    StepWorld                 world;
    const Physics::BodyHandle centred = world.Box( glm::vec3( 0.0f ) );
    const Physics::BodyHandle offset  = world.Box( glm::vec3( 1000.0f, 0.0f, 0.0f ) );
    const Physics::BodyHandle plain   = world.Box( glm::vec3( -1000.0f, 0.0f, 0.0f ) );
    const glm::vec3           force( kForce, 0.0f, 0.0f );

    world.Physics.AddForceAtPoint( centred, force, glm::vec3( 0.0f ) );
    // +X pushed at +Z of the centre: the torque r x F points along +Y.
    world.Physics.AddForceAtPoint( offset, force, glm::vec3( 1000.0f, 0.0f, kHalfSize ) );
    world.Physics.AddForce( plain, force );
    world.Physics.Step( Physics::kFixedStepSeconds );

    EXPECT_LT( glm::length( world.Physics.GetAngularVelocity( centred ) ), 1e-6f );
    EXPECT_GT( world.Physics.GetAngularVelocity( offset ).y, 0.0f );
    EXPECT_NEAR( world.Physics.GetLinearVelocity( offset ).x, world.Physics.GetLinearVelocity( plain ).x, 1e-4f );
    EXPECT_NEAR( world.Physics.GetLinearVelocity( centred ).x, world.Physics.GetLinearVelocity( plain ).x, 1e-4f );
}

TEST( PhysicsStep, TorqueSpinsWithoutMoving )
{
    StepWorld                 world;
    const Physics::BodyHandle body = world.Box( glm::vec3( 0.0f ) );
    world.Physics.AddTorque( body, glm::vec3( 0.0f, kForce * kHalfSize, 0.0f ) );
    world.Physics.Step( Physics::kFixedStepSeconds );
    EXPECT_GT( world.Physics.GetAngularVelocity( body ).y, 0.0f );
    EXPECT_LT( glm::length( world.Physics.GetLinearVelocity( body ) ), 1e-6f );
}

TEST( PhysicsStep, AStaticBodyIgnoresForces )
{
    StepWorld                 world;
    const Physics::BodyHandle body = world.Box( glm::vec3( 0.0f ), Physics::BodyType::Static );
    world.Physics.AddForce( body, glm::vec3( kForce, 0.0f, 0.0f ) );
    world.Physics.AddTorque( body, glm::vec3( 0.0f, kForce, 0.0f ) );
    world.Physics.Step( Physics::kFixedStepSeconds );
    EXPECT_EQ( world.Physics.GetPosition( body ), glm::vec3( 0.0f ) );
}

TEST( PhysicsStep, InterpolatedPoseLiesBetweenTheLastTwoSteps )
{
    StepWorld                 world( 981.0f );
    const glm::vec3           start( 0.0f, 1000.0f, 0.0f );
    const Physics::BodyHandle body = world.Box( start );

    world.Physics.Step( Physics::kFixedStepSeconds );
    const glm::vec3 afterStep = world.Physics.GetPosition( body );
    ASSERT_LT( afterStep.y, start.y );
    EXPECT_NEAR( world.Physics.GetInterpolatedPosition( body ).y, start.y, 1e-3f ); // alpha 0: the pose before

    world.Physics.Step( Physics::kFixedStepSeconds * 0.5f ); // no step: half way into the next
    EXPECT_EQ( world.Physics.GetPosition( body ), afterStep );
    EXPECT_NEAR( world.Physics.GetInterpolatedPosition( body ).y, ( start.y + afterStep.y ) * 0.5f, 1e-3f );

    // A teleport is drawn where it lands, not swept from where the body was.
    const glm::vec3 teleported( 0.0f, 5000.0f, 0.0f );
    world.Physics.SetTransform( body, teleported, glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ) );
    EXPECT_NEAR( world.Physics.GetInterpolatedPosition( body ).y, teleported.y, 1e-3f );
}
