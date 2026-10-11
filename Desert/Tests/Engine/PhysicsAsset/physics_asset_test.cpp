// RAG1a: the physics asset (`.dephysasset`, UE UPhysicsAsset) — its bytes, its validation against a skeleton,
// and the pure conversion into what Jolt's RagdollSettings is built from (no PhysicsSystem, no device).

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Physics/PhysicsAssetFormat.hpp>
#include <Engine/Physics/RagdollDesc.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace Desert::Physics;

namespace
{
    Desert::Animation::BoneInfo Bone( const char* name, std::optional<uint32_t> parent, glm::vec3 offsetCm )
    {
        Desert::Animation::BoneInfo b;
        b.Name               = name;
        b.ParentBoneID       = parent;
        b.LocalBindTransform = glm::translate( glm::mat4( 1.0f ), offsetCm );
        return b;
    }

    // pelvis(0) -> spine(1) -> neck_twist(2, no body) -> head(3); pelvis -> thigh_l(4). The head's parent BODY is
    // the spine's: the twist bone between them carries none.
    Desert::Animation::Skeleton MakeRig()
    {
        std::vector<Desert::Animation::BoneInfo> bones;
        bones.push_back( Bone( "pelvis", std::nullopt, { 0.0f, 100.0f, 0.0f } ) );
        bones.push_back( Bone( "spine", 0u, { 0.0f, 20.0f, 0.0f } ) );
        bones.push_back( Bone( "neck_twist", 1u, { 0.0f, 10.0f, 0.0f } ) );
        bones.push_back( Bone( "head", 2u, { 0.0f, 15.0f, 0.0f } ) );
        bones.push_back( Bone( "thigh_l", 0u, { 10.0f, -5.0f, 0.0f } ) );
        return Desert::Animation::Skeleton( std::move( bones ) );
    }

    PhysicsAssetData MakeAsset()
    {
        PhysicsAssetData d;
        d.Guid     = { 0xA5A5ull, 0x0001ull };
        d.Skeleton = { 0x5CE1ull, 0x0002ull };

        PhysicsAssetBody pelvis;
        pelvis.Bone       = "pelvis";
        pelvis.Shape      = PhysicsBodyShape::Box;
        pelvis.BoxExtents = { 30.0f, 20.0f, 15.0f };
        pelvis.MassKg     = 12.0f;
        PhysicsAssetBody spine;
        spine.Bone     = "spine";
        spine.Shape    = PhysicsBodyShape::Capsule;
        spine.Radius   = 6.0f;
        spine.Length   = 30.0f;
        spine.Center   = { 0.0f, 8.0f, 0.0f };
        spine.Rotation = glm::angleAxis( glm::half_pi<float>(), glm::vec3( 1.0f, 0.0f, 0.0f ) );
        PhysicsAssetBody head;
        head.Bone               = "head";
        head.Shape              = PhysicsBodyShape::Sphere;
        head.Radius             = 10.0f;
        head.DensityGramsPerCm3 = 1.0f;
        PhysicsAssetBody thigh;
        thigh.Bone   = "thigh_l";
        thigh.Shape  = PhysicsBodyShape::Capsule;
        thigh.Radius = 7.0f;
        thigh.Length = 35.0f;
        thigh.MassKg = 8.0f;
        d.Bodies     = { pelvis, spine, head, thigh };

        PhysicsAssetConstraint lowerBack;
        lowerBack.ParentBone         = "pelvis";
        lowerBack.ChildBone          = "spine";
        lowerBack.Swing1LimitDegrees = 30.0f;
        lowerBack.Swing2LimitDegrees = 20.0f;
        lowerBack.TwistLimitDegrees  = 15.0f;
        PhysicsAssetConstraint neck;
        neck.ParentBone         = "spine";
        neck.ChildBone          = "head";
        neck.Position           = { 0.0f, 2.0f, 0.0f };
        neck.Swing1LimitDegrees = 45.0f;
        neck.Swing2LimitDegrees = 35.0f;
        neck.TwistLimitDegrees  = 60.0f;
        PhysicsAssetConstraint hip;
        hip.ParentBone = "pelvis";
        hip.ChildBone  = "thigh_l";
        d.Constraints  = { lowerBack, neck, hip };
        return d;
    }

    bool AnyIssueMentions( const std::vector<std::string>& issues, const std::string& a, const std::string& b )
    {
        return std::any_of( issues.begin(), issues.end(), [&]( const std::string& s )
                            { return s.find( a ) != std::string::npos && s.find( b ) != std::string::npos; } );
    }

    const RagdollPartDesc& PartOf( const RagdollDesc& desc, const std::string& bone )
    {
        const auto it = std::find_if( desc.Parts.begin(), desc.Parts.end(),
                                      [&]( const RagdollPartDesc& p ) { return p.Bone == bone; } );
        EXPECT_NE( it, desc.Parts.end() ) << bone;
        return *it;
    }
} // namespace

TEST( PhysicsAsset, FileRoundTripsAndTheSecondSaveIsByteIdentical )
{
    const PhysicsAssetData authored = MakeAsset();
    auto                   first    = EncodePhysicsAsset( authored );
    ASSERT_TRUE( first ) << first.GetError();

    auto decoded = DecodePhysicsAsset( first.GetValue() );
    ASSERT_TRUE( decoded ) << decoded.GetError();
    EXPECT_EQ( decoded.GetValue(), authored );

    auto second = EncodePhysicsAsset( decoded.GetValue() );
    ASSERT_TRUE( second ) << second.GetError();
    EXPECT_EQ( second.GetValue(), first.GetValue() );
}

TEST( PhysicsAsset, FileRefusesANullGuidATruncationAndTrailingBytes )
{
    PhysicsAssetData noGuid = MakeAsset();
    noGuid.Guid             = {};
    EXPECT_FALSE( EncodePhysicsAsset( noGuid ) );

    std::vector<unsigned char> payload = EncodePhysicsAssetPayload( MakeAsset() );
    std::vector<unsigned char> cut( payload.begin(), payload.end() - 3 );
    EXPECT_FALSE( DecodePhysicsAssetPayload( cut ) );
    payload.push_back( 0u );
    EXPECT_FALSE( DecodePhysicsAssetPayload( payload ) );
}

TEST( PhysicsAsset, ValidAssetHasNoIssues )
{
    const auto issues = ValidatePhysicsAsset( MakeAsset(), MakeRig() );
    EXPECT_TRUE( issues.empty() ) << ( issues.empty() ? std::string() : issues.front() );
}

TEST( PhysicsAsset, ValidationNamesUnknownBoneNonParentJointAndNegativeExtent )
{
    PhysicsAssetData bad       = MakeAsset();
    bad.Bodies[0].BoxExtents.y = -5.0f; // pelvis box
    PhysicsAssetBody ghost;
    ghost.Bone = "tail_01";
    bad.Bodies.push_back( ghost );
    PhysicsAssetConstraint skip; // head's parent body is the spine's, not the pelvis'
    skip.ParentBone = "pelvis";
    skip.ChildBone  = "head";
    bad.Constraints.push_back( skip );

    const auto issues = ValidatePhysicsAsset( bad, MakeRig() );
    EXPECT_TRUE( AnyIssueMentions( issues, "'tail_01'", "does not have" ) );
    EXPECT_TRUE( AnyIssueMentions( issues, "'pelvis' -> 'head'", "not parent and child" ) );
    EXPECT_TRUE( AnyIssueMentions( issues, "'pelvis'", "box extents" ) );
    EXPECT_TRUE( AnyIssueMentions( issues, "'head'", "more than one constraint" ) );

    auto refused = BuildRagdollDesc( bad, MakeRig() );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "tail_01" ), std::string::npos );
}

TEST( PhysicsAsset, RagdollPartsFollowTheSkeleton )
{
    auto built = BuildRagdollDesc( MakeAsset(), MakeRig() );
    ASSERT_TRUE( built ) << built.GetError();
    const RagdollDesc& desc = built.GetValue();

    ASSERT_EQ( desc.Parts.size(), 4u ); // the twist bone carries no body, so no part
    for ( size_t i = 0; i < desc.Parts.size(); ++i )
        EXPECT_LT( desc.Parts[i].Parent, static_cast<int32_t>( i ) ) << desc.Parts[i].Bone;

    EXPECT_EQ( PartOf( desc, "pelvis" ).Parent, -1 );
    EXPECT_EQ( desc.Parts[PartOf( desc, "spine" ).Parent].Bone, "pelvis" );
    EXPECT_EQ( desc.Parts[PartOf( desc, "head" ).Parent].Bone, "spine" );
    EXPECT_EQ( desc.Parts[PartOf( desc, "thigh_l" ).Parent].Bone, "pelvis" );
    EXPECT_FALSE( PartOf( desc, "pelvis" ).HasConstraint );
    EXPECT_TRUE( PartOf( desc, "head" ).HasConstraint );

    // The body sits at the bone's component-space bind position: 100 + 20 + 10 + 15 cm up for the head.
    EXPECT_FLOAT_EQ( PartOf( desc, "head" ).Position.y, 145.0f * kJoltUnitsPerCentimetre );
}

TEST( PhysicsAsset, RagdollLimitsAreTheAuthoredDegreesInRadians )
{
    auto built = BuildRagdollDesc( MakeAsset(), MakeRig() );
    ASSERT_TRUE( built ) << built.GetError();
    const RagdollConstraintDesc& neck = PartOf( built.GetValue(), "head" ).ToParent;

    EXPECT_FLOAT_EQ( neck.NormalHalfConeAngle, 45.0f * glm::pi<float>() / 180.0f ); // Swing1
    EXPECT_FLOAT_EQ( neck.PlaneHalfConeAngle, 35.0f * glm::pi<float>() / 180.0f );  // Swing2
    EXPECT_FLOAT_EQ( neck.TwistMinAngle, -60.0f * glm::pi<float>() / 180.0f );
    EXPECT_FLOAT_EQ( neck.TwistMaxAngle, 60.0f * glm::pi<float>() / 180.0f );
    // The joint frame is the authored offset in the child bone's space: 2 cm above the head bone.
    EXPECT_FLOAT_EQ( neck.Position1.y, 147.0f * kJoltUnitsPerCentimetre );
    EXPECT_EQ( neck.Position1, neck.Position2 );
}

TEST( PhysicsAsset, RagdollShapesAreInJoltUnits )
{
    auto built = BuildRagdollDesc( MakeAsset(), MakeRig() );
    ASSERT_TRUE( built ) << built.GetError();
    const RagdollDesc& desc = built.GetValue();

    const RagdollPartDesc& spine = PartOf( desc, "spine" );
    EXPECT_FLOAT_EQ( spine.Radius, 6.0f * kJoltUnitsPerCentimetre );
    EXPECT_FLOAT_EQ( spine.HalfHeight, 15.0f * kJoltUnitsPerCentimetre ); // Length 30 cm between the caps
    EXPECT_FLOAT_EQ( spine.ShapeOffset.y, 8.0f * kJoltUnitsPerCentimetre );
    // Authored along the body's Z, turned so Jolt's Y-built capsule lies along the authored axis.
    const glm::vec3 capsuleAxis = spine.ShapeRotation * glm::vec3( 0.0f, 1.0f, 0.0f );
    const glm::vec3 authored    = MakeAsset().Bodies[1].Rotation * glm::vec3( 0.0f, 0.0f, 1.0f );
    EXPECT_NEAR( glm::dot( capsuleAxis, authored ), 1.0f, 1e-5f );

    const RagdollPartDesc& pelvis = PartOf( desc, "pelvis" );
    EXPECT_FLOAT_EQ( pelvis.HalfExtents.x, 15.0f * kJoltUnitsPerCentimetre );
    EXPECT_FLOAT_EQ( pelvis.MassKg, 12.0f );
    // No mass override: a 10 cm sphere at 1 g/cm3 is 4/3 pi 1000 g.
    EXPECT_NEAR( PartOf( desc, "head" ).MassKg, 4.0f / 3.0f * glm::pi<float>(), 1e-4f );
}
