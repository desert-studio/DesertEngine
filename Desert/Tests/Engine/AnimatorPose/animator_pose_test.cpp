// Unit tests for the Animator's editable pose buffer (the fix for Sequencer bone-keying that used to mutate
// the shared bind pose). The buffer is ADDITIVE: it never touches LocalBindTransform, and normal clip
// playback ignores it — only ApplyLocalPose() renders it.

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <cmath>

using Desert::Animation::AnimationClip;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTrack;
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

    AnimationClip ChildPosClip( const glm::vec3& pos )
    {
        AnimationClip clip;
        // A5: the clip states a length in TICKS on the project grid. `Duration` + `TicksPerSecond = 1`
        // used to say "one second" by setting the rate so a tick WAS a second.
        clip.AnimationName = "test";
        clip.DurationTicks = FrameNumber{ PROJECT_TICK_RATE.Numerator };

        BoneTrack track;
        track.BoneName = "child";
        track.PositionKeys.push_back( { FrameNumber{ 0 }, pos } );
        track.RotationKeys.push_back( { FrameNumber{ 0 }, glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ) } );
        track.ScaleKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 1.0f ) } );
        clip.Tracks.push_back( track );
        return clip;
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

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
