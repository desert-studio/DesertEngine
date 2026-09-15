// The pose substrate: LocalPose / ComponentPose / SkinningMatrices, the skeleton's single parent-chain
// walk, and the one-bone-index-space invariant.
//
// WHAT THESE TESTS ARE FOR, because it decides how they are written. The substrate replaced eight
// hand-written copies of the same chain walk plus a ninth that scanned the whole bone array for children.
// A test per function would pass on all nine copies and on the one that replaced them, and would have
// passed on the DISAGREEMENT between them — which is what was actually wrong: for a bone whose
// ParentBoneID pointed outside the array, the bind-pose walk treated it as a root while the playback walk
// never visited it at all. So what is asserted here is RELATIONS: that the resolver agrees with itself
// through every entry point, that bind reached two ways is one answer, and that a rig the old code could
// not survive now has a defined result and says what is wrong with it.

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>

using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneRef;
using Desert::Animation::BoneTransform;
using Desert::Animation::ComponentPose;
using Desert::Animation::LocalPose;
using Desert::Animation::Skeleton;

namespace
{
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

    /// A four-bone chain with a NON-IDENTITY bind at every level — translation, rotation and scale all
    /// non-trivial, so `chainGlobal * OffsetMatrix` is nowhere near the identity. Our only committed probe
    /// rig (SkinProbe.skeleton) is one bone with two identity matrices, which is exactly the rig on which
    /// the skinning-space blend defect is invisible.
    Skeleton MakeRichChain()
    {
        std::vector<BoneInfo> bones;
        bones.push_back( MakeBone( "root", std::nullopt,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 3.0F, 11.0F, -2.0F ) ) ) );
        bones.push_back( MakeBone( "spine", 0U,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 40.0F, 0.0F ) ) *
                                        glm::rotate( glm::mat4( 1.0F ), 0.4F, glm::vec3( 0, 0, 1 ) ) ) );
        bones.push_back( MakeBone( "upper_arm", 1U,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 17.0F, 6.0F, 0.0F ) ) *
                                        glm::rotate( glm::mat4( 1.0F ), -0.9F, glm::vec3( 1, 0, 0 ) ) *
                                        glm::scale( glm::mat4( 1.0F ), glm::vec3( 1.3F, 1.3F, 1.3F ) ) ) );
        bones.push_back( MakeBone( "lower_arm", 2U,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, -23.0F, 4.0F ) ) *
                                        glm::rotate( glm::mat4( 1.0F ), 0.6F, glm::vec3( 0, 1, 0 ) ) ) );
        Skeleton skeleton( std::move( bones ) );
        skeleton.RecomputeOffsetMatrices();
        return skeleton;
    }

    /// The same four bones with the PARENTS DECLARED AFTER THEIR CHILDREN in the array. Bone array order is
    /// arbitrary in this engine (`Skeleton.hpp` said so and every one of the eight walks memoised precisely
    /// because of it), so any resolver has to produce the identical rig from either listing.
    Skeleton MakeRichChainReversed()
    {
        std::vector<BoneInfo> bones;
        bones.push_back( MakeBone( "lower_arm", 1U,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, -23.0F, 4.0F ) ) *
                                        glm::rotate( glm::mat4( 1.0F ), 0.6F, glm::vec3( 0, 1, 0 ) ) ) );
        bones.push_back( MakeBone( "upper_arm", 2U,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 17.0F, 6.0F, 0.0F ) ) *
                                        glm::rotate( glm::mat4( 1.0F ), -0.9F, glm::vec3( 1, 0, 0 ) ) *
                                        glm::scale( glm::mat4( 1.0F ), glm::vec3( 1.3F, 1.3F, 1.3F ) ) ) );
        bones.push_back( MakeBone( "spine", 3U,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 40.0F, 0.0F ) ) *
                                        glm::rotate( glm::mat4( 1.0F ), 0.4F, glm::vec3( 0, 0, 1 ) ) ) );
        bones.push_back( MakeBone( "root", std::nullopt,
                                   glm::translate( glm::mat4( 1.0F ), glm::vec3( 3.0F, 11.0F, -2.0F ) ) ) );
        Skeleton skeleton( std::move( bones ) );
        skeleton.RecomputeOffsetMatrices();
        return skeleton;
    }

    /// The chain walk exactly as it was written, eight times over, before the substrate. Kept here on
    /// purpose: a rewrite claiming to "absorb" duplicates has to be shown to compute the same thing they
    /// did, and quoting the old body is the only way to show it.
    ///
    /// It quotes the GLOBALS, before the multiply by OffsetMatrix, and that is the load-bearing detail:
    /// the product cancels a broken resolver against itself, so comparing the offset-multiplied result
    /// would pass for a resolver that is wrong (see TheOneResolverAgreesWithTheEightItReplaced). A second
    /// quotation that DID multiply by the offset used to sit below this one, unused — a dead reference
    /// implementation beside a live one is an invitation to compare against the wrong one, so it is gone.
    std::vector<glm::mat4> LegacyChainGlobals( const std::vector<BoneInfo>& bones )
    {
        std::vector<glm::mat4>             global( bones.size(), glm::mat4( 1.0F ) );
        std::vector<bool>                  done( bones.size(), false );
        std::function<glm::mat4( size_t )> resolve = [&]( size_t i ) -> glm::mat4
        {
            if ( done[i] )
                return global[i];
            glm::mat4 m = bones[i].LocalBindTransform;
            if ( bones[i].ParentBoneID.has_value() && bones[i].ParentBoneID.value() < bones.size() )
                m = resolve( bones[i].ParentBoneID.value() ) * bones[i].LocalBindTransform;
            global[i] = m;
            done[i]   = true;
            return m;
        };
        for ( size_t i = 0; i < bones.size(); ++i )
        {
            resolve( i );
        }
        return global;
    }
} // namespace

// ── BoneTransform: the decomposition that used to straighten a mirror in silence ────────────────────

TEST( PoseSubstrate, AnOrdinaryTransformSurvivesTheRoundTrip )
{
    const glm::mat4 m = glm::translate( glm::mat4( 1.0F ), glm::vec3( 4.0F, -9.0F, 2.5F ) ) *
                        glm::rotate( glm::mat4( 1.0F ), 1.1F, glm::normalize( glm::vec3( 1, 2, 3 ) ) ) *
                        glm::scale( glm::mat4( 1.0F ), glm::vec3( 2.0F, 0.5F, 1.25F ) );

    const auto trs = BoneTransform::FromMatrix( m );
    ASSERT_TRUE( trs.IsSuccess() ) << trs.GetError();
    EXPECT_TRUE( MatNear( trs.GetValue().ToMatrix(), m ) );
}

TEST( PoseSubstrate, AMirroredTransformIsRefusedInsteadOfStraightened )
{
    // Negative X scale: a real mirrored bone. The old decomposition took scale as the LENGTH of each basis
    // column, so this came back with scale +1 and the sign folded into the rotation as a turn that does not
    // exist — on EVERY blend, with no way for a caller to tell.
    const glm::mat4 mirrored = glm::scale( glm::mat4( 1.0F ), glm::vec3( -1.0F, 1.0F, 1.0F ) );

    const auto trs = BoneTransform::FromMatrix( mirrored );
    EXPECT_FALSE( trs.IsSuccess() ) << "a mirrored basis was silently rectified into a rotation";
    EXPECT_NE( trs.GetError().find( "determinant" ), std::string::npos ) << trs.GetError();
}

TEST( PoseSubstrate, ADegenerateTransformIsRefusedRatherThanProducingNaNs )
{
    // A collapsed axis divides by zero in the normalise step and hands back a quaternion of NaNs, which
    // then propagates through every blend downstream of it.
    const glm::mat4 flat = glm::scale( glm::mat4( 1.0F ), glm::vec3( 1.0F, 0.0F, 1.0F ) );
    EXPECT_FALSE( BoneTransform::FromMatrix( flat ).IsSuccess() );
}

// ── The resolver: one walk, and it agrees with the eight it replaced ────────────────────────────────

TEST( PoseSubstrate, TheOneResolverAgreesWithTheEightItReplaced )
{
    // ON THE REVERSED RIG, AND COMPARING GLOBALS RATHER THAN SKINNING MATRICES. Both of those are the
    // result of a surviving mutation: replacing the parent-before-child resolve order with a flat 0..n
    // loop left this suite GREEN, because (a) the forward rig happens to list parents first, and (b) the
    // skinning matrix is `global * OffsetMatrix` and OffsetMatrix is computed by the SAME resolver, so a
    // broken resolver cancels itself out and every bind pose comes back as the identity either way. A
    // test that cannot fail when the thing it names is broken is not evidence.
    const Skeleton skeleton = MakeRichChainReversed();

    std::vector<glm::mat4> mine;
    skeleton.ResolveComponentSpace( [&skeleton]( uint32_t i )
                                    { return skeleton.GetBones()[i].LocalBindTransform; }, mine );

    const std::vector<glm::mat4> legacy = LegacyChainGlobals( skeleton.GetBones() );

    ASSERT_EQ( mine.size(), legacy.size() );
    for ( size_t i = 0; i < mine.size(); ++i )
    {
        EXPECT_TRUE( MatNear( mine[i], legacy[i] ) )
             << "bone " << i << " ('" << skeleton.GetBones()[i].Name
             << "') resolves differently from the memoised recursion this replaced";
    }
}

TEST( PoseSubstrate, BoneArrayOrderDoesNotChangeTheRig )
{
    const Skeleton forward  = MakeRichChain();
    const Skeleton backward = MakeRichChainReversed();

    // Same rig, listed differently: the signature is order-independent by construction and so, therefore,
    // must the resolved pose be — matched up by NAME, because the indices differ between the two.
    EXPECT_EQ( forward.GetSignature(), backward.GetSignature() );

    // COMPONENT-SPACE globals, not skinning matrices: at bind the latter are the identity by construction
    // whatever the resolver did, so they cannot tell two orderings apart.
    std::vector<glm::mat4> a, b;
    forward.ResolveComponentSpace( [&forward]( uint32_t i ) { return forward.GetBones()[i].LocalBindTransform; },
                                   a );
    backward.ResolveComponentSpace( [&backward]( uint32_t i )
                                    { return backward.GetBones()[i].LocalBindTransform; }, b );

    for ( const auto& bone : forward.GetBones() )
    {
        const auto ia = forward.FindBoneIndex( bone.Name );
        const auto ib = backward.FindBoneIndex( bone.Name );
        ASSERT_TRUE( ia && ib ) << bone.Name;
        EXPECT_TRUE( MatNear( a[*ia], b[*ib] ) ) << "bone '" << bone.Name << "'";
    }
}

TEST( PoseSubstrate, TheResolveOrderPutsEveryParentBeforeItsChild )
{
    const Skeleton skeleton = MakeRichChainReversed();
    const auto&    order    = skeleton.GetResolveOrder();

    ASSERT_EQ( order.size(), skeleton.GetBones().size() )
         << "the resolve order does not cover the rig, so some bone would never be resolved at all";

    std::vector<bool> seen( skeleton.GetBones().size(), false );
    for ( const uint32_t bone : order )
    {
        const uint32_t parent = skeleton.ResolveParent( bone );
        if ( parent != Skeleton::NO_PARENT )
            EXPECT_TRUE( seen[parent] ) << "bone " << bone << " is resolved before its parent " << parent
                                        << ", so it would multiply against an unwritten matrix";
        seen[bone] = true;
    }
}

// ── The two shapes of a broken rig, which the old tree answered twice or not at all ─────────────────

TEST( PoseSubstrate, AParentOutsideTheArrayResolvesAsARootAndIsReported )
{
    std::vector<BoneInfo> bones;
    bones.push_back( MakeBone( "root", std::nullopt, glm::mat4( 1.0F ) ) );
    bones.push_back(
         MakeBone( "orphan", 99U, glm::translate( glm::mat4( 1.0F ), glm::vec3( 5.0F, 0.0F, 0.0F ) ) ) );
    Skeleton skeleton( std::move( bones ) );

    EXPECT_EQ( skeleton.ResolveParent( 1 ), Skeleton::NO_PARENT );
    EXPECT_NE( skeleton.GetStructureError().find( "orphan" ), std::string::npos )
         << "a bone naming a parent that does not exist produced no diagnostic: " << skeleton.GetStructureError();

    // And the pose is DEFINED. The playback walk used to descend only from genuine roots, so this bone's
    // slot kept whatever the previous frame left there.
    const auto bind = LocalPose::FromBindPose( skeleton );
    ASSERT_TRUE( bind.IsSuccess() );
    ComponentPose component( skeleton, bind.GetValue() );
    EXPECT_FLOAT_EQ( component.Get( 1 )[3].x, 5.0F );
}

TEST( PoseSubstrate, AParentCycleTerminatesInsteadOfExhaustingTheStack )
{
    // The memoised recursion set its `done` flag on the way OUT, so a cycle recursed until the stack ended.
    // A crash is not a defined answer, and no test could have caught it because no test could survive it.
    std::vector<BoneInfo> bones;
    bones.push_back( MakeBone( "a", 1U, glm::mat4( 1.0F ) ) );
    bones.push_back( MakeBone( "b", 0U, glm::mat4( 1.0F ) ) );
    Skeleton skeleton( std::move( bones ) );

    EXPECT_EQ( skeleton.GetResolveOrder().size(), 2U );
    EXPECT_NE( skeleton.GetStructureError().find( "cycle" ), std::string::npos ) << skeleton.GetStructureError();

    std::vector<glm::mat4> out;
    skeleton.WriteBindSkinningMatrices( out ); // must return at all
    EXPECT_EQ( out.size(), 2U );
}

TEST( PoseSubstrate, TwoBonesWithOneNameIsReportedRatherThanResolvedAtRandom )
{
    std::vector<BoneInfo> bones;
    bones.push_back( MakeBone( "hand", std::nullopt, glm::mat4( 1.0F ) ) );
    bones.push_back( MakeBone( "hand", 0U, glm::mat4( 1.0F ) ) );
    Skeleton skeleton( std::move( bones ) );

    // FIRST WINS, which is what the linear scan did, so no caller's behaviour changes...
    EXPECT_EQ( skeleton.FindBoneIndex( "hand" ), std::optional<uint32_t>( 0U ) );
    // ...but the rig now says it has an ambiguity, which the scan could never report.
    EXPECT_NE( skeleton.GetStructureError().find( "named 'hand'" ), std::string::npos )
         << skeleton.GetStructureError();
}

// ── ComponentPose: laziness that is real, not claimed ───────────────────────────────────────────────

TEST( PoseSubstrate, ResolvingOneBoneConvertsItsChainAndNothingElse )
{
    const Skeleton skeleton = MakeRichChain(); // root -> spine -> upper_arm -> lower_arm
    const auto     bind     = LocalPose::FromBindPose( skeleton );
    ASSERT_TRUE( bind.IsSuccess() );

    ComponentPose component( skeleton, bind.GetValue() );
    for ( uint32_t i = 0; i < 4; ++i )
    {
        EXPECT_FALSE( component.Converted( i ) ) << "bone " << i << " was converted before anyone asked";
    }

    (void)component.Get( 2 ); // upper_arm

    EXPECT_TRUE( component.Converted( 0 ) );
    EXPECT_TRUE( component.Converted( 1 ) );
    EXPECT_TRUE( component.Converted( 2 ) );
    EXPECT_FALSE( component.Converted( 3 ) )
         << "a DESCENDANT was converted for a query about its ancestor; the whole point of the lazy view is "
            "that one bone costs its depth and not the rig";
}

TEST( PoseSubstrate, LazyAndEagerResolutionAreTheSameAnswer )
{
    const Skeleton skeleton = MakeRichChainReversed();
    const auto     bind     = LocalPose::FromBindPose( skeleton );
    ASSERT_TRUE( bind.IsSuccess() );

    ComponentPose lazily( skeleton, bind.GetValue() );
    ComponentPose eagerly( skeleton, bind.GetValue() );
    eagerly.ConvertAll();

    for ( uint32_t i = 0; i < skeleton.GetBones().size(); ++i )
    {
        EXPECT_TRUE( MatNear( lazily.Get( i ), eagerly.Get( i ) ) ) << "bone " << i;
    }
}

TEST( PoseSubstrate, AskingForOneBoneTwiceDoesNotMoveIt )
{
    const Skeleton skeleton = MakeRichChain();
    const auto     bind     = LocalPose::FromBindPose( skeleton );
    ASSERT_TRUE( bind.IsSuccess() );

    ComponentPose   component( skeleton, bind.GetValue() );
    const glm::mat4 first = component.Get( 3 );
    // Converting an already-converted bone by multiplying its parent in AGAIN is the obvious way to get
    // this wrong, and it compounds silently: the bone drifts a little further from its parent each frame.
    const glm::mat4 second = component.Get( 3 );
    EXPECT_TRUE( MatNear( first, second ) );
}

// ── The relation the substrate is most able to break: two answers for "the rig at rest" ─────────────

TEST( PoseSubstrate, TheAnimatorsRestPoseIsTheSkeletonsBindPose )
{
    // Two INDEPENDENT paths to the same picture. The skeleton resolves the raw bind matrices; the Animator
    // decomposes them into a LocalPose and resolves that. If they diverge, a mesh visibly jumps the moment
    // an AnimationComponent is added to it — which is the render path's own `if (animator) ... else bind`.
    const Skeleton skeleton = MakeRichChain();

    std::vector<glm::mat4> fromSkeleton;
    skeleton.WriteBindSkinningMatrices( fromSkeleton );

    const Animator animator( skeleton );
    const auto&    fromAnimator = animator.GetPose().Matrices;

    ASSERT_EQ( fromSkeleton.size(), fromAnimator.size() );
    for ( size_t i = 0; i < fromSkeleton.size(); ++i )
    {
        EXPECT_TRUE( MatNear( fromSkeleton[i], fromAnimator[i] ) )
             << "bone " << i << " ('" << skeleton.GetBones()[i].Name
             << "') is in a different place depending on which of the two bind routes drew it";
    }
}

TEST( PoseSubstrate, TheRestPoseSkinsTheMeshWhereItWasAuthored )
{
    // OffsetMatrix is the inverse of the bind chain, so the rest pose's skinning matrix is the identity per
    // bone. That relation — and not the matrices themselves — is what makes a rig with no clip render at
    // its authored size instead of at raw-vertex scale (thousands of units).
    const Skeleton         skeleton = MakeRichChain();
    std::vector<glm::mat4> skin;
    skeleton.WriteBindSkinningMatrices( skin );

    for ( size_t i = 0; i < skin.size(); ++i )
    {
        EXPECT_TRUE( MatNear( skin[i], glm::mat4( 1.0F ) ) ) << "bone " << i;
    }
}

// ── T1.3: one bone index space ──────────────────────────────────────────────────────────────────────

TEST( PoseSubstrate, AMeshSkinnedToABoneTheRigDoesNotHaveIsRefusedByName )
{
    const Skeleton skeleton = MakeRichChain(); // 4 bones, valid indices are 0..3

    EXPECT_TRUE( skeleton.ValidateBoneIndexSpace( { 0, 1, 2, 3, 0 }, "Probe.skmesh" ).IsSuccess() );

    const auto refused = skeleton.ValidateBoneIndexSpace( { 0, 7, 2 }, "Probe.skmesh" );
    ASSERT_FALSE( refused.IsSuccess() )
         << "a vertex skinned to bone 7 of a 4-bone rig was accepted; at render time that reads past the "
            "end of the pose array";
    EXPECT_NE( refused.GetError().find( "Probe.skmesh" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( "7" ), std::string::npos ) << refused.GetError();
}

TEST( PoseSubstrate, EveryIndexSpaceIsTheSameIndexSpace )
{
    // The invariant stated directly: a name resolves to ONE index, and that index addresses the skeleton's
    // bone array, the pose, and the skinning matrices identically. UE needs a per-mesh linkup table and
    // three index types because this is false there; it is true here, and it was guarded by nothing.
    const Skeleton skeleton = MakeRichChain();
    const Animator animator( skeleton );

    for ( uint32_t i = 0; i < skeleton.GetBones().size(); ++i )
    {
        const auto byName = skeleton.FindBoneIndex( skeleton.GetBones()[i].Name );
        ASSERT_TRUE( byName.has_value() );
        EXPECT_EQ( *byName, i );
    }

    EXPECT_EQ( animator.GetPose().Matrices.size(), skeleton.GetBones().size() );
    EXPECT_EQ( animator.GetLocalPose().Size(), skeleton.GetBones().size() );
}

// ── BoneRef: two validity predicates, because the two failures need different fixes ─────────────────

TEST( PoseSubstrate, ABoneRefDistinguishesUnauthoredFromUnresolvable )
{
    const Skeleton skeleton = MakeRichChain();

    BoneRef unauthored;
    EXPECT_FALSE( unauthored.HasName() );
    EXPECT_FALSE( unauthored.Resolve( skeleton ) );

    BoneRef wrongRig( "left_wing" );
    EXPECT_TRUE( wrongRig.HasName() ) << "the field IS filled in — it names a bone this rig does not have, "
                                         "which is a different report to the user than an empty field";
    EXPECT_FALSE( wrongRig.Resolve( skeleton ) );

    BoneRef good( "upper_arm" );
    EXPECT_TRUE( good.Resolve( skeleton ) );
    EXPECT_EQ( good.GetIndex(), 2U );

    // Renaming drops the cached index. Keeping it is the classic stale-cache defect: the ref reports
    // resolved, and hands back the OLD bone.
    good.SetName( "root" );
    EXPECT_FALSE( good.IsResolved() );
    EXPECT_TRUE( good.Resolve( skeleton ) );
    EXPECT_EQ( good.GetIndex(), 0U );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
