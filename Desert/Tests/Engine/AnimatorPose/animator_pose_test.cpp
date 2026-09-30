// Unit tests for the Animator's editable pose buffer (the fix for Sequencer bone-keying that used to mutate
// the shared bind pose). The buffer is ADDITIVE: it never touches LocalBindTransform, and normal clip
// playback ignores it — only ApplyLocalPose() renders it.

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/AnimatorForSkeleton.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include "../ClipFixture.hpp"

#include <gtest/gtest.h>

#include <cmath>

using Desert::Animation::AnimationClip;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::Skeleton;

namespace
{
    // Two-bone chain: root at the origin, child 1 unit up (parent-relative). OffsetMatrices are recomputed
    // from the bind chain so the bind pose is self-consistent.
    Skeleton MakeChain()
    {
        std::vector<BoneInfo> bones( 2 );
        bones[0].Name               = "root";
        bones[0].ParentBoneID       = std::nullopt;
        bones[0].LocalBindTransform = glm::mat4( 1.0f );
        bones[0].OffsetMatrix       = glm::mat4( 1.0f );

        bones[1].Name               = "child";
        bones[1].ParentBoneID       = 0u;
        bones[1].LocalBindTransform = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
        bones[1].OffsetMatrix       = glm::mat4( 1.0f );

        Skeleton skel( std::move( bones ) );
        skel.RecomputeOffsetMatrices(); // OffsetMatrix = inverse(global bind)
        return skel;
    }

    ::testing::AssertionResult MatNear( const glm::mat4& a, const glm::mat4& b, float eps = 1e-4f )
    {
        for ( int c = 0; c < 4; ++c )
            for ( int r = 0; r < 4; ++r )
                if ( std::abs( a[c][r] - b[c][r] ) > eps )
                    return ::testing::AssertionFailure()
                           << "mismatch at [" << c << "][" << r << "]: " << a[c][r] << " vs " << b[c][r];
        return ::testing::AssertionSuccess();
    }

    /// One second on the project grid, "child" held at @p pos: a Bone binding with one Transform track.
    AnimationClip ChildPosClip( const glm::vec3& pos )
    {
        return ClipFixture::StaticBoneClip( "test", FrameNumber{ PROJECT_TICK_RATE.Numerator }, "child", pos );
    }
} // namespace

TEST( AnimatorPose, LocalPoseStartsAtBind )
{
    Skeleton skel = MakeChain();
    Animator anim( skel );
    EXPECT_TRUE( MatNear( anim.GetBoneLocalPose( 0 ), skel.GetBones()[0].LocalBindTransform ) );
    EXPECT_TRUE( MatNear( anim.GetBoneLocalPose( 1 ), skel.GetBones()[1].LocalBindTransform ) );
    EXPECT_TRUE( MatNear( anim.GetBoneLocalPose( 99 ), glm::mat4( 1.0f ) ) ); // out of range -> identity
}

TEST( AnimatorPose, SetBoneLocalPoseDoesNotMutateBind )
{
    Skeleton        skel         = MakeChain();
    const glm::mat4 originalBind = skel.GetBones()[1].LocalBindTransform;
    Animator        anim( skel );

    const glm::mat4 posed = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.5f, 1.0f, 0.0f ) );
    anim.SetBoneLocalPose( 1, posed );

    EXPECT_TRUE( MatNear( anim.GetBoneLocalPose( 1 ), posed ) );
    // The rig's shared bind pose is untouched — the whole point of the fix.
    EXPECT_TRUE( MatNear( skel.GetBones()[1].LocalBindTransform, originalBind ) );
}

TEST( AnimatorPose, ApplyLocalPoseRendersPosedSkeleton )
{
    Skeleton skel = MakeChain();
    Animator anim( skel );

    const glm::mat4 posedChildLocal = glm::translate( glm::mat4( 1.0f ), glm::vec3( 2.0f, 1.0f, 0.0f ) );
    anim.SetBoneLocalPose( 1, posedChildLocal );
    anim.ApplyLocalPose();

    const auto&     bones       = skel.GetBones();
    const glm::mat4 rootGlobal  = anim.GetBoneLocalPose( 0 ); // root local == global (no parent)
    const glm::mat4 childGlobal = rootGlobal * posedChildLocal;
    const glm::mat4 expected    = childGlobal * bones[1].OffsetMatrix;

    ASSERT_GE( anim.GetPose().Matrices.size(), 2u );
    EXPECT_TRUE( MatNear( anim.GetPose().Matrices[1], expected ) );
}

TEST( AnimatorPose, SampleClipIntoLocalPoseLoadsKeys )
{
    Skeleton      skel = MakeChain();
    Animator      anim( skel );
    AnimationClip clip = ChildPosClip( glm::vec3( 0.0f, 3.0f, 0.0f ) );

    anim.SampleClipIntoLocalPose( clip, FrameTime{} );

    const glm::mat4 expectedChild = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 3.0f, 0.0f ) );
    EXPECT_TRUE( MatNear( anim.GetBoneLocalPose( 1 ), expectedChild ) );
    // Untracked root falls back to bind.
    EXPECT_TRUE( MatNear( anim.GetBoneLocalPose( 0 ), skel.GetBones()[0].LocalBindTransform ) );
}

TEST( AnimatorPose, PlaybackIgnoresLocalPoseBuffer )
{
    Skeleton      skel = MakeChain();
    Animator      anim( skel );
    AnimationClip clip = ChildPosClip( glm::vec3( 0.0f, 5.0f, 0.0f ) );

    anim.Play( clip );
    anim.SetTime( 0.0f );
    const glm::mat4 p1 = anim.GetPose().Matrices[1];

    // Editing the pose buffer must NOT change what SetTime/playback produces (additive-only guarantee).
    anim.SetBoneLocalPose( 1, glm::translate( glm::mat4( 1.0f ), glm::vec3( 99.0f, 0.0f, 0.0f ) ) );
    anim.SetTime( 0.0f );
    const glm::mat4 p2 = anim.GetPose().Matrices[1];

    EXPECT_TRUE( MatNear( p1, p2 ) );
}

// `ResetLocalPoseToBind` was deleted with the test that used to stand here: it had no caller outside this
// file, and an entry point exercised only by its own test is not a feature, it is a claim. What the buffer
// IS reset by has a production caller, and that is what is pinned instead: sampling a clip into it
// overwrites every bone, so a bone the clip does not animate goes back to bind.
TEST( AnimatorPose, SamplingAClipIntoTheBufferResetsBonesTheClipDoesNotAnimate )
{
    Skeleton      skel = MakeChain();
    Animator      anim( skel );
    AnimationClip clip = ChildPosClip( glm::vec3( 0.0f, 5.0f, 0.0f ) ); // animates bone 1 only

    anim.SetBoneLocalPose( 0, glm::translate( glm::mat4( 1.0f ), glm::vec3( 7.0f, 0.0f, 0.0f ) ) );
    anim.SampleClipIntoLocalPose( clip, FrameTime{} );

    EXPECT_TRUE( MatNear( anim.GetBoneLocalPose( 0 ), skel.GetBones()[0].LocalBindTransform ) )
         << "an untracked bone kept an authored value across a clip load, so the buffer and the clip "
            "disagree about what the pose at this time is";
}

// ── A SECTION REACHES THE SKINNING MATRICES, NOT ONLY THE CLIP (A28) ───────────────────────────────
//
// THE POSITIVE CONTROL THE `ClipSections` SUITE CANNOT PROVIDE. That suite asserts the section maths
// (`Timeline::WeightAt`); it does not say whether PLAYBACK folds a bone track's section weight. A pose sampler
// reading the Transform channel past the section would leave every assertion over there green while no section in
// the project changed a single pixel — which is exactly the shape of "both named suites stayed green while the
// graph received nothing".
//
// `GetPose().Matrices` is what the renderer uploads (Scene.cpp and MeshECSSystem.hpp are its only two
// consumers), so asserting here is asserting about the frame.
TEST( AnimatorPose, AZeroWeightSectionReachesThePoseThePlaybackProduces )
{
    Skeleton      skel = MakeChain();
    Animator      anim( skel );
    AnimationClip clip = ChildPosClip( glm::vec3( 0.0f, 5.0f, 0.0f ) );

    anim.Play( clip );
    anim.SetTime( 0.0f );
    const glm::mat4 unsectioned = anim.GetPose().Matrices[1];

    // The same clip, its bone track's section muted by a zero Weight. Nothing else about it changes.
    AnimationClip muted = ClipFixture::Clip( "test", FrameNumber{ PROJECT_TICK_RATE.Numerator } );
    Desert::Animation::Timeline::Section& off =
         ClipFixture::AddStaticBone( muted, "child", glm::vec3( 0.0f, 5.0f, 0.0f ) );
    off.Name  = "muted";
    off.Blend = Desert::Animation::SectionBlendType::Absolute;
    off.Weight.push_back( ClipFixture::Key( FrameNumber{ 0 }, 0.0f ) );

    Animator silent( skel );
    silent.Play( muted );
    silent.SetTime( 0.0f );
    const glm::mat4 sectioned = silent.GetPose().Matrices[1];

    EXPECT_FALSE( MatNear( unsectioned, sectioned ) )
         << "a zero-weight section changed nothing in the matrices the renderer uploads, so playback is "
            "not folding the bone track's section weight (Timeline::WeightAt)";

    // AND IT IS THE REST POSE IT FELL BACK TO, not an arbitrary difference. A clip that broke for any
    // other reason would also satisfy the assertion above.
    Animator rest( skel );
    EXPECT_TRUE( MatNear( sectioned, rest.GetPose().Matrices[1] ) );

    // NEGATIVE CONTROL: a FULL-weight Absolute section must leave the same matrices the unsectioned clip
    // produced, because that is what every migrated file in the repository now carries.
    // Keyed at 1 rather than left empty (empty already IS the clip above): the keyed-weight path at full
    // weight must be the identity too.
    AnimationClip full = ClipFixture::Clip( "test", FrameNumber{ PROJECT_TICK_RATE.Numerator } );
    Desert::Animation::Timeline::Section& whole =
         ClipFixture::AddStaticBone( full, "child", glm::vec3( 0.0f, 5.0f, 0.0f ) );
    whole.Name  = "whole";
    whole.Blend = Desert::Animation::SectionBlendType::Absolute;
    whole.Weight.push_back( ClipFixture::Key( FrameNumber{ 0 }, 1.0f ) );

    Animator unchanged( skel );
    unchanged.Play( full );
    unchanged.SetTime( 0.0f );
    EXPECT_TRUE( MatNear( unsectioned, unchanged.GetPose().Matrices[1] ) )
         << "a full-weight Absolute section is what the whole corpus migrated to; it must be invisible";
}

// A REIMPORTED RIG IS THE SAME SKELETON OBJECT WITH OTHER BONES (SkeletonAsset::LoadFromFile rewrites it in
// place, AssetEviction.AReloadedRigKeepsItsAddressAndTakesTheNewBones). The Animator's `const Skeleton&` stays
// valid, but its bind pose and buffers were sized from the old list: only the signature stamp can tell, and
// EnsureAnimatorFor is the one rule AnimationECSSystem and the editor preview rebuild by.
TEST( AnimatorPose, AnAnimatorIsRebuiltWhenItsSkeletonIsRewrittenWithAnExtraBone )
{
    std::vector<BoneInfo> one( 1 );
    one[0].Name               = "root";
    one[0].LocalBindTransform = glm::mat4( 1.0f );
    one[0].OffsetMatrix       = glm::mat4( 1.0f );
    Skeleton rig( std::move( one ) );

    std::unique_ptr<Animator> animator;
    uint64_t                  built = 0;
    ASSERT_TRUE( Desert::Animation::EnsureAnimatorFor( animator, built, rig ) ) << "no Animator was built";
    animator->ApplyLocalPose();
    ASSERT_EQ( animator->GetPose().Matrices.size(), 1U );
    const Animator* const first = animator.get();
    EXPECT_FALSE( Desert::Animation::EnsureAnimatorFor( animator, built, rig ) )
         << "an unchanged rig rebuilt the Animator, which throws away its live pose every frame";
    EXPECT_EQ( animator.get(), first );

    rig = MakeChain(); // the reimport: same object, one more bone
    ASSERT_TRUE( Desert::Animation::EnsureAnimatorFor( animator, built, rig ) )
         << "the rig gained a bone and the Animator built on one bone was kept";
    EXPECT_EQ( &animator->GetSkeleton(), &rig );
    EXPECT_EQ( built, rig.GetContentSignature() ) << "the stamp is the rig's content (names + binds)";
    animator->ApplyLocalPose();
    EXPECT_EQ( animator->GetPose().Matrices.size(), 2U ) << "the rebuilt pose does not have the new bone";
    EXPECT_EQ( animator->GetLocalPose().Size(), 2U );
}

// THM1l-b19: A RIG REREAD WITH THE SAME BONE NAMES AND OTHER BINDS (a reimport at another Uniform Scale) is
// another rig to the Animator: the name signature stays, the content signature moves, and EnsureAnimatorFor
// builds a new Animator. Kept across it, the old one posed Fox.glb's rescaled mesh with the old binds.
TEST( AnimatorPose, ARigRereadWithOtherBindsRebuildsTheAnimator )
{
    Skeleton                                     skeleton = MakeChain();
    std::unique_ptr<Desert::Animation::Animator> animator;
    uint64_t                                     built = 0;
    ASSERT_TRUE( Desert::Animation::EnsureAnimatorFor( animator, built, skeleton ) );
    EXPECT_FALSE( Desert::Animation::EnsureAnimatorFor( animator, built, skeleton ) )
         << "an unchanged rig rebuilt";

    std::vector<BoneInfo> scaled = skeleton.GetBones();
    for ( auto& bone : scaled )
    {
        bone.LocalBindTransform[3] = glm::vec4( 10.0f * glm::vec3( bone.LocalBindTransform[3] ), 1.0f );
        bone.OffsetMatrix[3]       = glm::vec4( 10.0f * glm::vec3( bone.OffsetMatrix[3] ), 1.0f );
    }
    const uint64_t names = skeleton.GetSignature();
    skeleton             = Skeleton( std::move( scaled ) ); // SkeletonAsset::LoadFromFile: the same address
    EXPECT_EQ( skeleton.GetSignature(), names );
    EXPECT_TRUE( Desert::Animation::EnsureAnimatorFor( animator, built, skeleton ) )
         << "the Animator kept the old binds across a reread";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
