// GP2a: the playable character's movement model (UE CharacterMovementComponent walking / falling / crouch) and
// the spring arm (UE USpringArmComponent), driven against a device-free PhysicsWorld in centimetres.
#include <Engine/Animation/Graph/AnimGraph.hpp>
#include <Engine/ECS/System/CharacterMovement.hpp>
#include <Engine/ECS/System/SpringArm.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

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

// ---- Jolt in centimetres ----

namespace
{
    std::string RepoRootForCensus()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 8; ++up )
        {
            if ( std::filesystem::exists( prefix + "Desert/Desert/Source/Engine/Physics/PhysicsWorld.cpp" ) )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadText( const std::string& path )
    {
        const std::ifstream in( path );
        std::ostringstream  ss;
        ss << in.rdbuf();
        return ss.str();
    }
} // namespace

TEST( JoltCentimetres, EveryLengthSpeedAndForceSettingIsAssignedInTheOnePlace )
{
    const std::string root = RepoRootForCensus();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string source = ReadText( root + "Desert/Desert/Source/Engine/Physics/PhysicsWorld.cpp" );

    const std::string kBegin = "// ---- Jolt in centimetres (the one place) ----";
    const std::string kEnd   = "// ---- end Jolt in centimetres ----";
    const size_t      begin  = source.find( kBegin );
    const size_t      end    = source.find( kEnd );
    ASSERT_NE( begin, std::string::npos ) << "the centimetre block is gone from PhysicsWorld.cpp";
    ASSERT_NE( end, std::string::npos );
    ASSERT_LT( begin, end );
    const std::string block   = source.substr( begin, end - begin );
    const std::string outside = source.substr( 0, begin ) + source.substr( end );

    const char* const settings[] = {
         // PhysicsSettings (the world)
         "mBaumgarte", "mSpeculativeContactDistance", "mPenetrationSlop", "mLinearCastThreshold",
         "mLinearCastMaxPenetration", "mManifoldTolerance", "mMaxPenetrationDistance",
         "mBodyPairCacheMaxDeltaPositionSq", "mContactPointPreserveLambdaMaxDistSq", "mMinVelocityForRestitution",
         "mPointVelocitySleepThreshold",
         // CharacterVirtualSettings (the character)
         "mMaxStrength", "mPredictiveContactDistance", "mCharacterPadding", "mCollisionTolerance",
         "mPenetrationRecoverySpeed", "mMaxCollisionIterations", "mMaxConstraintIterations", "mMinTimeRemaining" };
    for ( const char* name : settings )
    {
        const std::regex assigned( std::string( R"(\.\s*)" ) + name + R"(\s*=[^=])" );
        EXPECT_TRUE( std::regex_search( block, assigned ) )
             << name << " is not assigned in the centimetre block: it runs at Jolt's metre default";
        EXPECT_FALSE( std::regex_search( outside, assigned ) )
             << name << " is assigned outside the centimetre block: two places own one unit decision";
    }
    EXPECT_NE( source.find( "SetPhysicsSettings( CentimetrePhysicsSettings() )" ), std::string::npos )
         << "the world never receives the centimetre PhysicsSettings";
    EXPECT_NE( source.find( "ApplyCentimetreCharacterSettings( settings )" ), std::string::npos )
         << "CreateCharacter never applies the centimetre CharacterVirtualSettings";
}

TEST( JoltCentimetres, EveryConvexShapePassesTheCentimetreConvexRadius )
{
    const std::string root = RepoRootForCensus();
    ASSERT_FALSE( root.empty() ) << "run from inside the repository";
    const std::string source = ReadText( root + "Desert/Desert/Source/Engine/Physics/PhysicsWorld.cpp" );
    EXPECT_NE( source.find( "constexpr float kConvexRadiusCm = 5.0f;" ), std::string::npos )
         << "the engine's convex radius (0.05 m = 5 cm) is gone from the centimetre block";

    // Every construction of a shape that HAS a convex radius: without the argument Jolt's
    // cDefaultConvexRadius (0.05, a metre value) is used, i.e. half a millimetre in this world.
    const std::regex site(
         R"(new\s+JPH::(Box|Cylinder|TaperedCylinder)Shape\s*\(|JPH::(Box|Cylinder|ConvexHull)ShapeSettings\s+\w+\s*\()" );
    int sites = 0;
    for ( auto it = std::sregex_iterator( source.begin(), source.end(), site ); it != std::sregex_iterator();
          ++it )
    {
        ++sites;
        const size_t      close = source.find( ';', static_cast<size_t>( it->position() ) );
        const std::string call =
             source.substr( static_cast<size_t>( it->position() ), close - static_cast<size_t>( it->position() ) );
        EXPECT_NE( call.find( "kConvexRadiusCm" ), std::string::npos )
             << "a convex shape is built without the engine's convex radius: " << call;
    }
    EXPECT_GE( sites, 2 ) << "the Box and ConvexHull construction sites were not found: the census reads nothing";
    EXPECT_EQ( source.find( "cDefaultConvexRadius" ) == std::string::npos ||
                    source.find( "cDefaultConvexRadius" ) < source.find( "constexpr float kConvexRadiusCm" ),
               true )
         << "cDefaultConvexRadius is used by code (only the centimetre block may name it, in its comment)";
}

TEST( JoltCentimetres, CharacterStandsOnAFlatFloorWithoutVerticalJitter )
{
    Fixture f;
    ASSERT_TRUE( f.cc.OnGround ) << "the character did not settle on the floor";

    float lowest        = f.world.GetCharacterPosition( f.cc.RuntimeCharacter ).y;
    float highest       = lowest;
    int   airborneSteps = 0;
    for ( int i = 0; i < 300; ++i )
    {
        f.Run( { 0.0f, 0.0f, 0.0f }, 1 );
        const float y = f.world.GetCharacterPosition( f.cc.RuntimeCharacter ).y;
        lowest        = std::min( lowest, y );
        highest       = std::max( highest, y );
        airborneSteps += f.cc.OnGround ? 0 : 1;
    }
    EXPECT_LE( highest - lowest, 0.1f ) << "the capsule centre moved " << ( highest - lowest )
                                        << " cm while standing still: metre-tuned padding / contact distances";
    EXPECT_EQ( airborneSteps, 0 ) << "the ground state flickered while standing on a flat floor";
    EXPECT_NEAR( lowest, f.cc.Data.Height * 0.5f, 3.0f ) << "the capsule does not rest on the floor (top y = 0)";
}

// ---- GP2b: movement -> AnimGraph parameters (UE AnimBP reads Speed / IsFalling / IsCrouching) ----

namespace
{
    std::shared_ptr<Animation::Graph::AnimGraph> LocomotionGraph( bool declareCrouch )
    {
        auto graph = std::make_shared<Animation::Graph::AnimGraph>();
        graph->Parameters.push_back(
             { ECS::CharacterMovement::kAnimParamSpeed, static_cast<int>( Animation::Graph::ParamType::Float ) } );
        graph->Parameters.push_back( { ECS::CharacterMovement::kAnimParamIsFalling,
                                       static_cast<int>( Animation::Graph::ParamType::Bool ) } );
        if ( declareCrouch )
            graph->Parameters.push_back( { ECS::CharacterMovement::kAnimParamIsCrouched,
                                           static_cast<int>( Animation::Graph::ParamType::Bool ) } );
        return graph;
    }

    const ECS::AnimationComponent::PendingGraphParam* Queued( const ECS::AnimationComponent& anim,
                                                              const char*                    name )
    {
        for ( const auto& p : anim.PendingGraphParams )
            if ( p.Name == name )
                return &p;
        return nullptr;
    }
} // namespace

TEST( CharacterAnimGraph, MovementPublishesSpeedFallingAndCrouchIntoTheDeclaredParameters )
{
    Fixture f;
    f.Run( { 0.0f, 0.0f, -1.0f }, 120 ); // walking at Max Walk Speed on the ground
    ASSERT_TRUE( f.cc.OnGround );

    ECS::AnimationComponent anim;
    anim.Graph = LocomotionGraph( /*declareCrouch=*/true );
    ECS::CharacterMovement::PublishAnimGraphParameters( f.cc, anim );
    ASSERT_NE( Queued( anim, "Speed" ), nullptr ) << "Speed was not published";
    EXPECT_NEAR( Queued( anim, "Speed" )->Value, f.cc.Data.MaxWalkSpeed, f.cc.Data.MaxWalkSpeed * 0.02f )
         << "Speed is the planar speed in cm/s";
    ASSERT_NE( Queued( anim, "IsFalling" ), nullptr );
    EXPECT_EQ( Queued( anim, "IsFalling" )->Value, 0.0f );
    ASSERT_NE( Queued( anim, "IsCrouched" ), nullptr );
    EXPECT_EQ( Queued( anim, "IsCrouched" )->Value, 0.0f );

    // A jump: the next frames are airborne, and the same queue entry carries the new value (no pile-up while
    // the evaluator does not exist yet).
    f.cc.JumpRequested = true;
    f.Run( { 0.0f, 0.0f, 0.0f }, 6 );
    ASSERT_FALSE( f.cc.OnGround );
    ECS::CharacterMovement::PublishAnimGraphParameters( f.cc, anim );
    EXPECT_EQ( Queued( anim, "IsFalling" )->Value, 1.0f ) << "airborne is IsFalling";
    EXPECT_EQ( anim.PendingGraphParams.size(), 3u ) << "a second publish appended instead of replacing";

    // Crouched on the ground.
    f.Run( { 0.0f, 0.0f, 0.0f }, 240 );
    f.cc.CrouchRequested = true;
    f.Run( { 0.0f, 0.0f, 0.0f }, 2 );
    ASSERT_TRUE( f.cc.IsCrouched );
    ECS::CharacterMovement::PublishAnimGraphParameters( f.cc, anim );
    EXPECT_EQ( Queued( anim, "IsCrouched" )->Value, 1.0f );
    EXPECT_EQ( Queued( anim, "IsFalling" )->Value, 0.0f );
}

TEST( CharacterAnimGraph, OnlyDeclaredParametersAreWrittenAndNothingBeforeTheGraphLoads )
{
    Fixture                 f;
    ECS::AnimationComponent anim; // GraphAsset named, graph not loaded yet
    ECS::CharacterMovement::PublishAnimGraphParameters( f.cc, anim );
    EXPECT_TRUE( anim.PendingGraphParams.empty() ) << "a write before the graph exists has no declaration to obey";

    anim.Graph = LocomotionGraph( /*declareCrouch=*/false );
    ECS::CharacterMovement::PublishAnimGraphParameters( f.cc, anim );
    EXPECT_NE( Queued( anim, "Speed" ), nullptr );
    EXPECT_EQ( Queued( anim, "IsCrouched" ), nullptr )
         << "a graph that does not declare IsCrouched must not be told about it (the drain would refuse it)";
}

TEST( CharacterAnimGraph, LocomotionSystemHasNoClipNameSwitchLeft )
{
    const std::string root = RepoRootForCensus();
    ASSERT_FALSE( root.empty() );
    const std::string system = ReadText( root + "Desert/Desert/Source/Engine/ECS/System/LocomotionSystem.hpp" );
    EXPECT_NE( system.find( "PublishAnimGraphParameters" ), std::string::npos )
         << "LocomotionSystem must drive animation through the AnimGraph parameters";
    for ( const char* gone : { "CurrentClip", "LocomotionClipFor", "IdleClip", "RunClip" } )
        EXPECT_EQ( system.find( gone ), std::string::npos ) << gone << ": a second path picks clips by name";
}
