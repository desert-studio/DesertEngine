// The half of the Animator that had no test at all: CrossFade, the pose graph's layer and additive nodes
// (they replaced AddLayer / SetLayerMaskByNames), ResolveTrack and notify firing. `grep -rln
// "AddLayer\|CrossFade\|SetLayerMask" Desert/Tests` returned nothing before this file, while `Animator.cpp` spent
// 300 of its 576 lines on exactly those.
//
// EVERY ASSERTION HERE IS ABOUT A RELATION, not about a number this build happens to produce. The pose
// substrate changed how all of this is computed, so a test written against the old output would only
// prove the new code reproduces the old code — including the parts of it that were wrong. What is pinned
// instead is what has to be true of any implementation: a blend at 0 IS its source, a bone outside a mask
// does not move by one bit, a notify fires once per pass over its time, and a rig's segment lengths
// survive a rotation.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include "../ClipFixture.hpp"
#include "../PoseGraphFixture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

using Common::Timestep;
using Desert::Animation::AnimationClip;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::NearestTick;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::SecondsToFrameTime;
namespace Animation = Desert::Animation;
using Desert::Animation::PoseStage;
using Desert::Animation::Skeleton;

namespace
{
    bool MatExact( const glm::mat4& a, const glm::mat4& b )
    {
        for ( int c = 0; c < 4; ++c )
        {
            for ( int r = 0; r < 4; ++r )
            {
                if ( a[c][r] != b[c][r] )
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool MatNear( const glm::mat4& a, const glm::mat4& b, float eps = 1e-4F )
    {
        for ( int c = 0; c < 4; ++c )
        {
            for ( int r = 0; r < 4; ++r )
            {
                if ( std::fabs( a[c][r] - b[c][r] ) > eps )
                {
                    return false;
                }
            }
        }
        return true;
    }

    BoneInfo MakeBone( const char* name, std::optional<uint32_t> parent, const glm::mat4& localBind )
    {
        BoneInfo b;
        b.Name               = name;
        b.ParentBoneID       = parent;
        b.LocalBindTransform = localBind;
        return b;
    }

    /// root -> spine -> arm, every bind transform non-trivial so `OffsetMatrix` is nowhere near identity.
    /// That is the whole point: the crossfade used to blend `chainGlobal * OffsetMatrix` and the error it
    /// made is proportional to how far OffsetMatrix is from the identity — which on the repository's one
    /// probe rig (one bone, both matrices identity) is exactly zero.
    Skeleton MakeRig()
    {
        std::vector<BoneInfo> bones;
        bones.push_back( MakeBone( "root", std::nullopt,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 90.0F, 0.0F ) ) ) );
        bones.push_back( MakeBone( "spine", 0U,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 30.0F, 0.0F ) ) *
                                        glm::rotate( glm::mat4( 1.0F ), 0.35F, glm::vec3( 0, 0, 1 ) ) ) );
        bones.push_back( MakeBone( "arm", 1U,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 20.0F, 0.0F, 0.0F ) ) *
                                        glm::rotate( glm::mat4( 1.0F ), -0.8F, glm::vec3( 1, 0, 0 ) ) ) );
        Skeleton skeleton( std::move( bones ) );
        skeleton.RecomputeOffsetMatrices();
        return skeleton;
    }

    /// A one-key clip: `bone` sits at `position` with `rotation`, for the whole of `duration`.
    AnimationClip StaticClip( const char* name, const char* bone, const glm::vec3& position,
                              const glm::quat& rotation = glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ),
                              float            duration = 2.0F )
    {
        // The clip's data is its Timeline::Sequence: a Bone binding with one Transform track (ClipFixture).
        return ClipFixture::StaticBoneClip( name, NearestTick( SecondsToFrameTime( duration, PROJECT_TICK_RATE ) ),
                                            bone, position, rotation );
    }

    /// Component-space position of a bone, recovered from the skinning matrices the way every consumer
    /// downstream of GetPose() effectively does.
    glm::vec3 BonePosition( const Animator& animator, uint32_t bone )
    {
        return glm::vec3( animator.GetBoneModelMatrix( bone )[3] );
    }
} // namespace

// ── The pipeline is a list, and its membership is data ──────────────────────────────────────────────

TEST( AnimatorBlending, TheGraphStageJoinsAndLeavesWithThePoseGraph )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip = StaticClip( "Base", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ) );

    ASSERT_EQ( animator.GetStages().size(), 1U );
    EXPECT_EQ( animator.GetStages()[0], PoseStage::Source )
         << "something has to produce a pose; Source is not optional";

    ASSERT_TRUE( PoseGraphFixture::Drive( animator, PoseGraphFixture::FullBodyLayer( "root" ), clip ) );
    ASSERT_EQ( animator.GetStages().size(), 2U );
    EXPECT_EQ( animator.GetStages()[1], PoseStage::Graph )
         << "layers must run AFTER the source — an order this test exists to state, because the previous "
            "code expressed it only as statement order inside Update()";

    animator.ClearPoseGraph();
    EXPECT_EQ( animator.GetStages().size(), 1U )
         << "a stage with nothing to do stayed in the list, so the list stopped describing what runs";
}

// ── CrossFade ───────────────────────────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, ACrossFadeAtZeroIsTheSourceClipBitForBit )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    // THE TWO CLIPS MUST DIFFER IN ROTATION, and the first version of this test did not make them. With
    // equal quaternions glm::slerp takes its cosTheta ~ 1 shortcut and returns the input exactly, so the
    // short-circuit this test exists to pin was UNREACHABLE and deleting it left the suite green — a
    // surviving mutation, and a test that proved nothing. A wide angle forces the sin(theta) path, where
    // sin(0)/sin(theta) is not exactly the identity in floating point.
    AnimationClip a = StaticClip( "A", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ),
                                  glm::angleAxis( 0.3F, glm::normalize( glm::vec3( 1, 2, 3 ) ) ) );
    AnimationClip b = StaticClip( "B", "spine", glm::vec3( 0.0F, 80.0F, 0.0F ),
                                  glm::angleAxis( 2.4F, glm::normalize( glm::vec3( 3, -1, 2 ) ) ) );

    animator.Play( a );
    animator.SetTime( 0.5F );
    const glm::mat4 pureA = animator.GetPose().Matrices[1];

    animator.CrossFade( b, 4.0F );
    animator.Update( Timestep( 0.0F ) ); // alpha still 0

    EXPECT_TRUE( MatExact( animator.GetPose().Matrices[1], pureA ) )
         << "the first frame of a crossfade already moved the character. A blend at alpha 0 that is only "
            "APPROXIMATELY its source is a visible pop at the start of every transition";
}

TEST( AnimatorBlending, ACrossFadeAtOneIsTheTargetClip )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip a = StaticClip( "A", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ) );
    AnimationClip b = StaticClip( "B", "spine", glm::vec3( 0.0F, 80.0F, 0.0F ) );

    animator.Play( b );
    animator.SetTime( 0.0F );
    const glm::mat4 pureB = animator.GetPose().Matrices[1];

    animator.Play( a );
    animator.CrossFade( b, 1.0F );
    animator.Update( Timestep( 1.0F ) ); // alpha hits exactly 1

    EXPECT_TRUE( MatNear( animator.GetPose().Matrices[1], pureB ) )
         << "the end of the blend is not the clip it blended to, so the transition ends with a snap";
    EXPECT_EQ( animator.GetCurrentClip(), &b ) << "the blend did not retire into its target";
}

TEST( AnimatorBlending, ACrossFadeKeepsTheSkeletonConnected )
{
    // THE INVARIANT THAT DOES NOT DEPEND ON THE IMPLEMENTATION: blending two poses of the same rig cannot
    // change the distance from a bone to its parent, because neither pose scales or translates the bone's
    // own local. The old crossfade blended `chainGlobal * OffsetMatrix` — a quantity with the inverse bind
    // baked in — so a slerp of it is not a rotation of the bone, and the arm can leave the shoulder.
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip a =
         StaticClip( "A", "arm", glm::vec3( 20.0F, 0.0F, 0.0F ), glm::angleAxis( 0.0F, glm::vec3( 0, 0, 1 ) ) );
    AnimationClip b =
         StaticClip( "B", "arm", glm::vec3( 20.0F, 0.0F, 0.0F ), glm::angleAxis( 2.2F, glm::vec3( 0, 0, 1 ) ) );

    animator.Play( a );
    animator.SetTime( 0.0F );
    const float restLength = glm::length( BonePosition( animator, 2 ) - BonePosition( animator, 1 ) );
    ASSERT_GT( restLength, 1.0F ) << "the fixture is degenerate: the arm sits on top of the spine";

    animator.CrossFade( b, 1.0F );
    for ( int step = 0; step < 10; ++step )
    {
        animator.Update( Timestep( 0.1F ) );
        const float length = glm::length( BonePosition( animator, 2 ) - BonePosition( animator, 1 ) );
        EXPECT_NEAR( length, restLength, restLength * 1e-3F )
             << "at step " << step << " the arm is " << length << " from the spine instead of " << restLength
             << "; a rotation blend moved a bone away from its parent";
    }
}

// ── Layers ──────────────────────────────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, AnOverrideLayerAtFullWeightReplacesTheBase )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip base  = StaticClip( "Base", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ) );
    AnimationClip layer = StaticClip( "Layer", "spine", glm::vec3( 0.0F, 300.0F, 0.0F ) );

    animator.Play( layer );
    animator.SetTime( 0.0F );
    const glm::mat4 pureLayer = animator.GetPose().Matrices[1];

    animator.Play( base );
    ASSERT_TRUE( PoseGraphFixture::Drive( animator, PoseGraphFixture::FullBodyLayer( "root" ), layer, 1.0F ) );
    animator.Update( Timestep( 0.0F ) );

    EXPECT_TRUE( MatNear( animator.GetPose().Matrices[1], pureLayer ) )
         << "weight 1 on an override layer is not the layer, so the weight slider does not mean what it says";
}

TEST( AnimatorBlending, AnOverrideLayerAtZeroWeightChangesNothing )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip base  = StaticClip( "Base", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ) );
    AnimationClip layer = StaticClip( "Layer", "spine", glm::vec3( 0.0F, 300.0F, 0.0F ) );

    animator.Play( base );
    animator.Update( Timestep( 0.0F ) );
    const auto without = animator.GetPose().Matrices;

    ASSERT_TRUE( PoseGraphFixture::Drive( animator, PoseGraphFixture::FullBodyLayer( "root" ), layer, 0.0F ) );
    animator.Update( Timestep( 0.0F ) );

    for ( size_t i = 0; i < without.size(); ++i )
    {
        EXPECT_TRUE( MatExact( animator.GetPose().Matrices[i], without[i] ) )
             << "bone " << i << " moved under a layer of weight zero";
    }
}

TEST( AnimatorBlending, AnApplyAdditiveNodeAddsItsDeltaFromBindAndNotItsPose )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    // The bind local of "spine" translates (0,30,0). An additive clip putting it at (0,45,0) is a delta of
    // +15, so over a base at (0,100,0) the result must be (0,115,0) — NOT (0,45,0), which is what an
    // additive layer implemented as an override would give.
    AnimationClip base     = StaticClip( "Base", "spine", glm::vec3( 0.0F, 100.0F, 0.0F ) );
    AnimationClip additive = StaticClip( "Add", "spine", glm::vec3( 0.0F, 45.0F, 0.0F ) );

    animator.Play( base );
    ASSERT_TRUE( PoseGraphFixture::Drive( animator, PoseGraphFixture::AdditiveGraph(), additive, 1.0F ) );
    animator.Update( Timestep( 0.0F ) );

    const glm::vec3 spine = BonePosition( animator, 1 );
    const glm::vec3 root  = BonePosition( animator, 0 );
    EXPECT_NEAR( spine.y - root.y, 115.0F, 1e-3F )
         << "the additive layer contributed its POSE rather than its delta from bind";
}

TEST( AnimatorBlending, HalfWeightIsBetweenTheTwoAndOnTheSegment )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip base  = StaticClip( "Base", "spine", glm::vec3( 0.0F, 0.0F, 0.0F ) );
    AnimationClip layer = StaticClip( "Layer", "spine", glm::vec3( 0.0F, 100.0F, 0.0F ) );

    animator.Play( base );
    ASSERT_TRUE( PoseGraphFixture::Drive( animator, PoseGraphFixture::FullBodyLayer( "root" ), layer, 0.5F ) );
    animator.Update( Timestep( 0.0F ) );

    const glm::vec3 spine = BonePosition( animator, 1 );
    const glm::vec3 root  = BonePosition( animator, 0 );
    EXPECT_NEAR( spine.y - root.y, 50.0F, 1e-3F ) << "a half-weight override is not halfway";
}

// ── Masks ───────────────────────────────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, ABoneOutsideTheBranchDoesNotMoveByOneBit )
{
    const Skeleton skeleton = MakeRig(); // root -> spine -> arm
    Animator       animator( skeleton );

    // The layer keys the ROOT only; its branch is the spine's. The root, outside the branch, must keep the
    // base's transform bit for bit; the spine, inside it, takes the layer's (its bind, the layer not keying it).
    const AnimationClip base  = StaticClip( "Base", "spine", glm::vec3( 0.0F, 60.0F, 0.0F ) ); // not the bind's 30
    AnimationClip layer = StaticClip( "Layer", "root", glm::vec3( 500.0F, 0.0F, 0.0F ) );

    animator.Play( base );
    animator.Update( Timestep( 0.0F ) );
    const Desert::Animation::BoneTransform rootWithout  = animator.GetLocalPose()[0];
    const Desert::Animation::BoneTransform spineWithout = animator.GetLocalPose()[1];

    ASSERT_TRUE(
         PoseGraphFixture::Drive( animator, PoseGraphFixture::OneLayerGraph( { { "spine", 0 } } ), layer ) );
    animator.Update( Timestep( 0.0F ) );

    const Desert::Animation::BoneTransform rootWith = animator.GetLocalPose()[0];
    EXPECT_EQ( rootWith.Translation, rootWithout.Translation )
         << "a bone outside the layer's branch changed; the per-bone table is not being consulted";
    EXPECT_EQ( rootWith.Rotation, rootWithout.Rotation );
    EXPECT_EQ( rootWith.Scale, rootWithout.Scale );

    EXPECT_NE( animator.GetLocalPose()[1].Translation, spineWithout.Translation )
         << "the layer changed nothing at all, so the test above proves nothing";
}

TEST( AnimatorBlending, ABranchFilterReachesItsDescendantsAndANegativeDepthExcludes )
{
    namespace G             = Desert::Animation::Graph;
    const Skeleton skeleton = MakeRig(); // root -> spine -> arm

    G::LayeredBlendPerBoneNode node;
    node.Layers.push_back( G::LayerSetup{ { { "spine", 0 } } } );
    const auto whole = G::BuildPerBoneWeights( node, skeleton );
    ASSERT_TRUE( whole ) << whole.GetError();
    EXPECT_EQ( whole.GetValue()[0].Layer, -1 ) << "the PARENT was reached";
    EXPECT_EQ( whole.GetValue()[1].Layer, 0 );
    EXPECT_EQ( whole.GetValue()[2].Layer, 0 )
         << "a spine branch has to take the arm, or 'upper body' is unsayable";
    EXPECT_EQ( whole.GetValue()[2].Weight, 1.0F );

    node.Layers[0].Filters.push_back( { "arm", -1 } );
    const auto excluded = G::BuildPerBoneWeights( node, skeleton );
    ASSERT_TRUE( excluded ) << excluded.GetError();
    EXPECT_EQ( excluded.GetValue()[2].Weight, 0.0F ) << "a negative depth did not exclude the arm";
}

TEST( AnimatorBlending, AnUnknownFilterBoneIsRefusedByNameRatherThanReachingNothing )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    const AnimationClip clip = StaticClip( "Layer", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ) );

    const auto set =
         PoseGraphFixture::Drive( animator, PoseGraphFixture::OneLayerGraph( { { "no_such_bone", 0 } } ), clip );
    ASSERT_FALSE( set )
         << "a layer naming a bone the rig lacks was accepted — a typo is a layer that does nothing";
    EXPECT_NE( set.GetError().find( "no_such_bone" ), std::string::npos ) << set.GetError();
    EXPECT_EQ( animator.GetPoseGraph(), nullptr );
}

TEST( AnimatorBlending, LayersKeepRunningThroughACrossFade )
{
    // `if ( !m_IsBlending ) ApplyLayers();` under a comment calling it "that brief frame". It is not a
    // frame, it is the whole blend: a 0.5 s crossfade dropped an upper-body override for 0.5 s.
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip a     = StaticClip( "A", "spine", glm::vec3( 0.0F, 0.0F, 0.0F ) );
    AnimationClip b     = StaticClip( "B", "spine", glm::vec3( 0.0F, 10.0F, 0.0F ) );
    AnimationClip layer = StaticClip( "Layer", "spine", glm::vec3( 0.0F, 400.0F, 0.0F ) );

    animator.Play( a );
    ASSERT_TRUE( PoseGraphFixture::Drive( animator, PoseGraphFixture::FullBodyLayer( "root" ), layer ) );
    animator.CrossFade( b, 1.0F );

    animator.Update( Timestep( 0.25F ) ); // mid-blend
    const glm::vec3 spine = BonePosition( animator, 1 );
    const glm::vec3 root  = BonePosition( animator, 0 );
    EXPECT_NEAR( spine.y - root.y, 400.0F, 1e-3F )
         << "the layer was dropped for the duration of the blend; an upper-body override vanished for the "
            "whole transition and came back when it ended";
}

// ── ResolveTrack ────────────────────────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, ATrackBindsByNameAndNotByPosition )
{
    const Skeleton skeleton = MakeRig(); // root=0, spine=1, arm=2
    Animator       animator( skeleton );

    // One track, named "arm", sitting at index 0 of the clip. A clip and the rig it drives come from
    // different files with different bone orders, which is the entire reason the binding is by name.
    AnimationClip clip = StaticClip( "ArmOnly", "arm", glm::vec3( 77.0F, 0.0F, 0.0F ) );

    animator.Play( clip );
    animator.Update( Timestep( 0.0F ) );

    // The DISTANCE, not the x component: the spine carries a bind rotation, so the arm's local offset of
    // (77,0,0) arrives in component space turned by it. Asserting the x difference would be asserting
    // cos(0.35) — a fact about the fixture's rotation, not about whether the track reached its bone.
    const glm::vec3 arm   = BonePosition( animator, 2 );
    const glm::vec3 spine = BonePosition( animator, 1 );
    EXPECT_NEAR( glm::length( arm - spine ), 77.0F, 1e-3F )
         << "the clip's only track did not reach the bone it names";

    // And the negative control: the bone the clip does NOT name stayed where the bind put it.
    const Animator rest( skeleton );
    EXPECT_TRUE( MatNear( animator.GetPose().Matrices[0], rest.GetPose().Matrices[0] ) )
         << "a clip with one track named 'arm' moved the root as well";
}

TEST( AnimatorBlending, ATrackNameTheRigDoesNotHaveLeavesThatBoneAtBind )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip  foreign = StaticClip( "Foreign", "mixamorig:Hips", glm::vec3( 999.0F, 0.0F, 0.0F ) );
    const Animator rest( skeleton );

    animator.Play( foreign );
    animator.Update( Timestep( 0.0F ) );

    for ( size_t i = 0; i < animator.GetPose().Matrices.size(); ++i )
    {
        EXPECT_TRUE( MatNear( animator.GetPose().Matrices[i], rest.GetPose().Matrices[i] ) )
             << "bone " << i << " moved for a clip that animates no bone this rig has";
    }
}

TEST( AnimatorBlending, TheTrackCacheIsRebuiltWhenTheClipsStorageMoves )
{
    // The clip keeps its address while its Sequence's track list is freed and reallocated — which is exactly what
    // asset eviction plus reload does, and what segfaulted inside lower_bound when the cache was keyed on
    // the address alone.
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip clip = StaticClip( "Live", "spine", glm::vec3( 0.0F, 10.0F, 0.0F ) );
    animator.Play( clip );
    animator.Update( Timestep( 0.0F ) );
    EXPECT_NEAR( BonePosition( animator, 1 ).y - BonePosition( animator, 0 ).y, 10.0F, 1e-3F );

    // THE RELOADED CLIP DRIVES A DIFFERENT BONE, and that is what makes this test able to fail. Reloading
    // the same bone with a different value is not enough: the stale pointer aims at freed memory, and
    // whether reading it yields the old value, the new one, or rubbish is the ALLOCATOR's business — with
    // a same-sized reallocation it lands on the very block just freed and reports the new value, so the
    // test passes with the cache-invalidation check deleted. Written this way, the stale binding maps
    // "arm" to the nullptr the OLD clip's track list gave it, so the arm simply does not move, and that
    // is a fact about the binding rather than about the heap.
    // Exactly what an evicted-then-reloaded asset does to the clip it owns: the AnimationClip keeps its
    // address, its track list is freed and rebuilt, and `AnimationAsset` stamps the new generation.
    clip.Sequence.Bindings.clear();
    clip.Sequence.Bindings.shrink_to_fit();
    clip.Sequence.Tracks.clear();
    clip.Sequence.Tracks.shrink_to_fit();

    AnimationClip reloaded = StaticClip( "Live", "arm", glm::vec3( 250.0F, 0.0F, 0.0F ) );
    clip.Sequence.Bindings = std::move( reloaded.Sequence.Bindings );
    clip.Sequence.Tracks   = std::move( reloaded.Sequence.Tracks );
    ++clip.Sequence.Revision;

    animator.Update( Timestep( 0.0F ) );
    EXPECT_NEAR( glm::length( BonePosition( animator, 2 ) - BonePosition( animator, 1 ) ), 250.0F, 1e-3F )
         << "the reloaded clip's track never reached its bone: the cache was keyed on the clip's ADDRESS, "
            "which an unload+reload does not change, so it kept the binding built against storage that has "
            "been returned to the allocator";
}

// ── Notifies ────────────────────────────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, ANotifyFiresExactlyOncePerPassOverItsTime )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip clip = StaticClip( "Walk", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ),
                                     /*duration=*/1.0F );
    ClipFixture::AddNotify( clip, "Footstep", NearestTick( SecondsToFrameTime( 0.5, PROJECT_TICK_RATE ) ) );

    animator.Play( clip, /*loop=*/false );

    int fired = 0;
    for ( int step = 0; step < 20; ++step ) // 20 x 0.1 s = 2 s over a 1 s clip
    {
        animator.Update( Timestep( 0.1F ) );
        fired += static_cast<int>( animator.ConsumeNotifyEvents().size() );
    }
    EXPECT_EQ( fired, 1 ) << "a marker at 0.5 s fired " << fired << " times in one non-looping pass";
}

TEST( AnimatorBlending, ALoopingClipFiresItsNotifyOncePerLap )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip clip = StaticClip( "Run", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ),
                                     /*duration=*/1.0F );
    ClipFixture::AddNotify( clip, "MidStep", NearestTick( SecondsToFrameTime( 0.5, PROJECT_TICK_RATE ) ) );
    // A MARKER INSIDE THE WRAP STEP, and without it this test proved nothing. With 0.1 s steps over a 1 s
    // clip, the frame that wraps covers (0.9, 1.0] u [0, 0.0] — a marker at 0.5 is nowhere near it, so
    // deleting the whole wrap-around branch left the suite green. A marker at 0.95 is reachable ONLY
    // through `n.Time > prev` on the wrapping frame, which is the half that was untested.
    ClipFixture::AddNotify( clip, "LateStep", NearestTick( SecondsToFrameTime( 0.95, PROJECT_TICK_RATE ) ) );

    animator.Play( clip, /*loop=*/true );

    int mid = 0, late = 0;
    for ( int step = 0; step < 30; ++step ) // 3.0 s => three laps
    {
        animator.Update( Timestep( 0.1F ) );
        for ( const auto& name : animator.ConsumeNotifyEvents() )
        {
            if ( name.Name == "MidStep" )
                ++mid;
            else if ( name.Name == "LateStep" )
                ++late;
        }
    }
    EXPECT_EQ( mid, 3 ) << "three laps produced " << mid << " mid-clip markers";
    EXPECT_EQ( late, 3 ) << "three laps produced " << late
                         << " end-of-clip markers; the wrap-around interval is (prev, duration) u "
                            "[0, newTime] and getting it wrong drops a step at every loop point";
}

TEST( AnimatorBlending, ScrubbingDoesNotFireNotifies )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip clip = StaticClip( "Walk", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ),
                                     /*duration=*/1.0F );
    ClipFixture::AddNotify( clip, "Footstep", NearestTick( SecondsToFrameTime( 0.5, PROJECT_TICK_RATE ) ) );

    animator.Play( clip, false );
    animator.SetTime( 0.9F ); // the Sequencer dragging the playhead past the marker

    EXPECT_TRUE( animator.ConsumeNotifyEvents().empty() )
         << "dragging the playhead fired gameplay events; scrubbing a timeline would spawn footstep VFX";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}

// ── ANV3: notify states (UE's UAnimNotifyState) and anim curves (UE's FFloatCurve) ───────────────────

namespace
{
    Animation::FrameNumber At( double seconds )
    {
        return NearestTick( SecondsToFrameTime( seconds, PROJECT_TICK_RATE ) );
    }

    /// A notify state over [from, to) seconds: an Event key with a duration on the clip's Event track.
    void AddState( AnimationClip& clip, const char* name, double from, double to )
    {
        ClipFixture::AddNotify( clip, name, At( from ),
                                Animation::FrameNumber{ At( to ).Value - At( from ).Value } );
    }

    int Count( const std::vector<Animation::NotifyEvent>& events, const char* name,
               Animation::NotifyEventKind kind )
    {
        return static_cast<int>(
             std::count( events.begin(), events.end(), Animation::NotifyEvent{ name, kind } ) );
    }

    using Kind = Animation::NotifyEventKind;
} // namespace

TEST( AnimatorBlending, ANotifyStateBeginsAndEndsOnceOnAForwardPass )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip =
         StaticClip( "Swing", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    AddState( clip, "Trail", 0.25, 0.65 );
    animator.Play( clip, /*loop=*/false );

    std::vector<Animation::NotifyEvent> events;
    for ( int step = 0; step < 20; ++step )
    {
        animator.Update( Timestep( 0.1F ) );
        if ( step == 4 ) // t = 0.5: inside the state
        {
            ASSERT_EQ( animator.GetActiveNotifyStates().size(), 1u );
            EXPECT_EQ( animator.GetActiveNotifyStates()[0].Name, "Trail" );
        }
        const auto frame = animator.ConsumeNotifyEvents();
        events.insert( events.end(), frame.begin(), frame.end() );
    }
    EXPECT_EQ( events,
               ( std::vector<Animation::NotifyEvent>{ { "Trail", Kind::Begin }, { "Trail", Kind::End } } ) );
    EXPECT_TRUE( animator.GetActiveNotifyStates().empty() );
}

TEST( AnimatorBlending, ANotifyStateBeginsAndEndsOncePerLapOfALoop )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip =
         StaticClip( "Run", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    // One state from the clip's first tick, one up to its last: the two halves a loop wrap touches.
    AddState( clip, "Early", 0.0, 0.2 );
    AddState( clip, "Late", 0.8, 1.0 );
    animator.Play( clip, /*loop=*/true );

    std::vector<Animation::NotifyEvent> events;
    for ( int step = 0; step < 25; ++step ) // 2.5 s: laps at 0, 1, 2
    {
        animator.Update( Timestep( 0.1F ) );
        const auto frame = animator.ConsumeNotifyEvents();
        events.insert( events.end(), frame.begin(), frame.end() );
    }
    EXPECT_EQ( Count( events, "Early", Kind::Begin ), 3 );
    EXPECT_EQ( Count( events, "Early", Kind::End ), 3 );
    EXPECT_EQ( Count( events, "Late", Kind::Begin ), 2 );
    EXPECT_EQ( Count( events, "Late", Kind::End ), 2 ) << "the state reaching the clip's end must End on the wrap";
}

TEST( AnimatorBlending, AScrubBackwardsEndsAStateAndFiresNoInstantNotify )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip =
         StaticClip( "Swing", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    AddState( clip, "Trail", 0.3, 0.6 );
    ClipFixture::AddNotify( clip, "Hit", At( 0.45 ) );
    animator.Play( clip, /*loop=*/false );

    animator.SetTime( 0.5F );
    EXPECT_EQ( animator.ConsumeNotifyEvents(),
               ( std::vector<Animation::NotifyEvent>{ { "Trail", Kind::Begin } } ) );
    animator.SetTime( 0.1F ); // backwards, out of the state
    EXPECT_EQ( animator.ConsumeNotifyEvents(), ( std::vector<Animation::NotifyEvent>{ { "Trail", Kind::End } } ) );
    animator.SetTime( 0.9F ); // forwards over the whole state and the marker: a scrub is not playback
    EXPECT_TRUE( animator.ConsumeNotifyEvents().empty() );
    animator.SetTime( 0.4F ); // backwards, into the state
    EXPECT_EQ( animator.ConsumeNotifyEvents(),
               ( std::vector<Animation::NotifyEvent>{ { "Trail", Kind::Begin } } ) );
}

TEST( AnimatorBlending, AStateShorterThanTheFrameStillBeginsAndEnds )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip =
         StaticClip( "Swing", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    AddState( clip, "Blip", 0.33, 0.36 );
    animator.Play( clip, /*loop=*/false );

    std::vector<Animation::NotifyEvent> events;
    for ( int step = 0; step < 10; ++step )
    {
        animator.Update( Timestep( 0.1F ) );
        const auto frame = animator.ConsumeNotifyEvents();
        events.insert( events.end(), frame.begin(), frame.end() );
    }
    EXPECT_EQ( events, ( std::vector<Animation::NotifyEvent>{ { "Blip", Kind::Begin }, { "Blip", Kind::End } } ) );
}

TEST( AnimatorBlending, PlayingAnotherClipEndsTheActiveStates )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip =
         StaticClip( "Swing", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    AddState( clip, "Trail", 0.0, 0.9 );
    const AnimationClip other = StaticClip( "Idle", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ) );
    animator.Play( clip, false );
    animator.Update( Timestep( 0.1F ) );
    EXPECT_EQ( animator.ConsumeNotifyEvents(),
               ( std::vector<Animation::NotifyEvent>{ { "Trail", Kind::Begin } } ) );
    animator.Play( other, false );
    EXPECT_EQ( animator.ConsumeNotifyEvents(), ( std::vector<Animation::NotifyEvent>{ { "Trail", Kind::End } } ) );
}

TEST( AnimatorBlending, ACurveIsSampledBetweenKeysByTheLaterKeysInterpolation )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip =
         StaticClip( "Blink", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );

    const auto curve = [&clip]( const char* name, Animation::KeyInterp interp )
    {
        std::vector<Animation::ScalarKey> keys;
        Animation::ScalarKey              first;
        first.Tick  = At( 0.0 );
        first.Value = 0.0F;
        Animation::ScalarKey last;
        last.Tick   = At( 1.0 );
        last.Value  = 10.0F;
        first.Interp = interp; // the segment's mode is its earlier key's (UE's rule, ANIM v6)
        last.Interp  = interp;
        keys         = { first, last };
        Animation::AutoSetTangents( keys, PROJECT_TICK_RATE ); // end keys are flat
        ClipFixture::AddCurve( clip, name, std::move( keys ) );
    };
    curve( "Linear", Animation::KeyInterp::Linear );
    curve( "Constant", Animation::KeyInterp::Constant );
    curve( "Cubic", Animation::KeyInterp::Cubic );
    animator.Play( clip, false );
    animator.SetTime( 0.25F );

    // A missing curve reads as NaN here, so the EXPECT_NEAR below fails instead of dereferencing nothing.
    const auto valueOf = [&animator]( const char* name )
    { return animator.GetCurveValue( name ).value_or( std::numeric_limits<float>::quiet_NaN() ); };
    ASSERT_TRUE( animator.GetCurveValue( "Linear" ).has_value() );
    EXPECT_NEAR( valueOf( "Linear" ), 2.5F, 1e-4F );
    EXPECT_NEAR( valueOf( "Constant" ), 0.0F, 1e-6F ) << "constant holds the previous key";
    // Flat tangents at both ends: the cubic is the smoothstep 10 * (3t^2 - 2t^3) at t = 0.25.
    EXPECT_NEAR( valueOf( "Cubic" ), 1.5625F, 1e-3F );
    EXPECT_FALSE( animator.GetCurveValue( "Missing" ).has_value() ) << "no curve is not the value 0";

    animator.SetTime( 1.0F );
    EXPECT_NEAR( valueOf( "Constant" ), 10.0F, 1e-6F );
}

// ── I8b-5: every graph player's notifies and curves, by its weight (UE FAnimNotifyQueue, FBlendedCurve) ─

namespace
{
    /// Events named `name` of kind `kind` reported by the graph source `node` (-1 = the base clip).
    int CountFrom( const std::vector<Animation::NotifyEvent>& events, const char* name, Kind kind, int node )
    {
        return static_cast<int>(
             std::count_if( events.begin(), events.end(), [&]( const Animation::NotifyEvent& e )
                            { return e.Name == name && e.Kind == kind && e.SourceNode == node; } ) );
    }

    /// A 1 s clip holding still, with a "Step" notify at 0.5 s.
    AnimationClip SteppingClip( const char* name )
    {
        AnimationClip clip =
             StaticClip( name, "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
        ClipFixture::AddNotify( clip, "Step", At( 0.5 ) );
        return clip;
    }

    /// Every event of ten 0.1 s ticks.
    std::vector<Animation::NotifyEvent> PlayOneSecond( Animator& animator )
    {
        std::vector<Animation::NotifyEvent> events;
        for ( int step = 0; step < 10; ++step )
        {
            animator.Update( Timestep( 0.1F ) );
            for ( Animation::NotifyEvent& e : animator.ConsumeNotifyEvents() )
                events.push_back( std::move( e ) );
        }
        return events;
    }

    /// A curve holding `value` over the clip's first second.
    void FlatCurve( AnimationClip& clip, const char* name, float value )
    {
        Animation::ScalarKey first;
        first.Tick                = At( 0.0 );
        first.Value               = value;
        Animation::ScalarKey last = first;
        last.Tick                 = At( 1.0 );
        ClipFixture::AddCurve( clip, name, { first, last } );
    }
} // namespace

TEST( AnimatorBlending, ALayerPlayersNotifyIsHeardAtFullWeightAndSilentAtZero )
{
    const Skeleton      skeleton = MakeRig();
    const AnimationClip base =
         StaticClip( "Base", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    const AnimationClip layer = SteppingClip( "Layer" );

    for ( const float weight : { 1.0F, 0.0F } )
    {
        Animator animator( skeleton );
        animator.Play( base, false );
        ASSERT_TRUE( PoseGraphFixture::Drive( animator, PoseGraphFixture::FullBodyLayer( "root" ), layer, weight,
                                              /*loop=*/false ) );
        const auto events = PlayOneSecond( animator );
        EXPECT_EQ( CountFrom( events, "Step", Kind::Fire, static_cast<int>( PoseGraphFixture::kLayerNode ) ),
                   weight > 0.0F ? 1 : 0 )
             << "weight " << weight
             << ": UE's notify queue takes every player above NotifyTriggerWeight and no "
                "other — a second player's notify was lost, or a blended-out one heard";
        EXPECT_EQ( CountFrom( events, "Step", Kind::Fire, -1 ), 0 ) << "the layer's notify was blamed on the base";
    }
}

TEST( AnimatorBlending, AnAdditivePlayerAtAlphaZeroIsSilentAndAtOneIsHeard )
{
    const Skeleton      skeleton = MakeRig();
    const AnimationClip base =
         StaticClip( "Base", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    const AnimationClip added = SteppingClip( "Additive" );

    for ( const float alpha : { 1.0F, 0.0F } )
    {
        Animator animator( skeleton );
        animator.Play( base, false );
        ASSERT_TRUE( PoseGraphFixture::Drive( animator, PoseGraphFixture::AdditiveGraph(), added, alpha,
                                              /*loop=*/false ) );
        EXPECT_EQ( CountFrom( PlayOneSecond( animator ), "Step", Kind::Fire,
                              static_cast<int>( PoseGraphFixture::kLayerNode ) ),
                   alpha > 0.0F ? 1 : 0 )
             << "alpha " << alpha << ": the additive player's weight is the node's weight x Alpha";
    }
}

TEST( AnimatorBlending, ALayerPlayersNotifyStateEndsWhenItsWeightFallsToZero )
{
    const Skeleton      skeleton = MakeRig();
    Animator            animator( skeleton );
    const AnimationClip base =
         StaticClip( "Base", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    AnimationClip layer =
         StaticClip( "Layer", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    AddState( layer, "Trail", 0.2, 0.8 );

    animator.Play( base, false );
    ASSERT_TRUE(
         PoseGraphFixture::Drive( animator, PoseGraphFixture::FullBodyLayer( "root" ), layer, 1.0F, false ) );
    const int                           node = static_cast<int>( PoseGraphFixture::kLayerNode );
    std::vector<Animation::NotifyEvent> events;
    for ( int step = 0; step < 4; ++step ) // to 0.4 s: inside the state
    {
        animator.Update( Timestep( 0.1F ) );
        for ( Animation::NotifyEvent& e : animator.ConsumeNotifyEvents() )
            events.push_back( std::move( e ) );
    }
    EXPECT_EQ( CountFrom( events, "Trail", Kind::Begin, node ), 1 );
    EXPECT_EQ( CountFrom( events, "Trail", Kind::End, node ), 0 );

    animator.SetPoseGraphParameter( "LayerWeight", 0.0F );
    animator.Update( Timestep( 0.1F ) );
    EXPECT_EQ( CountFrom( animator.ConsumeNotifyEvents(), "Trail", Kind::End, node ), 1 )
         << "a player blended out while inside a notify state left it open (UE ends it: the state is no longer "
            "in the relevant players' set)";
}

TEST( AnimatorBlending, TwoPlayersCurvesAtFullWeightNormalizeToTheirMean )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  base =
         StaticClip( "Base", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    AnimationClip layer =
         StaticClip( "Layer", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    FlatCurve( base, "Blink", 2.0F );
    FlatCurve( layer, "Blink", 6.0F );
    FlatCurve( layer, "Only", 3.0F );

    auto  graph = PoseGraphFixture::FullBodyLayer( "root" );
    auto& blend = graph.Nodes[2];
    if ( !blend.LayeredBlend )
    {
        FAIL() << "the full-body layer graph's node 2 is its Layered Blend";
    }
    blend.LayeredBlend->CurveBlend = static_cast<int>( Animation::Graph::CurveBlendOption::NormalizeByWeight );
    animator.Play( base, false );
    ASSERT_TRUE( PoseGraphFixture::Drive( animator, graph, layer, 1.0F, false ) );
    animator.Update( Timestep( 0.25F ) );

    const auto blink = animator.GetCurveValue( "Blink" );
    if ( !blink )
    {
        FAIL() << "the graph's curves never reached GetCurveValue";
    }
    EXPECT_NEAR( *blink, 4.0F, 1e-5F )
         << "base 1 and layer 1, normalized by weight: the mean — the curves were not blended with the pose";
    const auto only = animator.GetCurveValue( "Only" );
    if ( !only )
    {
        FAIL() << "a curve only the layer has was dropped";
    }
    EXPECT_NEAR( *only, 3.0F, 1e-5F );
    EXPECT_FALSE( animator.GetCurveValue( "Missing" ).has_value() );
}

// ── I8b-8: the base players are sequence players like any other — heard by their weight ────────────────

namespace
{
    /// Output Pose = LinkedAnimLayer "Call" (Weapon.UpperBody) over the base machine "Base"; unlinked, the call
    /// passes the base through at full weight.
    Animation::Graph::AnimGraph CallingHost()
    {
        namespace G = Animation::Graph;
        G::AnimGraph graph;
        graph.Name = "Host";
        graph.Nodes.push_back( PoseGraphFixture::BaseMachine() );
        G::PoseNode call;
        call.Name        = "Call";
        call.Kind        = static_cast<int>( G::PoseNodeKind::LinkedAnimLayer );
        call.PoseInputs  = { "Base" };
        call.LinkedLayer = G::LinkedAnimLayerNode{ "Weapon", "UpperBody" };
        graph.Nodes.push_back( std::move( call ) );
        graph.OutputPose = "Call";
        graph.Layers     = G::AnimGraphLayers{ { G::AnimLayerInterface{ "Weapon", { "UpperBody" } } }, {} };
        return graph;
    }

    /// Implements Weapon.UpperBody with its own sequence player and NO LinkedInputPose: the host's base is
    /// in no output any more, so its weight is 0.
    Animation::Graph::AnimGraph ReplacingLayer()
    {
        namespace G = Animation::Graph;
        G::AnimGraph graph;
        graph.Name = "Rifle";
        graph.Nodes.push_back( PoseGraphFixture::Sequence( "Main" ) );
        graph.OutputPose = "Main";
        graph.Layers     = G::AnimGraphLayers{
                 { G::AnimLayerInterface{ "Weapon", { "UpperBody" } } },
                 { G::AnimLayerGraph{ "Weapon", "UpperBody", { PoseGraphFixture::Sequence( "Own" ) }, "Own" } } };
        return graph;
    }
} // namespace

TEST( AnimatorBlending, ABasePlayerAtGraphWeightZeroIsSilentAndAtOneIsHeard )
{
    const Skeleton      skeleton = MakeRig();
    const AnimationClip base     = SteppingClip( "Base" );

    for ( const bool replaced : { false, true } )
    {
        Animator animator( skeleton );
        animator.Play( base, false );
        ASSERT_TRUE( animator.SetPoseGraph( CallingHost() ) );
        if ( replaced )
            ASSERT_TRUE( animator.LinkLayers( 7, ReplacingLayer() ) );
        EXPECT_EQ( CountFrom( PlayOneSecond( animator ), "Step", Kind::Fire, -1 ), replaced ? 0 : 1 )
             << ( replaced
                       ? "a base clip the graph no longer plays (weight 0) was heard: the Source stage stepped "
                         "its notifies before the evaluation, with no weight"
                       : "the base clip at full graph weight lost its notify" );
    }
}

TEST( AnimatorBlending, AnUnlinkReturnsTheInterfaceToTheHostsOwnLayerAndNotToPassThrough )
{
    namespace G                   = Animation::Graph;
    const Skeleton      skeleton  = MakeRig();
    const AnimationClip base      = SteppingClip( "Base" );
    G::AnimGraph        ownsLayer = CallingHost(); // UE: the AnimBlueprint implements the interface it calls
    if ( !ownsLayer.Layers )
        FAIL() << "CallingHost declares its layer interfaces";
    auto& ownLayers = *ownsLayer.Layers;
    ownLayers.Implemented.push_back(
         G::AnimLayerGraph{ "Weapon", "UpperBody", { PoseGraphFixture::Sequence( "Own" ) }, "Own" } );

    Animator animator( skeleton );
    animator.Play( base, false );
    ASSERT_TRUE( animator.SetPoseGraph( ownsLayer ) );
    ASSERT_EQ( animator.GetLinkedLayers().Layers().size(), 1U ) << "the host's own layer answers with no link";
    EXPECT_EQ( animator.GetLinkedLayers().Layers()[0].Implementation, 0U );

    ASSERT_TRUE( animator.LinkLayers( 7, ReplacingLayer() ) );
    ASSERT_EQ( animator.GetLinkedLayers().Layers().size(), 1U );
    EXPECT_EQ( animator.GetLinkedLayers().Layers()[0].Implementation, 7U ) << "a link replaces the default";

    animator.UnlinkLayers( 7 );
    ASSERT_EQ( animator.GetLinkedLayers().Layers().size(), 1U )
         << "unlinked: the interface went to pass-through instead of back to the host's own layer";
    EXPECT_EQ( animator.GetLinkedLayers().Layers()[0].Implementation, 0U );
    EXPECT_EQ( CountFrom( PlayOneSecond( animator ), "Step", Kind::Fire, -1 ), 0 )
         << "the default layer plays its own sequence, so the base under the call is at weight 0";

    ASSERT_TRUE( animator.LinkLayers( 7, ReplacingLayer() ) );
    animator.ClearLinkedLayers();
    ASSERT_EQ( animator.GetLinkedLayers().Layers().size(), 1U ) << "clearing the links keeps the defaults";

    ASSERT_TRUE( animator.SetPoseGraph( CallingHost() ) );
    EXPECT_TRUE( animator.GetLinkedLayers().Layers().empty() )
         << "a host implementing nothing has no defaults: its calls pass their input";
}

TEST( AnimatorBlending, ACrossFadesOutgoingNotifyIsHeardUntilItsWeightReachesZero )
{
    const Skeleton      skeleton = MakeRig();
    const AnimationClip outgoing = SteppingClip( "Out" ); // "Step" at 0.5 s
    AnimationClip       incoming =
         StaticClip( "In", "spine", glm::vec3( 0.0F, 30.0F, 0.0F ), glm::quat( 1, 0, 0, 0 ), 1.0F );
    ClipFixture::AddNotify( incoming, "Land", At( 0.2 ) );
    AddState( incoming, "Trail", 0.1, 0.9 );

    // 1 s: at 0.5 s the outgoing clip is at weight 0.5 — heard. 0.45 s: at 0.5 s alpha is 1 — silent.
    for ( const float duration : { 1.0F, 0.45F } )
    {
        Animator animator( skeleton );
        animator.Play( outgoing, false );
        animator.CrossFade( incoming, duration, false );
        const auto events = PlayOneSecond( animator );
        EXPECT_EQ( CountFrom( events, "Step", Kind::Fire, -1 ), duration > 0.5F ? 1 : 0 )
             << "fade " << duration
             << " s: the outgoing player is heard while (1 - alpha) is above "
                "NotifyTriggerWeight and not after";
        EXPECT_EQ( CountFrom( events, "Land", Kind::Fire, -1 ), 1 )
             << "fade " << duration << " s: the incoming player's notify at alpha 0.2 was not heard";
        EXPECT_EQ( CountFrom( events, "Trail", Kind::Begin, -1 ), 1 )
             << "fade " << duration
             << " s: the incoming clip's state must begin once, not again when it "
                "becomes the current clip";
        EXPECT_EQ( CountFrom( events, "Trail", Kind::End, -1 ), 1 ) << "fade " << duration << " s";
    }
}
