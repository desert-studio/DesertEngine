// The half of the Animator that had no test at all: CrossFade, AddLayer / layer composition,
// SetLayerMaskByNames, ResolveTrack and notify firing. `grep -rln "AddLayer\|CrossFade\|SetLayerMask"
// Desert/Tests` returned nothing before this file, while `Animator.cpp` spent 300 of its 576 lines on
// exactly those.
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

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using Common::Timestep;
using Desert::Animation::AnimationClip;
using Desert::Animation::AnimationNotify;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTrack;
namespace Animation = Desert::Animation;
using Desert::Animation::PoseStage;
using Desert::Animation::Skeleton;

namespace
{
    bool MatExact( const glm::mat4& a, const glm::mat4& b )
    {
        for ( int c = 0; c < 4; ++c )
            for ( int r = 0; r < 4; ++r )
                if ( a[c][r] != b[c][r] )
                    return false;
        return true;
    }

    bool MatNear( const glm::mat4& a, const glm::mat4& b, float eps = 1e-4f )
    {
        for ( int c = 0; c < 4; ++c )
            for ( int r = 0; r < 4; ++r )
                if ( std::fabs( a[c][r] - b[c][r] ) > eps )
                    return false;
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
                                   glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 90.0f, 0.0f ) ) ) );
        bones.push_back( MakeBone( "spine", 0u,
                                   glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 30.0f, 0.0f ) ) *
                                        glm::rotate( glm::mat4( 1.0f ), 0.35f, glm::vec3( 0, 0, 1 ) ) ) );
        bones.push_back( MakeBone( "arm", 1u,
                                   glm::translate( glm::mat4( 1.0f ), glm::vec3( 20.0f, 0.0f, 0.0f ) ) *
                                        glm::rotate( glm::mat4( 1.0f ), -0.8f, glm::vec3( 1, 0, 0 ) ) ) );
        Skeleton skeleton( std::move( bones ) );
        skeleton.RecomputeOffsetMatrices();
        return skeleton;
    }

    /// A one-key clip: `bone` sits at `position` with `rotation`, for the whole of `duration`.
    AnimationClip StaticClip( const char* name, const char* bone, const glm::vec3& position,
                              const glm::quat& rotation = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ),
                              float            duration = 2.0f )
    {
        AnimationClip clip;
        clip.AnimationName  = name;
        clip.Duration       = duration;
        clip.TicksPerSecond = 1.0f;

        BoneTrack track;
        track.BoneName = bone;
        track.PositionKeys.push_back( { 0.0f, position } );
        track.RotationKeys.push_back( { 0.0f, rotation } );
        track.ScaleKeys.push_back( { 0.0f, glm::vec3( 1.0f ) } );
        clip.Tracks.push_back( std::move( track ) );
        return clip;
    }

    /// Component-space position of a bone, recovered from the skinning matrices the way every consumer
    /// downstream of GetPose() effectively does.
    glm::vec3 BonePosition( const Animator& animator, uint32_t bone )
    {
        return glm::vec3( animator.GetBoneModelMatrix( bone )[3] );
    }
} // namespace

// ── The pipeline is a list, and its membership is data ──────────────────────────────────────────────

TEST( AnimatorBlending, TheLayerStageJoinsAndLeavesWithTheLayerStack )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip = StaticClip( "Base", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ) );

    ASSERT_EQ( animator.GetStages().size(), 1u );
    EXPECT_EQ( animator.GetStages()[0], PoseStage::Source )
         << "something has to produce a pose; Source is not optional";

    animator.AddLayer( clip, 1.0f );
    ASSERT_EQ( animator.GetStages().size(), 2u );
    EXPECT_EQ( animator.GetStages()[1], PoseStage::Layers )
         << "layers must run AFTER the source — an order this test exists to state, because the previous "
            "code expressed it only as statement order inside Update()";

    animator.ClearLayers();
    EXPECT_EQ( animator.GetStages().size(), 1u )
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
    AnimationClip a = StaticClip( "A", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ),
                                  glm::angleAxis( 0.3f, glm::normalize( glm::vec3( 1, 2, 3 ) ) ) );
    AnimationClip b = StaticClip( "B", "spine", glm::vec3( 0.0f, 80.0f, 0.0f ),
                                  glm::angleAxis( 2.4f, glm::normalize( glm::vec3( 3, -1, 2 ) ) ) );

    animator.Play( a );
    animator.SetTime( 0.5f );
    const glm::mat4 pureA = animator.GetPose().Matrices[1];

    animator.CrossFade( b, 4.0f );
    animator.Update( Timestep( 0.0f ) ); // alpha still 0

    EXPECT_TRUE( MatExact( animator.GetPose().Matrices[1], pureA ) )
         << "the first frame of a crossfade already moved the character. A blend at alpha 0 that is only "
            "APPROXIMATELY its source is a visible pop at the start of every transition";
}

TEST( AnimatorBlending, ACrossFadeAtOneIsTheTargetClip )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip a = StaticClip( "A", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ) );
    AnimationClip b = StaticClip( "B", "spine", glm::vec3( 0.0f, 80.0f, 0.0f ) );

    animator.Play( b );
    animator.SetTime( 0.0f );
    const glm::mat4 pureB = animator.GetPose().Matrices[1];

    animator.Play( a );
    animator.CrossFade( b, 1.0f );
    animator.Update( Timestep( 1.0f ) ); // alpha hits exactly 1

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
         StaticClip( "A", "arm", glm::vec3( 20.0f, 0.0f, 0.0f ), glm::angleAxis( 0.0f, glm::vec3( 0, 0, 1 ) ) );
    AnimationClip b =
         StaticClip( "B", "arm", glm::vec3( 20.0f, 0.0f, 0.0f ), glm::angleAxis( 2.2f, glm::vec3( 0, 0, 1 ) ) );

    animator.Play( a );
    animator.SetTime( 0.0f );
    const float restLength = glm::length( BonePosition( animator, 2 ) - BonePosition( animator, 1 ) );
    ASSERT_GT( restLength, 1.0f ) << "the fixture is degenerate: the arm sits on top of the spine";

    animator.CrossFade( b, 1.0f );
    for ( int step = 0; step < 10; ++step )
    {
        animator.Update( Timestep( 0.1f ) );
        const float length = glm::length( BonePosition( animator, 2 ) - BonePosition( animator, 1 ) );
        EXPECT_NEAR( length, restLength, restLength * 1e-3f )
             << "at step " << step << " the arm is " << length << " from the spine instead of " << restLength
             << "; a rotation blend moved a bone away from its parent";
    }
}

// ── Layers ──────────────────────────────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, AnOverrideLayerAtFullWeightReplacesTheBase )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip base  = StaticClip( "Base", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ) );
    AnimationClip layer = StaticClip( "Layer", "spine", glm::vec3( 0.0f, 300.0f, 0.0f ) );

    animator.Play( layer );
    animator.SetTime( 0.0f );
    const glm::mat4 pureLayer = animator.GetPose().Matrices[1];

    animator.Play( base );
    animator.AddLayer( layer, 1.0f );
    animator.Update( Timestep( 0.0f ) );

    EXPECT_TRUE( MatNear( animator.GetPose().Matrices[1], pureLayer ) )
         << "weight 1 on an override layer is not the layer, so the weight slider does not mean what it says";
}

TEST( AnimatorBlending, AnOverrideLayerAtZeroWeightChangesNothing )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip base  = StaticClip( "Base", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ) );
    AnimationClip layer = StaticClip( "Layer", "spine", glm::vec3( 0.0f, 300.0f, 0.0f ) );

    animator.Play( base );
    animator.Update( Timestep( 0.0f ) );
    const auto without = animator.GetPose().Matrices;

    animator.AddLayer( layer, 0.0f );
    animator.Update( Timestep( 0.0f ) );

    for ( size_t i = 0; i < without.size(); ++i )
        EXPECT_TRUE( MatExact( animator.GetPose().Matrices[i], without[i] ) )
             << "bone " << i << " moved under a layer of weight zero";
}

TEST( AnimatorBlending, AnAdditiveLayerAddsItsDeltaFromBindAndNotItsPose )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    // The bind local of "spine" translates (0,30,0). An additive clip putting it at (0,45,0) is a delta of
    // +15, so over a base at (0,100,0) the result must be (0,115,0) — NOT (0,45,0), which is what an
    // additive layer implemented as an override would give.
    AnimationClip base     = StaticClip( "Base", "spine", glm::vec3( 0.0f, 100.0f, 0.0f ) );
    AnimationClip additive = StaticClip( "Add", "spine", glm::vec3( 0.0f, 45.0f, 0.0f ) );

    animator.Play( base );
    animator.AddLayer( additive, 1.0f, /*additive=*/true );
    animator.Update( Timestep( 0.0f ) );

    const glm::vec3 spine = BonePosition( animator, 1 );
    const glm::vec3 root  = BonePosition( animator, 0 );
    EXPECT_NEAR( spine.y - root.y, 115.0f, 1e-3f )
         << "the additive layer contributed its POSE rather than its delta from bind";
}

TEST( AnimatorBlending, HalfWeightIsBetweenTheTwoAndOnTheSegment )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip base  = StaticClip( "Base", "spine", glm::vec3( 0.0f, 0.0f, 0.0f ) );
    AnimationClip layer = StaticClip( "Layer", "spine", glm::vec3( 0.0f, 100.0f, 0.0f ) );

    animator.Play( base );
    animator.AddLayer( layer, 0.5f );
    animator.Update( Timestep( 0.0f ) );

    const glm::vec3 spine = BonePosition( animator, 1 );
    const glm::vec3 root  = BonePosition( animator, 0 );
    EXPECT_NEAR( spine.y - root.y, 50.0f, 1e-3f ) << "a half-weight override is not halfway";
}

// ── Masks ───────────────────────────────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, ABoneOutsideTheMaskDoesNotMoveByOneBit )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip base  = StaticClip( "Base", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ) );
    AnimationClip layer = StaticClip( "Layer", "root", glm::vec3( 500.0f, 0.0f, 0.0f ) );

    animator.Play( base );
    animator.Update( Timestep( 0.0f ) );
    const Desert::Animation::BoneTransform spineWithout = animator.GetLocalPose()[1];

    const int index = animator.AddLayer( layer, 1.0f );
    animator.SetLayerMaskByNames( index, { "root" }, /*includeChildren=*/false );
    animator.Update( Timestep( 0.0f ) );

    EXPECT_FALSE( animator.IsBoneInLayerMask( index, 1 ) );

    // ASSERTED ON THE LOCAL POSE, AND THAT IS THE POINT OF THE SUBSTRATE. A mask says which bones the layer
    // may MODIFY; it cannot say which bones move on screen, because a masked-out child is still carried by a
    // masked-IN parent — here the layer shifts the root 500 units and the spine goes with it, correctly.
    // Before the local pose existed there was nothing to make that statement about: the only thing the
    // Animator kept was the skinning matrix, in which "this bone was not modified" and "this bone did not
    // move" are indistinguishable.
    const Desert::Animation::BoneTransform spineWith = animator.GetLocalPose()[1];
    EXPECT_EQ( spineWith.Translation, spineWithout.Translation )
         << "a masked-OUT bone's own transform changed; the mask is not being consulted";
    EXPECT_EQ( spineWith.Rotation, spineWithout.Rotation );
    EXPECT_EQ( spineWith.Scale, spineWithout.Scale );

    // And the negative control: the masked-IN bone did move.
    EXPECT_NE( animator.GetLocalPose()[0].Translation.x, 0.0f )
         << "the layer changed nothing at all, so the test above proves nothing";
}

TEST( AnimatorBlending, MaskingABoneMasksItsDescendantsByDefault )
{
    const Skeleton skeleton = MakeRig(); // root -> spine -> arm
    Animator       animator( skeleton );
    AnimationClip  clip = StaticClip( "Layer", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ) );

    const int index = animator.AddLayer( clip, 1.0f );
    animator.SetLayerMaskByNames( index, { "spine" } );

    EXPECT_FALSE( animator.IsBoneInLayerMask( index, 0 ) ) << "the PARENT was masked in";
    EXPECT_TRUE( animator.IsBoneInLayerMask( index, 1 ) );
    EXPECT_TRUE( animator.IsBoneInLayerMask( index, 2 ) )
         << "masking a shoulder has to mask the arm, or 'upper body' is unsayable";

    animator.SetLayerMaskByNames( index, { "spine" }, /*includeChildren=*/false );
    EXPECT_FALSE( animator.IsBoneInLayerMask( index, 2 ) );
}

TEST( AnimatorBlending, AnUnknownBoneNameMasksNothingRatherThanEverything )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );
    AnimationClip  clip  = StaticClip( "Layer", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ) );
    const int      index = animator.AddLayer( clip, 1.0f );

    animator.SetLayerMaskByNames( index, { "no_such_bone" } );
    for ( uint32_t bone = 0; bone < 3; ++bone )
        EXPECT_FALSE( animator.IsBoneInLayerMask( index, bone ) )
             << "a mask naming only bones this rig does not have came out EMPTY, and an empty mask means "
                "every bone — so a typo turns an upper-body layer into a full-body one";
}

// ── T0.4: layers through a crossfade ────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, LayersKeepRunningThroughACrossFade )
{
    // `if ( !m_IsBlending ) ApplyLayers();` under a comment calling it "that brief frame". It is not a
    // frame, it is the whole blend: a 0.5 s crossfade dropped an upper-body override for 0.5 s.
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip a     = StaticClip( "A", "spine", glm::vec3( 0.0f, 0.0f, 0.0f ) );
    AnimationClip b     = StaticClip( "B", "spine", glm::vec3( 0.0f, 10.0f, 0.0f ) );
    AnimationClip layer = StaticClip( "Layer", "spine", glm::vec3( 0.0f, 400.0f, 0.0f ) );

    animator.Play( a );
    animator.AddLayer( layer, 1.0f );
    animator.CrossFade( b, 1.0f );

    animator.Update( Timestep( 0.25f ) ); // mid-blend
    const glm::vec3 spine = BonePosition( animator, 1 );
    const glm::vec3 root  = BonePosition( animator, 0 );
    EXPECT_NEAR( spine.y - root.y, 400.0f, 1e-3f )
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
    AnimationClip clip = StaticClip( "ArmOnly", "arm", glm::vec3( 77.0f, 0.0f, 0.0f ) );

    animator.Play( clip );
    animator.Update( Timestep( 0.0f ) );

    // The DISTANCE, not the x component: the spine carries a bind rotation, so the arm's local offset of
    // (77,0,0) arrives in component space turned by it. Asserting the x difference would be asserting
    // cos(0.35) — a fact about the fixture's rotation, not about whether the track reached its bone.
    const glm::vec3 arm   = BonePosition( animator, 2 );
    const glm::vec3 spine = BonePosition( animator, 1 );
    EXPECT_NEAR( glm::length( arm - spine ), 77.0f, 1e-3f )
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

    AnimationClip  foreign = StaticClip( "Foreign", "mixamorig:Hips", glm::vec3( 999.0f, 0.0f, 0.0f ) );
    const Animator rest( skeleton );

    animator.Play( foreign );
    animator.Update( Timestep( 0.0f ) );

    for ( size_t i = 0; i < animator.GetPose().Matrices.size(); ++i )
        EXPECT_TRUE( MatNear( animator.GetPose().Matrices[i], rest.GetPose().Matrices[i] ) )
             << "bone " << i << " moved for a clip that animates no bone this rig has";
}

TEST( AnimatorBlending, TheTrackCacheIsRebuiltWhenTheClipsStorageMoves )
{
    // The clip keeps its address while its Tracks vector is freed and reallocated — which is exactly what
    // asset eviction plus reload does, and what segfaulted inside lower_bound when the cache was keyed on
    // the address alone.
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip clip = StaticClip( "Live", "spine", glm::vec3( 0.0f, 10.0f, 0.0f ) );
    animator.Play( clip );
    animator.Update( Timestep( 0.0f ) );
    EXPECT_NEAR( BonePosition( animator, 1 ).y - BonePosition( animator, 0 ).y, 10.0f, 1e-3f );

    // THE RELOADED CLIP DRIVES A DIFFERENT BONE, and that is what makes this test able to fail. Reloading
    // the same bone with a different value is not enough: the stale pointer aims at freed memory, and
    // whether reading it yields the old value, the new one, or rubbish is the ALLOCATOR's business — with
    // a same-sized reallocation it lands on the very block just freed and reports the new value, so the
    // test passes with the cache-invalidation check deleted. Written this way, the stale binding maps
    // "arm" to the nullptr the OLD clip's track list gave it, so the arm simply does not move, and that
    // is a fact about the binding rather than about the heap.
    // Exactly what an evicted-then-reloaded asset does to the clip it owns: the AnimationClip keeps its
    // address, its track list is freed and rebuilt, and `AnimationAsset` stamps the new generation.
    clip.Tracks.clear();
    clip.Tracks.shrink_to_fit();

    AnimationClip reloaded = StaticClip( "Live", "arm", glm::vec3( 250.0f, 0.0f, 0.0f ) );
    clip.Tracks            = std::move( reloaded.Tracks );
    ++clip.TrackRevision;

    animator.Update( Timestep( 0.0f ) );
    EXPECT_NEAR( glm::length( BonePosition( animator, 2 ) - BonePosition( animator, 1 ) ), 250.0f, 1e-3f )
         << "the reloaded clip's track never reached its bone: the cache was keyed on the clip's ADDRESS, "
            "which an unload+reload does not change, so it kept the binding built against storage that has "
            "been returned to the allocator";
}

// ── Notifies ────────────────────────────────────────────────────────────────────────────────────────

TEST( AnimatorBlending, ANotifyFiresExactlyOncePerPassOverItsTime )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip clip = StaticClip( "Walk", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ), glm::quat( 1, 0, 0, 0 ),
                                     /*duration=*/1.0f );
    clip.Notifies.push_back( AnimationNotify{ "Footstep", 0.5f } );

    animator.Play( clip, /*loop=*/false );

    int fired = 0;
    for ( int step = 0; step < 20; ++step ) // 20 x 0.1 s = 2 s over a 1 s clip
    {
        animator.Update( Timestep( 0.1f ) );
        fired += static_cast<int>( animator.ConsumeNotifies().size() );
    }
    EXPECT_EQ( fired, 1 ) << "a marker at 0.5 s fired " << fired << " times in one non-looping pass";
}

TEST( AnimatorBlending, ALoopingClipFiresItsNotifyOncePerLap )
{
    const Skeleton skeleton = MakeRig();
    Animator       animator( skeleton );

    AnimationClip clip = StaticClip( "Run", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ), glm::quat( 1, 0, 0, 0 ),
                                     /*duration=*/1.0f );
    clip.Notifies.push_back( AnimationNotify{ "MidStep", 0.5f } );
    // A MARKER INSIDE THE WRAP STEP, and without it this test proved nothing. With 0.1 s steps over a 1 s
    // clip, the frame that wraps covers (0.9, 1.0] u [0, 0.0] — a marker at 0.5 is nowhere near it, so
    // deleting the whole wrap-around branch left the suite green. A marker at 0.95 is reachable ONLY
    // through `n.Time > prev` on the wrapping frame, which is the half that was untested.
    clip.Notifies.push_back( AnimationNotify{ "LateStep", 0.95f } );

    animator.Play( clip, /*loop=*/true );

    int mid = 0, late = 0;
    for ( int step = 0; step < 30; ++step ) // 3.0 s => three laps
    {
        animator.Update( Timestep( 0.1f ) );
        for ( const auto& name : animator.ConsumeNotifies() )
        {
            if ( name == "MidStep" )
                ++mid;
            else if ( name == "LateStep" )
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

    AnimationClip clip = StaticClip( "Walk", "spine", glm::vec3( 0.0f, 30.0f, 0.0f ), glm::quat( 1, 0, 0, 0 ),
                                     /*duration=*/1.0f );
    clip.Notifies.push_back( AnimationNotify{ "Footstep", 0.5f } );

    animator.Play( clip, false );
    animator.SetTime( 0.9f ); // the Sequencer dragging the playhead past the marker

    EXPECT_TRUE( animator.ConsumeNotifies().empty() )
         << "dragging the playhead fired gameplay events; scrubbing a timeline would spawn footstep VFX";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
