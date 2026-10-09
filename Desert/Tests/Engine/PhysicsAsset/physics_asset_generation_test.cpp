// "Create Physics Asset" from a skeletal mesh (RAG1c): which bones get a body, the capsule fitted along the bone,
// joints to the nearest bodied ancestor. The rig is built in memory: Y up, centimetres.

#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Physics/PhysicsAssetGeneration.hpp>
#include <Engine/Physics/RagdollDesc.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace Desert::Physics;
using Desert::SkinnedVertex;
using Desert::Animation::BoneInfo;
using Desert::Animation::Skeleton;

namespace
{
    enum Bone : uint32_t
    {
        Root = 0,
        Pelvis,
        Spine,
        NeckTiny,
        Head,
        IkTarget,
    };

    BoneInfo MakeBone( const char* name, std::optional<uint32_t> parent, const glm::vec3& offset )
    {
        BoneInfo b;
        b.Name               = name;
        b.ParentBoneID       = parent;
        b.LocalBindTransform = glm::translate( glm::mat4( 1.0f ), offset );
        return b;
    }

    // root (no vertices) -> pelvis y100 -> spine y120 -> neck_tiny y150 -> head y155; ik_target under root.
    Skeleton MakeRig()
    {
        std::vector<BoneInfo> bones;
        bones.push_back( MakeBone( "root", std::nullopt, { 0.0f, 0.0f, 0.0f } ) );
        bones.push_back( MakeBone( "pelvis", Root, { 0.0f, 100.0f, 0.0f } ) );
        bones.push_back( MakeBone( "spine", Pelvis, { 0.0f, 20.0f, 0.0f } ) );
        bones.push_back( MakeBone( "neck_tiny", Spine, { 0.0f, 30.0f, 0.0f } ) );
        bones.push_back( MakeBone( "head", NeckTiny, { 0.0f, 5.0f, 0.0f } ) );
        bones.push_back( MakeBone( "ik_target", Root, { 0.0f, 0.0f, 50.0f } ) );
        return Skeleton( std::move( bones ) );
    }

    // Rings of 8 vertices of @p radius around +Y from y0 to y1, every 5 cm and at both ends. @p secondary takes
    // a lighter second weight, so dominance (not "first influence") is what assigns the vertex.
    void AddTube( std::vector<SkinnedVertex>& out, uint32_t bone, float y0, float y1, float radius,
                  std::optional<uint32_t> secondary = std::nullopt )
    {
        std::vector<float> heights;
        for ( float y = y0; y < y1; y += 5.0f )
            heights.push_back( y );
        heights.push_back( y1 );
        for ( const float y : heights )
            for ( int i = 0; i < 8; ++i )
            {
                const float   a = glm::two_pi<float>() * static_cast<float>( i ) / 8.0f;
                SkinnedVertex v;
                v.StaticVertex.Position = { radius * std::cos( a ), y, radius * std::sin( a ) };
                if ( secondary )
                {
                    v.BoneIDs     = { *secondary, bone, 0u, 0u };
                    v.BoneWeights = { 0.3f, 0.7f, 0.0f, 0.0f };
                }
                else
                {
                    v.BoneIDs     = { bone, 0u, 0u, 0u };
                    v.BoneWeights = { 1.0f, 0.0f, 0.0f, 0.0f };
                }
                out.push_back( v );
            }
    }

    std::vector<SkinnedVertex> MakeMesh()
    {
        std::vector<SkinnedVertex> mesh;
        AddTube( mesh, Pelvis, 100.0f, 120.0f, 8.0f );
        AddTube( mesh, Spine, 120.0f, 150.0f, 6.0f, Pelvis );
        AddTube( mesh, NeckTiny, 150.0f, 151.0f, 0.3f ); // spans 1 cm: under the 5 cm minimum
        AddTube( mesh, Head, 155.0f, 175.0f, 9.0f );
        return mesh;
    }

    const PhysicsAssetBody* FindBody( const PhysicsAssetData& data, const std::string& bone )
    {
        for ( const auto& b : data.Bodies )
            if ( b.Bone == bone )
                return &b;
        return nullptr;
    }

    const PhysicsAssetConstraint* FindJoint( const PhysicsAssetData& data, const std::string& child )
    {
        for ( const auto& c : data.Constraints )
            if ( c.ChildBone == child )
                return &c;
        return nullptr;
    }

    const Desert::Common::Content::AssetGuid kSkeletonGuid{ 0x5EE1ull, 0x0003ull };
} // namespace

TEST( PhysicsAssetGeneration, KnownRigGetsABodyPerWeightedBoneAndTinyBonesAreSkipped )
{
    const Skeleton rig  = MakeRig();
    const auto     mesh = MakeMesh();
    const auto     made = GeneratePhysicsAsset( rig, mesh, kSkeletonGuid );
    ASSERT_TRUE( made.IsSuccess() ) << made.GetError();
    const PhysicsAssetData& data = made.GetValue();

    EXPECT_EQ( data.Skeleton, kSkeletonGuid );
    ASSERT_EQ( data.Bodies.size(), 3u );
    EXPECT_NE( FindBody( data, "pelvis" ), nullptr );
    EXPECT_NE( FindBody( data, "spine" ), nullptr );
    EXPECT_NE( FindBody( data, "head" ), nullptr );
    EXPECT_EQ( FindBody( data, "neck_tiny" ), nullptr ) << "a bone spanning 1 cm got a body";
    EXPECT_EQ( FindBody( data, "root" ), nullptr ) << "a bone with no vertices got a body";
    EXPECT_EQ( FindBody( data, "ik_target" ), nullptr );

    // The tiny bone's vertices thicken its parent rather than vanish: the spine reaches y = 151.
    const PhysicsAssetBody* spine = FindBody( data, "spine" );
    ASSERT_NE( spine, nullptr );
    EXPECT_NEAR( spine->Center.y + 0.5f * spine->Length + spine->Radius, 31.0f, 1e-3f );

    // What the generator writes is what the runtime accepts.
    EXPECT_TRUE( ValidatePhysicsAsset( data, rig ).empty() );
    EXPECT_TRUE( BuildRagdollDesc( data, rig ).IsSuccess() );

    PhysicsAssetGenerationSettings coarse;
    coarse.MinBoneSizeCm = 0.5f;
    const auto fine      = GeneratePhysicsAsset( rig, mesh, kSkeletonGuid, coarse );
    ASSERT_TRUE( fine.IsSuccess() );
    EXPECT_NE( FindBody( fine.GetValue(), "neck_tiny" ), nullptr ) << "the minimum size is not the setting's";
}

TEST( PhysicsAssetGeneration, CapsuleRunsAlongTheBoneAndEnclosesItsVertices )
{
    const auto made = GeneratePhysicsAsset( MakeRig(), MakeMesh(), kSkeletonGuid );
    ASSERT_TRUE( made.IsSuccess() ) << made.GetError();

    for ( const auto& body : made.GetValue().Bodies )
    {
        EXPECT_EQ( body.Shape, PhysicsBodyShape::Capsule ) << body.Bone;
        const glm::vec3 axis = body.Rotation * glm::vec3( 0.0f, 0.0f, 1.0f );
        EXPECT_GT( glm::dot( axis, glm::vec3( 0.0f, 1.0f, 0.0f ) ), 0.9999f )
             << body.Bone << ": axis off the bone";
    }

    const PhysicsAssetBody* pelvis = FindBody( made.GetValue(), "pelvis" );
    ASSERT_NE( pelvis, nullptr );
    EXPECT_NEAR( pelvis->Radius, 8.0f, 1e-3f );
    EXPECT_NEAR( pelvis->Length, 20.0f - 2.0f * 8.0f, 1e-3f );
    EXPECT_NEAR( glm::length( pelvis->Center - glm::vec3( 0.0f, 10.0f, 0.0f ) ), 0.0f, 1e-3f );

    // A leaf bone aims at its vertices: the head's capsule spans y 155..175.
    const PhysicsAssetBody* head = FindBody( made.GetValue(), "head" );
    ASSERT_NE( head, nullptr );
    EXPECT_NEAR( head->Radius, 9.0f, 1e-3f );
    EXPECT_NEAR( head->Length, 2.0f, 1e-3f );
    EXPECT_NEAR( head->Center.y, 10.0f, 1e-3f );
}

TEST( PhysicsAssetGeneration, JointsJoinEachBodyToItsNearestBodiedAncestor )
{
    const auto made = GeneratePhysicsAsset( MakeRig(), MakeMesh(), kSkeletonGuid );
    ASSERT_TRUE( made.IsSuccess() ) << made.GetError();
    const PhysicsAssetData& data = made.GetValue();

    ASSERT_EQ( data.Constraints.size(), 2u );
    EXPECT_EQ( FindJoint( data, "pelvis" ), nullptr ) << "the root body is jointed to nothing";

    const PhysicsAssetConstraint* spine = FindJoint( data, "spine" );
    ASSERT_NE( spine, nullptr );
    EXPECT_EQ( spine->ParentBone, "pelvis" );

    const PhysicsAssetConstraint* head = FindJoint( data, "head" );
    ASSERT_NE( head, nullptr );
    EXPECT_EQ( head->ParentBone, "spine" ) << "the head must skip the body-less neck_tiny";

    for ( const auto& c : data.Constraints )
    {
        EXPECT_FLOAT_EQ( c.Swing1LimitDegrees, 45.0f );
        EXPECT_FLOAT_EQ( c.Swing2LimitDegrees, 45.0f );
        EXPECT_FLOAT_EQ( c.TwistLimitDegrees, 45.0f );
        const glm::vec3 twist = c.Rotation * glm::vec3( 1.0f, 0.0f, 0.0f );
        EXPECT_GT( glm::dot( twist, glm::vec3( 0.0f, 1.0f, 0.0f ) ), 0.9999f )
             << c.ChildBone << ": twist off the bone";
    }
}

TEST( PhysicsAssetGeneration, RefusesAMeshWithoutVerticesOrBodies )
{
    const Skeleton rig = MakeRig();
    EXPECT_FALSE( GeneratePhysicsAsset( rig, {}, kSkeletonGuid ).IsSuccess() );

    std::vector<SkinnedVertex> crumbs;
    AddTube( crumbs, Head, 155.0f, 155.5f, 0.2f );
    EXPECT_FALSE( GeneratePhysicsAsset( rig, crumbs, kSkeletonGuid ).IsSuccess() );
}
