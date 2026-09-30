// THE SKELETON IS REFERENCED BY GUID (SKEL-TREE, contract: Engine/Animation/SkeletonReference.hpp).
//
// What these tests pin, and why each one: the previous identity (a hash of the bones) let a re-cooked rig
// orphan every mesh and clip that meant it, and let two exports of one rig be two identities. The GUID rule
// must therefore IGNORE bones entirely (structural: ClipPlaysOnMesh takes no bone input at all), CompatibleSkeletons must be one-directional, the assignment check is where names live, and
// migration must refuse rather than guess.

#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/SkeletonReference.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using Desert::Animation::BoneInfo;
using Desert::Animation::CheckSkeletonAssignment;
using Desert::Animation::ClipPlaysOnMesh;
using Desert::Animation::FindSkeletonsBySignature;
using Desert::Animation::MigrateSkeletonReference;
using Desert::Animation::RequiredBone;
using Desert::Animation::Skeleton;
using Desert::Animation::SkeletonAssetRef;
using Desert::Animation::SkeletonCandidate;
using Common::Content::AssetGuid;

namespace
{
    const AssetGuid kMannequin{ 0x1111U, 0x0001U };
    const AssetGuid kMixamo{ 0x2222U, 0x0002U };
    const AssetGuid kFox{ 0x3333U, 0x0003U };

    SkeletonAssetRef Ref( AssetGuid guid, const char* name )
    {
        return SkeletonAssetRef{ guid, name };
    }

    bool Mentions( const std::string& text, const std::string& what )
    {
        return text.find( what ) != std::string::npos;
    }

    // Hips -> Spine -> Head, plus LeftLeg under Hips.
    Skeleton SmallRig()
    {
        std::vector<BoneInfo> bones( 4 );
        bones[0].Name         = "Hips";
        bones[1].Name         = "Spine";
        bones[1].ParentBoneID = 0U;
        bones[2].Name         = "Head";
        bones[2].ParentBoneID = 1U;
        bones[3].Name         = "LeftLeg";
        bones[3].ParentBoneID = 0U;
        return Skeleton( std::move( bones ) );
    }
} // namespace

// ---- the playback rule ------------------------------------------------------------------------------------

TEST( SkeletonReference, SameSkeletonPlays )
{
    const auto r = ClipPlaysOnMesh( Ref( kMannequin, "SK_Mannequin" ), Ref( kMannequin, "SK_Mannequin" ), {} );
    EXPECT_TRUE( r ) << r.GetError();
}

TEST( SkeletonReference, DifferentSkeletonRefusedNamingBoth )
{
    const auto r = ClipPlaysOnMesh( Ref( kMixamo, "SK_Mixamo" ), Ref( kMannequin, "SK_Mannequin" ), {} );
    ASSERT_FALSE( r );
    EXPECT_TRUE( Mentions( r.GetError(), "SK_Mixamo" ) ) << r.GetError();
    EXPECT_TRUE( Mentions( r.GetError(), "SK_Mannequin" ) ) << r.GetError();
}

TEST( SkeletonReference, CompatibleSkeletonPlays )
{
    const std::vector<AssetGuid> compatible = { kFox, kMixamo };
    const auto r = ClipPlaysOnMesh( Ref( kMixamo, "SK_Mixamo" ), Ref( kMannequin, "SK_Mannequin" ), compatible );
    EXPECT_TRUE( r ) << r.GetError();
}

// The MESH's skeleton states what it accepts. Mannequin listing Mixamo does not let Mannequin clips onto a
// Mixamo mesh whose own list is empty.
TEST( SkeletonReference, CompatibilityIsOneDirectional )
{
    const auto r = ClipPlaysOnMesh( Ref( kMannequin, "SK_Mannequin" ), Ref( kMixamo, "SK_Mixamo" ), {} );
    EXPECT_FALSE( r );
}

TEST( SkeletonReference, NullClipSkeletonRefused )
{
    const auto r = ClipPlaysOnMesh( Ref( AssetGuid{}, "" ), Ref( kMannequin, "SK_Mannequin" ), {} );
    ASSERT_FALSE( r );
    EXPECT_TRUE( Mentions( r.GetError(), "SK_Mannequin" ) ) << r.GetError();
}

// Two nulls are NOT "the same skeleton": a mesh and a clip that both name nothing do not pair.
TEST( SkeletonReference, NullOnBothSidesIsNotEquality )
{
    const auto r = ClipPlaysOnMesh( Ref( AssetGuid{}, "clip" ), Ref( AssetGuid{}, "mesh" ), {} );
    EXPECT_FALSE( r );
}

TEST( SkeletonReference, NullMeshSkeletonRefusedEvenIfListed )
{
    const std::vector<AssetGuid> compatible = { kMixamo };
    const auto r = ClipPlaysOnMesh( Ref( kMixamo, "SK_Mixamo" ), Ref( AssetGuid{}, "SKM_NoRig" ), compatible );
    EXPECT_FALSE( r );
}

// ---- the assignment check (where names live) -------------------------------------------------------------

TEST( SkeletonReference, AssignmentAcceptsBonesPresent )
{
    const Skeleton                  rig      = SmallRig();
    const std::vector<RequiredBone> required = { { "Hips", std::nullopt }, { "Head", std::nullopt } };
    const auto                      r        = CheckSkeletonAssignment( rig, "SK_Small", required, "A_Walk" );
    EXPECT_TRUE( r ) << r.GetError();
}

TEST( SkeletonReference, AssignmentRefusalListsEveryMissingBone )
{
    const Skeleton                  rig      = SmallRig();
    const std::vector<RequiredBone> required = {
         { "Hips", std::nullopt }, { "Tail", std::nullopt }, { "RightLeg", std::nullopt } };
    const auto r = CheckSkeletonAssignment( rig, "SK_Small", required, "A_Walk" );
    ASSERT_FALSE( r );
    EXPECT_TRUE( Mentions( r.GetError(), "Tail" ) ) << r.GetError();
    EXPECT_TRUE( Mentions( r.GetError(), "RightLeg" ) ) << r.GetError();
    EXPECT_TRUE( Mentions( r.GetError(), "A_Walk" ) ) << r.GetError();
    EXPECT_TRUE( Mentions( r.GetError(), "SK_Small" ) ) << r.GetError();
}

TEST( SkeletonReference, AssignmentChecksStatedParent )
{
    const Skeleton                  good     = SmallRig();
    const std::vector<RequiredBone> meshSkin = {
         { "Hips", std::string{} }, { "Spine", std::string( "Hips" ) }, { "Head", std::string( "Spine" ) } };
    EXPECT_TRUE( CheckSkeletonAssignment( good, "SK_Small", meshSkin, "SKM_Small" ) );

    const std::vector<RequiredBone> misparented = { { "Head", std::string( "Hips" ) } };
    const auto                      r = CheckSkeletonAssignment( good, "SK_Small", misparented, "SKM_Other" );
    ASSERT_FALSE( r );
    EXPECT_TRUE( Mentions( r.GetError(), "Head" ) ) << r.GetError();
}

TEST( SkeletonReference, AssignmentEmptyParentMeansRoot )
{
    const Skeleton                  rig      = SmallRig();
    const std::vector<RequiredBone> required = { { "Spine", std::string{} } };
    EXPECT_FALSE( CheckSkeletonAssignment( rig, "SK_Small", required, "SKM_Small" ) );
}

// ---- migration: signature -> GUID, refuse rather than guess -----------------------------------------------

TEST( SkeletonReference, FindBySignatureZeroMatchesNothing )
{
    const std::vector<SkeletonCandidate> skeletons = { { kMannequin, 0U, "a.skeleton" },
                                                       { kMixamo, 7U, "b.skeleton" } };
    EXPECT_TRUE( FindSkeletonsBySignature( 0U, skeletons ).empty() );
}

TEST( SkeletonReference, FindBySignatureAllMatchesInOrder )
{
    const std::vector<SkeletonCandidate> skeletons = {
         { kMannequin, 7U, "a.skeleton" }, { kFox, 9U, "fox.skeleton" }, { kMixamo, 7U, "b.skeleton" } };
    EXPECT_EQ( FindSkeletonsBySignature( 7U, skeletons ), ( std::vector<size_t>{ 0U, 2U } ) );
}

TEST( SkeletonReference, MigrateUniqueSignatureGivesItsGuid )
{
    const std::vector<SkeletonCandidate> skeletons = { { kMannequin, 7U, "a.skeleton" },
                                                       { kFox, 9U, "fox.skeleton" } };
    const auto                           r = MigrateSkeletonReference( "Anims/Fox_Run.anim", 9U, skeletons );
    ASSERT_TRUE( r ) << r.GetError();
    EXPECT_EQ( r.GetValue(), kFox );
}

TEST( SkeletonReference, MigrateAmbiguousRefusesWithEveryPath )
{
    const std::vector<SkeletonCandidate> skeletons = { { kMannequin, 7U, "Rigs/a.skeleton" },
                                                       { kMixamo, 7U, "Rigs/b.skeleton" } };
    const auto                           r         = MigrateSkeletonReference( "Anims/Walk.anim", 7U, skeletons );
    ASSERT_FALSE( r );
    EXPECT_TRUE( Mentions( r.GetError(), "Anims/Walk.anim" ) ) << r.GetError();
    EXPECT_TRUE( Mentions( r.GetError(), "Rigs/a.skeleton" ) ) << r.GetError();
    EXPECT_TRUE( Mentions( r.GetError(), "Rigs/b.skeleton" ) ) << r.GetError();
}

TEST( SkeletonReference, MigrateNoCandidateRefusesWithReferencingPath )
{
    const std::vector<SkeletonCandidate> skeletons = { { kMannequin, 7U, "a.skeleton" } };
    const auto                           r = MigrateSkeletonReference( "Meshes/Hero.skmesh", 8U, skeletons );
    ASSERT_FALSE( r );
    EXPECT_TRUE( Mentions( r.GetError(), "Meshes/Hero.skmesh" ) ) << r.GetError();
}

TEST( SkeletonReference, MigrateZeroSignatureRefused )
{
    const std::vector<SkeletonCandidate> skeletons = { { kMannequin, 0U, "a.skeleton" } };
    const auto                           r         = MigrateSkeletonReference( "Anims/Idle.anim", 0U, skeletons );
    ASSERT_FALSE( r );
    EXPECT_TRUE( Mentions( r.GetError(), "Anims/Idle.anim" ) ) << r.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
