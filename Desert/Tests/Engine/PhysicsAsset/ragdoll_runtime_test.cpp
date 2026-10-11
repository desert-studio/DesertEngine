// RAG1b: the ragdoll in a real Jolt world (PhysicsWorld is device-free) and the pose in both directions —
// animated bones -> body targets, simulated bodies -> bone locals through the parent chain, with a 0.01 FBX root.
#include <Engine/Animation/BoneControl.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Physics/PhysicsAssetFormat.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>
#include <Engine/Physics/RagdollDesc.hpp>
#include <Engine/Physics/RagdollPose.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace Desert::Physics;
using Desert::Animation::BoneInfo;
using Desert::Animation::LocalPose;
using Desert::Animation::Skeleton;

namespace
{
    constexpr float kStep       = 1.0f / 60.0f;
    constexpr float kSwingLimit = 30.0f; // degrees, both cone half-angles
    constexpr float kTwistLimit = 20.0f; // degrees, +-
    constexpr float kLimitSlack = 3.0f;  // degrees a soft solve may overshoot by
    constexpr float kRadiusCm   = 5.0f;
    constexpr float kLengthCm   = 20.0f; // between the capsule's cap centres
    constexpr float kLinkCm     = 30.0f; // bone to child bone

    BoneInfo MakeBone( const char* name, std::optional<uint32_t> parent, const glm::mat4& local )
    {
        BoneInfo b;
        b.Name               = name;
        b.ParentBoneID       = parent;
        b.LocalBindTransform = local;
        return b;
    }

    // a(0) -> b(1) -> c(2), each link kLinkCm along the bone's +Z.
    Skeleton MakeChain()
    {
        const glm::mat4       link = glm::translate( glm::mat4( 1.0f ), { 0.0f, 0.0f, kLinkCm } );
        std::vector<BoneInfo> bones;
        bones.push_back( MakeBone( "a", std::nullopt, glm::mat4( 1.0f ) ) );
        bones.push_back( MakeBone( "b", 0u, link ) );
        bones.push_back( MakeBone( "c", 1u, link ) );
        return Skeleton( std::move( bones ) );
    }

    // A capsule per bone running from the bone towards its child (authored along Z, centre half a link out),
    // a swing-twist joint at each child bone.
    RagdollDesc MakeChainRagdoll( const Skeleton& skeleton )
    {
        PhysicsAssetData asset;
        asset.Guid     = { 0xBEEFull, 0x0001ull };
        asset.Skeleton = { 0xBEEFull, 0x0002ull };
        for ( const char* bone : { "a", "b", "c" } )
        {
            PhysicsAssetBody body;
            body.Bone   = bone;
            body.Shape  = PhysicsBodyShape::Capsule;
            body.Radius = kRadiusCm;
            body.Length = kLengthCm;
            body.Center = { 0.0f, 0.0f, 0.5f * kLinkCm };
            body.MassKg = 5.0f;
            asset.Bodies.push_back( body );
        }
        for ( const auto& [parent, child] : { std::pair{ "a", "b" }, std::pair{ "b", "c" } } )
        {
            PhysicsAssetConstraint joint;
            joint.ParentBone         = parent;
            joint.ChildBone          = child;
            joint.Swing1LimitDegrees = kSwingLimit;
            joint.Swing2LimitDegrees = kSwingLimit;
            joint.TwistLimitDegrees  = kTwistLimit;
            asset.Constraints.push_back( joint );
        }
        auto built = BuildRagdollDesc( asset, skeleton );
        EXPECT_TRUE( built.IsSuccess() ) << ( built.IsSuccess() ? "" : built.GetError() );
        return built.IsSuccess() ? built.GetValue() : RagdollDesc{};
    }

    void AddFloor( PhysicsWorld& world )
    {
        BodyDesc floor;
        floor.Shape       = ShapeType::Box;
        floor.HalfExtents = { 2000.0f, 10.0f, 2000.0f };
        floor.Position    = { 0.0f, -10.0f, 0.0f }; // top face at y = 0
        floor.Type        = BodyType::Static;
        ASSERT_TRUE( world.CreateBody( floor ).IsSuccess() );
    }

    // The lowest point of a part's capsule: its two cap centres (the body's +Z through the shape centre) less
    // the radius.
    float LowestPointCm( const RagdollPartTransform& body )
    {
        const glm::vec3 centre = body.Position + body.Rotation * glm::vec3( 0.0f, 0.0f, 0.5f * kLinkCm );
        const glm::vec3 half   = body.Rotation * glm::vec3( 0.0f, 0.0f, 0.5f * kLengthCm );
        return std::min( ( centre + half ).y, ( centre - half ).y ) - kRadiusCm;
    }

    // Swing and twist (degrees) of child relative to parent, both bind rotations identity, the joint frame's
    // twist axis X (Jolt's decomposition q = swing * twist).
    std::pair<float, float> SwingTwistDegrees( const glm::quat& parent, const glm::quat& child )
    {
        glm::quat relative = glm::inverse( parent ) * child;
        if ( relative.w < 0.0f )
            relative = -relative;
        const glm::quat twist      = glm::normalize( glm::quat( relative.w, relative.x, 0.0f, 0.0f ) );
        const glm::quat swing      = relative * glm::inverse( twist );
        const float     swingAngle = 2.0f * std::acos( std::clamp( std::abs( swing.w ), 0.0f, 1.0f ) );
        const float     twistAngle = std::abs( 2.0f * std::atan2( twist.x, twist.w ) );
        return { glm::degrees( swingAngle ),
                 glm::degrees( std::min( twistAngle, glm::two_pi<float>() - twistAngle ) ) };
    }

    void ExpectSameLocal( const Desert::Animation::BoneTransform& got,
                          const Desert::Animation::BoneTransform& want, const char* bone )
    {
        const float span = std::max( 1.0f, glm::length( want.Translation ) );
        EXPECT_LT( glm::length( got.Translation - want.Translation ) / span, 1e-4f ) << bone;
        EXPECT_GT( std::abs( glm::dot( got.Rotation, want.Rotation ) ), 1.0f - 1e-5f ) << bone;
        EXPECT_LT( glm::length( got.Scale - want.Scale ), 1e-4f ) << bone;
    }
} // namespace

TEST( RagdollRuntime, ChainDroppedOnAPlaneComesToRestOnIt )
{
    const Skeleton    skeleton = MakeChain();
    const RagdollDesc desc     = MakeChainRagdoll( skeleton );
    ASSERT_EQ( desc.Parts.size(), 3u );

    PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    AddFloor( world );
    auto ragdoll = world.CreateRagdoll( desc, { 0.0f, 80.0f, 0.0f }, glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ),
                                        RagdollMotion::Simulated );
    ASSERT_TRUE( ragdoll.IsSuccess() ) << ragdoll.GetError();
    EXPECT_EQ( world.GetRagdollCount(), 1u );
    EXPECT_EQ( world.GetBodyCount(), 4u ); // the floor and three parts

    std::vector<RagdollPartTransform> pose;
    for ( int frame = 0; frame < 600; ++frame )
        world.Step( kStep );
    world.GetRagdollPose( ragdoll.GetValue(), pose );
    ASSERT_EQ( pose.size(), 3u );
    for ( size_t i = 0; i < pose.size(); ++i )
    {
        EXPECT_GT( LowestPointCm( pose[i] ), -1.0f ) << "part " << i << " sank into the plane";
        EXPECT_LT( LowestPointCm( pose[i] ), 1.0f ) << "part " << i << " is not lying on the plane";
        EXPECT_LT( glm::length( world.GetLinearVelocity(
                        world.GetRagdollPartBody( ragdoll.GetValue(), static_cast<uint32_t>( i ) ) ) ),
                   5.0f )
             << "part " << i << " has not come to rest";
    }

    world.RemoveRagdoll( ragdoll.GetValue() );
    EXPECT_EQ( world.GetRagdollCount(), 0u );
    EXPECT_EQ( world.GetBodyCount(), 1u );
}

TEST( RagdollRuntime, JointLimitsHoldUnderGravityAndImpacts )
{
    const Skeleton    skeleton = MakeChain();
    const RagdollDesc desc     = MakeChainRagdoll( skeleton );
    ASSERT_EQ( desc.Parts.size(), 3u );

    PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    AddFloor( world );
    // Standing up (+Z -> +Y) on its root, then kicked: it topples and folds on the floor.
    const glm::quat up      = glm::angleAxis( -glm::half_pi<float>(), glm::vec3( 1.0f, 0.0f, 0.0f ) );
    auto            ragdoll = world.CreateRagdoll( desc, { 0.0f, 10.0f, 0.0f }, up, RagdollMotion::Simulated );
    ASSERT_TRUE( ragdoll.IsSuccess() ) << ragdoll.GetError();
    world.AddImpulse( world.GetRagdollPartBody( ragdoll.GetValue(), 2u ), { 2500.0f, 0.0f, 1200.0f } );
    world.AddImpulse( world.GetRagdollPartBody( ragdoll.GetValue(), 0u ), { -1500.0f, 0.0f, 0.0f } );

    float                             maxSwing = 0.0f;
    float                             maxTwist = 0.0f;
    std::vector<RagdollPartTransform> pose;
    for ( int frame = 0; frame < 300; ++frame )
    {
        world.Step( kStep );
        world.GetRagdollPose( ragdoll.GetValue(), pose );
        ASSERT_EQ( pose.size(), 3u );
        for ( size_t child = 1; child < 3; ++child )
        {
            // Both bind rotations are the entity's, so the bind-relative rotation is the relative one.
            const auto [swing, twist] = SwingTwistDegrees( pose[child - 1].Rotation, pose[child].Rotation );
            maxSwing                  = std::max( maxSwing, swing );
            maxTwist                  = std::max( maxTwist, twist );
        }
    }
    EXPECT_LE( maxSwing, kSwingLimit + kLimitSlack );
    EXPECT_LE( maxTwist, kTwistLimit + kLimitSlack );
    // Not vacuous: the fall did load the joints.
    EXPECT_GT( maxSwing, 0.5f * kSwingLimit );
}

TEST( RagdollRuntime, KinematicModeReproducesTheInputPoseExactly )
{
    const Skeleton    skeleton = MakeChain();
    const RagdollDesc desc     = MakeChainRagdoll( skeleton );
    ASSERT_EQ( desc.Parts.size(), 3u );

    auto bind = LocalPose::FromBindPose( skeleton );
    ASSERT_TRUE( bind.IsSuccess() );
    LocalPose animated = bind.GetValue();
    animated[1].Rotation =
         glm::angleAxis( glm::radians( 25.0f ), glm::normalize( glm::vec3( 1.0f, 0.3f, 0.0f ) ) );
    animated[2].Rotation = glm::angleAxis( glm::radians( -15.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );

    const glm::mat4 entity = glm::translate( glm::mat4( 1.0f ), { 40.0f, 120.0f, -30.0f } ) *
                             glm::mat4_cast( glm::angleAxis( 0.7f, glm::vec3( 0.0f, 1.0f, 0.0f ) ) );
    auto targets = RagdollPartsFromPose( desc, skeleton, animated, entity );
    ASSERT_TRUE( targets.IsSuccess() ) << targets.GetError();

    PhysicsWorld world;
    ASSERT_TRUE( world.Init( 981.0f ) );
    auto ragdoll =
         world.CreateRagdoll( desc, glm::vec3( entity[3] ), glm::angleAxis( 0.7f, glm::vec3( 0.0f, 1.0f, 0.0f ) ),
                              RagdollMotion::Kinematic );
    ASSERT_TRUE( ragdoll.IsSuccess() ) << ragdoll.GetError();
    EXPECT_EQ( world.GetRagdollMotion( ragdoll.GetValue() ), RagdollMotion::Kinematic );
    world.SetRagdollTarget( ragdoll.GetValue(), targets.GetValue() );

    std::vector<RagdollPartTransform> pose;
    for ( int frame = 0; frame < 30; ++frame ) // gravity has half a second to pull a body off its target
    {
        world.Step( kStep );
        world.GetRagdollPose( ragdoll.GetValue(), pose );
        ASSERT_EQ( pose.size(), 3u );
        for ( size_t i = 0; i < 3; ++i )
        {
            EXPECT_LT( glm::length( pose[i].Position - targets.GetValue()[i].Position ), 1e-3f ) << "part " << i;
            EXPECT_GT( std::abs( glm::dot( pose[i].Rotation, targets.GetValue()[i].Rotation ) ), 1.0f - 1e-5f )
                 << "part " << i;
        }
    }

    // And back into bones: the kinematic bodies give the animated locals again.
    auto overrides = RagdollBoneOverrides( desc, skeleton, bind.GetValue(), entity, pose );
    ASSERT_TRUE( overrides.IsSuccess() ) << overrides.GetError();
    LocalPose                                     written = bind.GetValue();
    Desert::Animation::ComponentPose              view( skeleton, written );
    std::vector<Desert::Animation::BoneTransform> scratch;
    ASSERT_TRUE(
         Desert::Animation::ApplyBoneOverrides( skeleton, written, view, overrides.GetValue(), 1.0f, scratch )
              .IsSuccess() );
    for ( uint32_t bone = 0; bone < 3; ++bone )
        ExpectSameLocal( written[bone], animated[bone], skeleton.GetBones()[bone].Name.c_str() );
}

TEST( RagdollRuntime, PoseWriteBackRoundTripsBoneLocalsUnderAHundredthScaledRoot )
{
    // An FBX rig: the root carries 0.01 (and a turn), every child translation is in the rig's own units
    // (2000 = 20 cm after the root's scale). twist(2) has no body: it keeps its animated local.
    const glm::mat4 root =
         glm::rotate( glm::mat4( 1.0f ), -glm::half_pi<float>(), glm::vec3( 1.0f, 0.0f, 0.0f ) ) *
         glm::scale( glm::mat4( 1.0f ), glm::vec3( 0.01f ) );
    std::vector<BoneInfo> bones;
    bones.push_back( MakeBone( "root", std::nullopt, root ) );
    bones.push_back( MakeBone( "pelvis", 0u, glm::translate( glm::mat4( 1.0f ), { 0.0f, 0.0f, 9000.0f } ) ) );
    bones.push_back( MakeBone( "twist", 1u, glm::translate( glm::mat4( 1.0f ), { 0.0f, 0.0f, 2000.0f } ) ) );
    bones.push_back( MakeBone( "head", 2u, glm::translate( glm::mat4( 1.0f ), { 0.0f, 0.0f, 1500.0f } ) ) );
    bones.push_back( MakeBone( "hand", 1u, glm::translate( glm::mat4( 1.0f ), { 2500.0f, 0.0f, 1000.0f } ) ) );
    const Skeleton skeleton( std::move( bones ) );

    RagdollDesc desc;
    for ( const auto& [bone, index, parent] :
          { std::tuple{ "pelvis", 1u, -1 }, std::tuple{ "head", 3u, 0 }, std::tuple{ "hand", 4u, 0 } } )
    {
        RagdollPartDesc part;
        part.Bone         = bone;
        part.SkeletonBone = index;
        part.Parent       = parent;
        desc.Parts.push_back( part );
    }

    auto bind = LocalPose::FromBindPose( skeleton );
    ASSERT_TRUE( bind.IsSuccess() );
    LocalPose animated   = bind.GetValue();
    animated[1].Rotation = glm::angleAxis( 0.4f, glm::normalize( glm::vec3( 0.2f, 1.0f, 0.1f ) ) );
    animated[1].Translation += glm::vec3( 300.0f, -120.0f, 50.0f );
    animated[3].Rotation = glm::angleAxis( -0.6f, glm::vec3( 1.0f, 0.0f, 0.0f ) );
    animated[4].Rotation = glm::angleAxis( 1.1f, glm::normalize( glm::vec3( 0.0f, 0.3f, 1.0f ) ) );

    const glm::mat4 entity = glm::translate( glm::mat4( 1.0f ), { -250.0f, 5.0f, 75.0f } ) *
                             glm::mat4_cast( glm::angleAxis( 2.0f, glm::vec3( 0.0f, 1.0f, 0.0f ) ) );
    auto bodies = RagdollPartsFromPose( desc, skeleton, animated, entity );
    ASSERT_TRUE( bodies.IsSuccess() ) << bodies.GetError();

    // The bodies are in world centimetres whatever the root's scale: pelvis sits 90 cm out of the root.
    Desert::Animation::ComponentPose animatedView( skeleton, animated );
    EXPECT_NEAR( glm::length( bodies.GetValue()[0].Position - glm::vec3( entity * animatedView.Get( 0 )[3] ) ),
                 glm::length( 0.01f * ( glm::vec3( 0.0f, 0.0f, 9000.0f ) + animated[1].Translation -
                                        glm::vec3( 0.0f, 0.0f, 9000.0f ) + glm::vec3( 0.0f, 0.0f, 9000.0f ) ) ),
                 1e-2f );

    // Write the bodies into a pose that starts somewhere else (the bind): every bone with a body gets its
    // animated local back, and the body-less twist keeps the local it had.
    auto overrides = RagdollBoneOverrides( desc, skeleton, bind.GetValue(), entity, bodies.GetValue() );
    ASSERT_TRUE( overrides.IsSuccess() ) << overrides.GetError();
    LocalPose                                     written = bind.GetValue();
    Desert::Animation::ComponentPose              view( skeleton, written );
    std::vector<Desert::Animation::BoneTransform> scratch;
    ASSERT_TRUE(
         Desert::Animation::ApplyBoneOverrides( skeleton, written, view, overrides.GetValue(), 1.0f, scratch )
              .IsSuccess() );
    for ( uint32_t bone = 0; bone < 5; ++bone )
        ExpectSameLocal( written[bone], animated[bone], skeleton.GetBones()[bone].Name.c_str() );
    EXPECT_LT( glm::length( written[1].Scale - glm::vec3( 1.0f ) ), 1e-4f )
         << "the root's 0.01 leaked into a child";

    // A scaled entity is refused by name, not simulated with shapes of the wrong size.
    auto scaled = RagdollPartsFromPose( desc, skeleton, animated, glm::scale( entity, glm::vec3( 2.0f ) ) );
    ASSERT_FALSE( scaled.IsSuccess() );
    EXPECT_NE( scaled.GetError().find( "scale" ), std::string::npos );
}
