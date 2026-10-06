#include <gtest/gtest.h>

#include <Editor/Panels/AnimationEditor/SkeletonTree.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>
#include <Engine/ECS/System/SystemRules.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <string>
#include <vector>

using namespace Desert;
using namespace Desert::Editor;

namespace
{
    // root -> spine -> { upperarm_l -> lowerarm_l, upperarm_r -> lowerarm_r }
    Animation::Skeleton MakeSkeleton()
    {
        std::vector<Animation::BoneInfo> bones( 6 );
        const char* names[]   = { "root", "spine", "upperarm_l", "lowerarm_l", "upperarm_r", "lowerarm_r" };
        const int   parents[] = { -1, 0, 1, 2, 1, 4 };
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            bones[i].Name = names[i];
            if ( parents[i] >= 0 )
                bones[i].ParentBoneID = static_cast<uint32_t>( parents[i] );
        }
        return Animation::Skeleton( std::move( bones ) );
    }

    std::vector<std::string> Names( const Animation::Skeleton& s, const std::vector<SkeletonTreeRow>& rows )
    {
        std::vector<std::string> out;
        out.reserve( rows.size() );
        for ( const auto& row : rows )
            out.push_back( std::string( row.Depth, '.' ) + s.GetBones()[row.Bone].Name );
        return out;
    }
} // namespace

TEST( SkeletonTree, NameSelectsItsIndexAndTheIndexNamesIt )
{
    const auto skeleton = MakeSkeleton();
    for ( uint32_t i = 0; i < skeleton.GetBones().size(); ++i )
    {
        const auto found = BoneByName( skeleton, skeleton.GetBones()[i].Name );
        ASSERT_TRUE( found.has_value() ) << skeleton.GetBones()[i].Name;
        EXPECT_EQ( found.value_or( ~0U ), i );
    }
    EXPECT_FALSE( BoneByName( skeleton, "lowerarm" ).has_value() ) << "a prefix is not a bone";
    EXPECT_FALSE( BoneByName( skeleton, "LOWERARM_L" ).has_value() ) << "names are exact";
}

TEST( SkeletonTree, RowsAreDepthFirstInIndexOrder )
{
    const auto skeleton = MakeSkeleton();
    const auto rows     = BuildSkeletonTreeRows( skeleton, "", {} );
    EXPECT_EQ( Names( skeleton, rows ),
               ( std::vector<std::string>{ "root", ".spine", "..upperarm_l", "...lowerarm_l", "..upperarm_r",
                                           "...lowerarm_r" } ) );
    EXPECT_TRUE( rows[0].HasChildren );
    EXPECT_FALSE( rows[3].HasChildren );
}

TEST( SkeletonTree, CollapsedBoneHidesItsSubtreeOnly )
{
    const auto        skeleton = MakeSkeleton();
    std::vector<bool> collapsed( 6, false );
    collapsed[2]    = true; // upperarm_l
    const auto rows = BuildSkeletonTreeRows( skeleton, "", collapsed );
    EXPECT_EQ( Names( skeleton, rows ),
               ( std::vector<std::string>{ "root", ".spine", "..upperarm_l", "..upperarm_r", "...lowerarm_r" } ) );
}

TEST( SkeletonTree, FilterKeepsMatchesAndTheirAncestorsIgnoringCollapse )
{
    const auto              skeleton = MakeSkeleton();
    const std::vector<bool> collapsed( 6, true );
    const auto              rows = BuildSkeletonTreeRows( skeleton, "LowerArm_L", collapsed );
    EXPECT_EQ( Names( skeleton, rows ),
               ( std::vector<std::string>{ "root", ".spine", "..upperarm_l", "...lowerarm_l" } ) );
    EXPECT_FALSE( rows[0].Matches );
    EXPECT_TRUE( rows[3].Matches );
    EXPECT_TRUE( BuildSkeletonTreeRows( skeleton, "nope", {} ).empty() );
}

TEST( SkeletonTree, DecomposeReadsLocationRotationScale )
{
    const glm::mat4 m = glm::translate( glm::mat4( 1.0f ), glm::vec3( 1.0f, 2.0f, 3.0f ) ) *
                        glm::rotate( glm::mat4( 1.0f ), glm::radians( 90.0f ), glm::vec3( 0.0f, 0.0f, 1.0f ) ) *
                        glm::scale( glm::mat4( 1.0f ), glm::vec3( 2.0f ) );
    const auto rows = DecomposeBoneTransform( m );
    EXPECT_NEAR( rows.Location.y, 2.0f, 1e-4f );
    EXPECT_NEAR( rows.RotationDegrees.z, 90.0f, 1e-3f );
    EXPECT_NEAR( rows.Scale.x, 2.0f, 1e-4f );
}

// ── ANIM-FIX5a: sockets live on the skeleton; an attachment names one ─────────────────────────────────────

TEST( SkeletonSockets, AnAttachmentNamingASocketFollowsItsBoneAndItsTransformAndABoneNameStillWorks )
{
    auto                        skeleton = MakeSkeleton();
    Animation::SkeletonSocket   grip{ "hand_r_grip", "lowerarm_r", glm::vec3( 0.0F, 10.0F, 0.0F ) };
    ASSERT_TRUE( skeleton.SetSockets( { grip } ) );

    const auto socket = ECS::Rules::ResolveAttachPoint( skeleton, "hand_r_grip" );
    ASSERT_TRUE( socket.has_value() );
    EXPECT_EQ( socket->Bone, 5u );
    EXPECT_FLOAT_EQ( socket->SocketLocal[3][1], 10.0F );

    // The attachment's own offset composes ON TOP of the socket: bone * socket * offset.
    const glm::mat4 world = ECS::Rules::SocketLocalTransform( glm::mat4( 1.0F ), glm::mat4( 1.0F ) * socket->SocketLocal,
                                                             glm::vec3( 1.0F, 0.0F, 0.0F ), glm::vec3( 0.0F ),
                                                             glm::vec3( 1.0F ) );
    EXPECT_FLOAT_EQ( world[3][0], 1.0F );
    EXPECT_FLOAT_EQ( world[3][1], 10.0F );

    const auto bone = ECS::Rules::ResolveAttachPoint( skeleton, "spine" );
    ASSERT_TRUE( bone.has_value() );
    EXPECT_EQ( bone->Bone, 1u );
    EXPECT_EQ( bone->SocketLocal, glm::mat4( 1.0F ) );
    EXPECT_FALSE( ECS::Rules::ResolveAttachPoint( skeleton, "nowhere" ).has_value() );

    // A socket on a bone the rig lacks is refused by name; the old set stays.
    const auto refused = skeleton.SetSockets( { Animation::SkeletonSocket{ "bad", "tail" } } );
    ASSERT_FALSE( refused );
    EXPECT_NE( refused.GetError().find( "'tail'" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( skeleton.FindSocket( "hand_r_grip" ), nullptr );
}

TEST( SkeletonSockets, SocketsAndBoneMasksSurviveTheSkel4RoundTrip )
{
    Assets::Serialization::SkeletonAssetData data;
    data.Bones = MakeSkeleton().GetBones();
    data.Sockets.push_back( Animation::SkeletonSocket{ "head_hat", "spine", glm::vec3( 0.0F, 0.0F, 5.0F ),
                                                       glm::quat( 0.0F, 0.0F, 1.0F, 0.0F ), glm::vec3( 2.0F ) } );
    data.BoneMasks.push_back(
         Animation::BoneMask{ "LeftArm", { Animation::BoneMaskEntry{ "upperarm_l", 0.75F, false } } } );

    const auto back = Assets::Serialization::ReadSkeletonJson( Assets::Serialization::WriteSkeletonJson( data ) );
    ASSERT_TRUE( back ) << back.GetError();
    ASSERT_EQ( back.GetValue().Sockets.size(), 1u );
    const auto& socket = back.GetValue().Sockets[0];
    EXPECT_EQ( socket.Name, "head_hat" );
    EXPECT_EQ( socket.Bone, "spine" );
    EXPECT_FLOAT_EQ( socket.Translation.z, 5.0F );
    EXPECT_FLOAT_EQ( socket.Rotation.z, 1.0F );
    EXPECT_FLOAT_EQ( socket.Scale.x, 2.0F );
    ASSERT_EQ( back.GetValue().BoneMasks.size(), 1u );
    ASSERT_EQ( back.GetValue().BoneMasks[0].Entries.size(), 1u );
    EXPECT_EQ( back.GetValue().BoneMasks[0].Entries[0].Bone, "upperarm_l" );
    EXPECT_FLOAT_EQ( back.GetValue().BoneMasks[0].Entries[0].Weight, 0.75F );
    EXPECT_FALSE( back.GetValue().BoneMasks[0].Entries[0].IncludeDescendants );
}
