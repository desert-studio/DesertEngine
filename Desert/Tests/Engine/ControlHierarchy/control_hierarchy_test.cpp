// CONTROLS, AND THE ONE CLAIM THE TIER EXISTS FOR.
//
// T5.1 is worth doing because of what it makes cheap: "space switching is then a weight assignment"
// (report 01 §7). That sentence is only true if there is no second mechanism — no `SwitchParent` beside
// the weights — so the suite asserts it as an EQUIVALENCE: switching by assigning weights and switching
// by the named call produce the same pose, because they are the same call.
//
// The rest are the invariants the report says get lost:
//   * a global that is written and read back is the same transform, because nothing global is STORED
//     (the local/global "never both dirty" invariant, held here by there being only one authored side);
//   * dirtying is PROPAGATED to dependents and not blanket (§(c)1);
//   * the offset is inverted on the one write path, so authoring it does not make the control jump
//     somewhere the animator did not ask for (§(c)3);
//   * a control with no space is REFUSED rather than answered with identity.

#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Skeleton.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <utility>

using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTransform;
using Desert::Animation::ComponentPose;
using Desert::Animation::ControlElement;
using Desert::Animation::ControlHierarchy;
using Desert::Animation::ControlSpace;
using Desert::Animation::ControlSpaceKind;
using Desert::Animation::LocalPose;
using Desert::Animation::Skeleton;

namespace
{
    /// Two bones, side by side under one root, so a control can be switched between two real spaces.
    Skeleton MakeRig()
    {
        std::vector<BoneInfo> bones;
        BoneInfo              root;
        root.Name = "root";
        bones.push_back( root );

        BoneInfo chest;
        chest.Name         = "chest";
        chest.ParentBoneID = 0;
        bones.push_back( chest );

        BoneInfo world;
        world.Name         = "prop";
        world.ParentBoneID = 0;
        bones.push_back( world );

        return Skeleton( std::move( bones ) );
    }

    LocalPose PoseWith( const glm::vec3& chest, const glm::vec3& prop )
    {
        LocalPose pose;
        pose.Resize( 3 );
        pose[1].Translation = chest;
        pose[2].Translation = prop;
        return pose;
    }

    ControlElement MakeControl( const char* name, std::vector<ControlSpace> parents )
    {
        ControlElement element;
        element.Name    = name;
        element.Parents = std::move( parents );
        return element;
    }

    [[nodiscard]] glm::vec3 PositionOf( const glm::mat4& m )
    {
        return { m[3].x, m[3].y, m[3].z };
    }

    /// Adds a control and hands back its index. A NAMED RESULT AND A CHECK, because `ResultStr`'s rvalue
    /// `GetValue()` is DELETED on purpose: `Foo().GetValue()` has no variable to guard, so the project
    /// makes the unguardable form a compile error rather than a run-time report. This is the guarded form,
    /// written once so nine call sites do not each write it.
    uint32_t MustAdd( ControlHierarchy& rig, ControlElement element )
    {
        auto added = rig.Add( std::move( element ) );
        EXPECT_TRUE( added.IsSuccess() ) << added.GetError();
        return added.IsSuccess() ? added.GetValue() : ControlHierarchy::INVALID;
    }
} // namespace

TEST( ControlHierarchyTest, AControlIsOffsetPlusPoseInItsParentSpace )
{
    const Skeleton skeleton = MakeRig();
    LocalPose      local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose  pose( skeleton, local );

    ControlHierarchy rig;
    ControlElement   hand   = MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } } );
    hand.Offset.Translation = glm::vec3( 0.0F, 0.0F, 10.0F ); // the rigger's zero
    const auto added        = rig.Add( hand );
    ASSERT_TRUE( added.IsSuccess() ) << added.GetError();
    const uint32_t control = added.GetValue();

    rig.Evaluate( skeleton, pose );

    // Pose is identity, so the control sits exactly on its offset inside the chest's space.
    EXPECT_EQ( PositionOf( rig.GetGlobalTransform( control ) ), glm::vec3( 0.0F, 100.0F, 10.0F ) );

    // The ANIMATED value moves it from there, and the offset is still underneath.
    BoneTransform animated;
    animated.Translation = glm::vec3( 5.0F, 0.0F, 0.0F );
    ASSERT_TRUE( rig.SetPose( control, animated ).IsSuccess() );
    EXPECT_EQ( PositionOf( rig.GetGlobalTransform( control ) ), glm::vec3( 5.0F, 100.0F, 10.0F ) );

    // And the control FOLLOWS ITS BONE: re-pose the skeleton, re-evaluate, the control has moved with it
    // without its own pose changing.
    local[1].Translation = glm::vec3( 0.0F, 130.0F, 0.0F );
    pose.Invalidate();
    rig.Evaluate( skeleton, pose );
    EXPECT_EQ( PositionOf( rig.GetGlobalTransform( control ) ), glm::vec3( 5.0F, 130.0F, 10.0F ) );
    EXPECT_EQ( rig.Get( control ).Pose.Translation, glm::vec3( 5.0F, 0.0F, 0.0F ) ) << "the pose was rewritten";
}

TEST( ControlHierarchyTest, AGlobalWrittenIsTheGlobalReadBackAndNothingGlobalIsStored )
{
    const Skeleton skeleton = MakeRig();
    LocalPose      local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose  pose( skeleton, local );

    ControlHierarchy rig;
    ControlElement   hand   = MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } } );
    hand.Offset.Translation = glm::vec3( 0.0F, 0.0F, 10.0F );
    hand.Offset.Rotation    = glm::angleAxis( glm::radians( 30.0F ), glm::vec3( 0.0F, 1.0F, 0.0F ) );
    const uint32_t control  = MustAdd( rig, hand );
    rig.Evaluate( skeleton, pose );

    // THE DRAG PATH: a place on screen, back-solved through the parent space AND the offset.
    glm::mat4 wanted = glm::translate( glm::mat4( 1.0F ), glm::vec3( 12.0F, 34.0F, -56.0F ) );
    wanted           = glm::rotate( wanted, glm::radians( 25.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) );
    ASSERT_TRUE( rig.SetGlobalTransform( control, wanted ).IsSuccess() );

    const glm::mat4 back = rig.GetGlobalTransform( control );
    for ( int column = 0; column < 4; ++column )
    {
        for ( int row = 0; row < 4; ++row )
        {
            EXPECT_NEAR( back[column][row], wanted[column][row], 1e-3F ) << "column " << column << " row " << row;
        }
    }

    // AND THE BONE STILL OWNS IT. Nothing global was stored, so re-posing the skeleton moves the control
    // with it — a stored global would leave it behind, which is the defect this invariant exists to stop.
    local[1].Translation += glm::vec3( 0.0F, 7.0F, 0.0F );
    pose.Invalidate();
    rig.Evaluate( skeleton, pose );
    EXPECT_NEAR( PositionOf( rig.GetGlobalTransform( control ) ).y, 34.0F + 7.0F, 1e-3F )
         << "the control did not follow its space, so something global was kept";
}

TEST( ControlHierarchyTest, SwitchingSpaceIsAWeightAssignmentAndKeepsTheControlWhereItWas )
{
    const Skeleton skeleton = MakeRig();
    LocalPose      local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose  pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   hand =
         MustAdd( rig, MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F },
                                                   ControlSpace{ ControlSpaceKind::Bone, 2, 0.0F } } ) );
    rig.Evaluate( skeleton, pose );

    const glm::mat4 beforeSwitch = rig.GetGlobalTransform( hand );
    EXPECT_EQ( PositionOf( beforeSwitch ), glm::vec3( 0.0F, 100.0F, 0.0F ) ) << "not in the chest's space";

    // THE SWITCH. Two numbers, and the control does not move on screen.
    ASSERT_TRUE( rig.SetSpaceWeights( hand, { 0.0F, 1.0F }, /*keepWorld=*/true ).IsSuccess() );
    const glm::mat4 afterSwitch = rig.GetGlobalTransform( hand );
    EXPECT_NEAR( PositionOf( afterSwitch ).x, PositionOf( beforeSwitch ).x, 1e-3F );
    EXPECT_NEAR( PositionOf( afterSwitch ).y, PositionOf( beforeSwitch ).y, 1e-3F );

    // ...but it IS in the other space now, which the pose proves: it had to change to stay put.
    EXPECT_NEAR( rig.Get( hand ).Pose.Translation.x, -50.0F, 1e-3F );
    EXPECT_NEAR( rig.Get( hand ).Pose.Translation.y, 100.0F, 1e-3F );

    // And moving the NEW space now moves the control, while the old one no longer does.
    local[2].Translation += glm::vec3( 0.0F, 0.0F, 9.0F );
    pose.Invalidate();
    rig.Evaluate( skeleton, pose );
    EXPECT_NEAR( PositionOf( rig.GetGlobalTransform( hand ) ).z, 9.0F, 1e-3F )
         << "the control did not follow the space it was switched into";
}

TEST( ControlHierarchyTest, SwitchingWithoutKeepingTheWorldMovesIt )
{
    // THE POSITIVE CONTROL for the test above: without `keepWorld` the control jumps into the new space,
    // so "it did not move" there is a statement about the compensation and not about nothing happening.
    const Skeleton  skeleton = MakeRig();
    const LocalPose local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose   pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   hand =
         MustAdd( rig, MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F },
                                                   ControlSpace{ ControlSpaceKind::Bone, 2, 0.0F } } ) );
    rig.Evaluate( skeleton, pose );
    const glm::vec3 before = PositionOf( rig.GetGlobalTransform( hand ) );

    ASSERT_TRUE( rig.SetSpaceWeights( hand, { 0.0F, 1.0F }, /*keepWorld=*/false ).IsSuccess() );
    EXPECT_NE( PositionOf( rig.GetGlobalTransform( hand ) ), before );
}

TEST( ControlHierarchyTest, HalfAndHalfSitsBetweenTheTwoSpacesAndWeightsAreNormalised )
{
    const Skeleton  skeleton = MakeRig();
    const LocalPose local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose   pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   hand =
         MustAdd( rig, MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 0.5F },
                                                   ControlSpace{ ControlSpaceKind::Bone, 2, 0.5F } } ) );
    rig.Evaluate( skeleton, pose );

    const glm::vec3 blended = PositionOf( rig.GetGlobalTransform( hand ) );
    EXPECT_NEAR( blended.x, 25.0F, 1e-3F ) << "a half-and-half control is not between its two spaces";
    EXPECT_NEAR( blended.y, 50.0F, 1e-3F );

    // 1/1 MUST MEAN THE SAME AS 0.5/0.5 — weights are a ratio, not an amount, and an animator typing 1
    // into both fields is saying "equally", not "twice as much".
    ASSERT_TRUE( rig.SetSpaceWeights( hand, { 1.0F, 1.0F }, /*keepWorld=*/false ).IsSuccess() );
    const glm::vec3 same = PositionOf( rig.GetGlobalTransform( hand ) );
    EXPECT_NEAR( same.x, blended.x, 1e-3F );
    EXPECT_NEAR( same.y, blended.y, 1e-3F );
}

TEST( ControlHierarchyTest, AControlWithNoSpaceIsRefusedRatherThanAnsweredWithIdentity )
{
    const Skeleton  skeleton = MakeRig();
    const LocalPose local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose   pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   hand =
         MustAdd( rig, MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F },
                                                   ControlSpace{ ControlSpaceKind::Bone, 2, 0.0F } } ) );
    rig.Evaluate( skeleton, pose );

    const auto refused = rig.SetSpaceWeights( hand, { 0.0F, 0.0F }, /*keepWorld=*/true );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "hand_ctrl" ), std::string::npos ) << "the refusal does not name it";

    // AND IT CHANGED NOTHING: a refused switch must not leave the rig half-switched.
    EXPECT_FLOAT_EQ( rig.Get( hand ).Parents[0].Weight, 1.0F );
    EXPECT_FLOAT_EQ( rig.Get( hand ).Parents[1].Weight, 0.0F );

    const auto wrongCount = rig.SetSpaceWeights( hand, { 1.0F }, /*keepWorld=*/false );
    EXPECT_FALSE( wrongCount.IsSuccess() ) << "a weight list of the wrong length was accepted";
}

TEST( ControlHierarchyTest, ResolutionIsLazyAndDirtyingIsPropagatedRatherThanBlanket )
{
    const Skeleton  skeleton = MakeRig();
    const LocalPose local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose   pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   root =
         MustAdd( rig, MakeControl( "root_ctrl", { ControlSpace{ ControlSpaceKind::Component, 0, 1.0F } } ) );
    const uint32_t child =
         MustAdd( rig, MakeControl( "child_ctrl", { ControlSpace{ ControlSpaceKind::Control, root, 1.0F } } ) );
    const uint32_t other =
         MustAdd( rig, MakeControl( "unrelated_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 2, 1.0F } } ) );

    rig.Evaluate( skeleton, pose );
    EXPECT_FALSE( rig.Resolved( root ) ) << "evaluation resolved something nobody asked for";
    EXPECT_FALSE( rig.Resolved( child ) );

    // Reading the CHILD resolves its parent on the way, and leaves the unrelated one alone.
    (void)rig.GetGlobalTransform( child );
    EXPECT_TRUE( rig.Resolved( child ) );
    EXPECT_TRUE( rig.Resolved( root ) ) << "the parent was not resolved by the child that needed it";
    EXPECT_FALSE( rig.Resolved( other ) ) << "an unrelated control was resolved";

    (void)rig.GetGlobalTransform( other );
    ASSERT_TRUE( rig.Resolved( other ) );

    // MOVING THE PARENT DIRTIES THE CHILD AND NOTHING ELSE. Blanket invalidation would pass the first of
    // these two and fail the second — which is the whole of report 01 §(c)1.
    BoneTransform moved;
    moved.Translation = glm::vec3( 3.0F, 0.0F, 0.0F );
    ASSERT_TRUE( rig.SetPose( root, moved ).IsSuccess() );
    EXPECT_FALSE( rig.Resolved( child ) ) << "the child kept a stale global after its parent moved";
    EXPECT_TRUE( rig.Resolved( other ) ) << "an unrelated control was invalidated too";

    EXPECT_EQ( rig.Dependents( root ).size(), 1u );
    EXPECT_EQ( rig.Dependents( root ).front(), child );
    EXPECT_TRUE( rig.Dependents( other ).empty() );

    EXPECT_NEAR( PositionOf( rig.GetGlobalTransform( child ) ).x, 3.0F, 1e-3F );
}

TEST( ControlHierarchyTest, TwoControlsUnderOneNameAreRefusedAndACycleCannotBeWrittenDown )
{
    ControlHierarchy rig;
    ASSERT_TRUE( rig.Add( MakeControl( "hand_ctrl", {} ) ).IsSuccess() );

    const auto duplicate = rig.Add( MakeControl( "hand_ctrl", {} ) );
    ASSERT_FALSE( duplicate.IsSuccess() );
    EXPECT_NE( duplicate.GetError().find( "hand_ctrl" ), std::string::npos );

    // A parent that does not exist YET is refused, which is what makes a cycle inexpressible rather than
    // detected: every index points backwards, so there is no traversal that can loop.
    const auto forward =
         rig.Add( MakeControl( "later_ctrl", { ControlSpace{ ControlSpaceKind::Control, 99, 1.0F } } ) );
    EXPECT_FALSE( forward.IsSuccess() );

    const auto unnamed = rig.Add( MakeControl( "", {} ) );
    EXPECT_FALSE( unnamed.IsSuccess() );
}

TEST( ControlHierarchyTest, TheOffsetIsTheRiggersAndAuthoringItMovesTheControlOnlyThen )
{
    const Skeleton  skeleton = MakeRig();
    const LocalPose local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose   pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   hand =
         MustAdd( rig, MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } } ) );
    rig.Evaluate( skeleton, pose );

    BoneTransform animated;
    animated.Translation = glm::vec3( 1.0F, 2.0F, 3.0F );
    ASSERT_TRUE( rig.SetPose( hand, animated ).IsSuccess() );
    const glm::vec3 before = PositionOf( rig.GetGlobalTransform( hand ) );

    BoneTransform offset;
    offset.Translation = glm::vec3( 0.0F, 0.0F, 20.0F );
    ASSERT_TRUE( rig.SetOffset( hand, offset ).IsSuccess() );

    // It moves NOW, by exactly the offset, and the animated value is untouched — the two layers are not
    // the same layer, which is the thing report 01 §(c)3 says editors get wrong.
    EXPECT_NEAR( PositionOf( rig.GetGlobalTransform( hand ) ).z, before.z + 20.0F, 1e-3F );
    EXPECT_EQ( rig.Get( hand ).Pose.Translation, glm::vec3( 1.0F, 2.0F, 3.0F ) );
}

TEST( ControlHierarchyTest, BlendingTwoNearlyEqualSpacesGivesThatRotationAndNotItsMirror )
{
    // TWO EARLIER VERSIONS OF THIS TEST COULD NOT SEE THE DEFECT THEY WERE NAMED AFTER, and both survivals
    // were worth more than the green run they replaced:
    //
    //   1. building one space from `q` and the other from `-q`. A MATRIX CANNOT CARRY THE SIGN — both
    //      decompose to the same quaternion, so there was nothing to align;
    //   2. a pair whose signs flip under `glm::decompose`. `BoneTransform::FromMatrix` does not use it; it
    //      normalises the basis and calls `glm::quat_cast`, and the two functions flip in DIFFERENT PLACES.
    //      A case measured against the wrong function is a case about the wrong function.
    //
    // The pair below was measured against the path that actually runs, by sweeping 400 000 nearby rotation
    // pairs through `FromMatrix` and keeping the worst: these two are 0.03 degrees apart and their
    // decomposed quaternions have a dot of -1.0000. Aligned, the blend maps x to (0.004, -0.951, -0.309),
    // which is where both inputs map it. Unaligned, the sum has length 0.000137 — numerical dust — and
    // normalising it yields (0.052, 0.999, -0.015): a control that flips to somewhere else entirely at the
    // moment the two spaces it sits between agree.
    const glm::vec3 axis( 0.166214F, 0.143298F, -0.975622F );
    const glm::mat4 firstMatrix  = glm::rotate( glm::mat4( 1.0F ), 1.595556F, axis );
    const glm::mat4 secondMatrix = glm::rotate( glm::mat4( 1.0F ), 1.595007F, axis );

    // Decomposed HERE, because that is where the hierarchy does it now: the blend takes transforms, so
    // a space that will not decompose is refused by the caller that knows which control it belongs to.
    auto firstT  = BoneTransform::FromMatrix( firstMatrix );
    auto secondT = BoneTransform::FromMatrix( secondMatrix );
    ASSERT_TRUE( firstT.IsSuccess() );
    ASSERT_TRUE( secondT.IsSuccess() );

    const glm::mat4 blended =
         Desert::Animation::BlendSpaces( { firstT.GetValue(), secondT.GetValue() }, { 0.5F, 0.5F } );

    const glm::vec4 probe( 1.0F, 0.0F, 0.0F, 0.0F );
    const glm::vec3 wanted = glm::vec3( firstMatrix * probe );
    const glm::vec3 got    = glm::vec3( blended * probe );

    EXPECT_NEAR( got.x, wanted.x, 0.01F ) << "the blend of two nearly equal rotations is not that rotation";
    EXPECT_NEAR( got.y, wanted.y, 0.01F );
    EXPECT_NEAR( got.z, wanted.z, 0.01F );
}

TEST( ControlHierarchyTest, ARigOverTheWrongSkeletonSaysSoInsteadOfParkingEveryControlAtTheOrigin )
{
    const Skeleton  skeleton = MakeRig(); // three bones
    const LocalPose local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose   pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   ok =
         MustAdd( rig, MakeControl( "fits_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } } ) );
    ASSERT_NE( ok, ControlHierarchy::INVALID );

    rig.Evaluate( skeleton, pose );
    EXPECT_TRUE( rig.GetStructureError().empty() ) << rig.GetStructureError();

    // A control naming a bone this skeleton does not have. Nothing about the VALUES is wrong — identity is
    // a perfectly good matrix — so the only way this is ever noticed is if it is said.
    const uint32_t stray =
         MustAdd( rig, MakeControl( "stray_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 9, 1.0F } } ) );
    ASSERT_NE( stray, ControlHierarchy::INVALID );

    rig.Evaluate( skeleton, pose );
    ASSERT_FALSE( rig.GetStructureError().empty() ) << "a rig over the wrong skeleton reported nothing";
    EXPECT_NE( rig.GetStructureError().find( "stray_ctrl" ), std::string::npos );
    EXPECT_NE( rig.GetStructureError().find( '9' ), std::string::npos );
}

TEST( ControlHierarchyTest, AParentSpaceThatWillNotDecomposeIsNamedRatherThanDroppedFromTheBlend )
{
    // FOUND BY A CENSUS, AND IT WAS MINE. The blend used to decompose each space itself and answer a
    // refusal with a bare `continue` — so a mirrored or degenerate parent vanished from the sum while its
    // WEIGHT still counted in the normalisation, and the result came back quietly scaled towards nothing
    // with nobody told. `GpuWriteCensus.NoRefusalIsAnsweredWithABareContinue` names exactly that shape.
    const Skeleton skeleton = MakeRig();
    LocalPose      local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    local[2].Scale          = glm::vec3( -1.0F, 1.0F, 1.0F ); // a MIRRORED space: determinant < 0
    ComponentPose pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   hand =
         MustAdd( rig, MakeControl( "hand_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 0.5F },
                                                   ControlSpace{ ControlSpaceKind::Bone, 2, 0.5F } } ) );
    ASSERT_NE( hand, ControlHierarchy::INVALID );

    rig.Evaluate( skeleton, pose );
    ASSERT_TRUE( rig.GetStructureError().empty() ) << "the mirror is not a skeleton mismatch";

    (void)rig.GetGlobalTransform( hand );
    ASSERT_FALSE( rig.GetStructureError().empty() )
         << "a space that cannot be blended was dropped and nothing was said";
    EXPECT_NE( rig.GetStructureError().find( "hand_ctrl" ), std::string::npos );
}

TEST( ControlHierarchyTest, WritingAGlobalResolvesTheParentsItIsMeasuredAgainst )
{
    // A WRITE READS. `SetGlobalTransform` back-solves through the parent space, so it needs that space to
    // be RESOLVED — and a parent nobody has read yet this evaluation is not. The first version of this
    // class read the parent's cached global straight out of the array, which after `Evaluate` is whatever
    // it was last frame (or identity on the first), so dragging a child before anything had drawn its
    // parent put the control somewhere nobody asked for.
    const Skeleton  skeleton = MakeRig();
    const LocalPose local    = PoseWith( glm::vec3( 0.0F, 100.0F, 0.0F ), glm::vec3( 50.0F, 0.0F, 0.0F ) );
    ComponentPose   pose( skeleton, local );

    ControlHierarchy rig;
    const uint32_t   parent =
         MustAdd( rig, MakeControl( "parent_ctrl", { ControlSpace{ ControlSpaceKind::Bone, 1, 1.0F } } ) );
    const uint32_t child =
         MustAdd( rig, MakeControl( "child_ctrl", { ControlSpace{ ControlSpaceKind::Control, parent, 1.0F } } ) );
    ASSERT_NE( child, ControlHierarchy::INVALID );

    rig.Evaluate( skeleton, pose );
    ASSERT_FALSE( rig.Resolved( parent ) ) << "the parent must be unresolved for this test to mean anything";

    const glm::mat4 wanted = glm::translate( glm::mat4( 1.0F ), glm::vec3( 7.0F, 8.0F, 9.0F ) );
    ASSERT_TRUE( rig.SetGlobalTransform( child, wanted ).IsSuccess() );

    const glm::vec3 got = PositionOf( rig.GetGlobalTransform( child ) );
    EXPECT_NEAR( got.x, 7.0F, 1e-3F ) << "the write was measured against an unresolved parent";
    EXPECT_NEAR( got.y, 8.0F, 1e-3F );
    EXPECT_NEAR( got.z, 9.0F, 1e-3F );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
