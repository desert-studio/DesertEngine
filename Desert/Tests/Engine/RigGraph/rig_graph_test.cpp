// A21 — T5.5, THE RIG GRAPH, AND THE FOUR THINGS THAT HAVE TO BE TRUE FOR "THE GRAPH RUNS" TO MEAN ANYTHING.
//
//   1. THE GRAPH EXECUTES, AND THE CHANGE IS ARITHMETIC. The driven bone's component transform is asserted
//      to equal a product this test computes independently — `elbowComponent * authoredOffset` — and not
//      "it is different from before". "It is different" is also what a defect says.
//   2. WITHOUT A GRAPH THE PIPELINE IS BIT-FOR-BIT WHAT T5.4 SHIPPED, and WITH one the bytes are DIFFERENT.
//      Both halves, because a positive control that cannot fail controls nothing: if the graph did not move
//      a single byte, "unchanged without a graph" would still pass. The undriven bones are bit-identical in
//      both directions, which is the negative control — a graph that rewrote the whole pose would satisfy
//      the difference assertion exactly as well as one that works.
//   3. THE SILENT GRAPH IS UNREACHABLE. T5.4's load-bearing refusal was "a rig with no drives", because a
//      stage that provably cannot change the pose passes every assertion a working one passes. A graph has
//      that shape three times — no sink, a node that feeds no sink, and writes that cannot reach a driven
//      bone — and each is asserted refused, by name.
//   4. THE FILE AND THE WALK REFUSE THE SAME THINGS. `ValidateControlRigData` must not accept a `.derig`
//      the loader will reject, so the two are asked the same questions here, side by side.
//
// The rig is eight bones for the reason T5.4's suite gives: a one- or two-bone rig cannot tell "wrote the
// driven bone" from "wrote the whole pose", because the two produce the same bytes.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Animation/Rig/RigGraph.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/ControlRig.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using Desert::Animation::AnimationClip;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTrack;
using Desert::Animation::BoneTransform;
using Desert::Animation::ComponentPose;
using Desert::Animation::ControlBoneDrive;
using Desert::Animation::ControlElement;
using Desert::Animation::ControlHierarchy;
using Desert::Animation::ControlRigStage;
using Desert::Animation::ControlSpace;
using Desert::Animation::ControlSpaceKind;
using Desert::Animation::DescribeRigNode;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::LocalPose;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::RigControlSpace;
using Desert::Animation::RigControlSpaceFromText;
using Desert::Animation::RigGraph;
using Desert::Animation::RigNode;
using Desert::Animation::RigNodeDescriptor;
using Desert::Animation::RigNodeDescriptors;
using Desert::Animation::RigNodeInput;
using Desert::Animation::RigNodeKind;
using Desert::Animation::RigNodeKindFromText;
using Desert::Animation::RigNodeTargetKind;
using Desert::Animation::RigValue;
using Desert::Animation::RigValueKind;
using Desert::Animation::Skeleton;

namespace Serialization = Desert::Assets::Serialization;

namespace
{
    constexpr uint32_t kSpine    = 0;
    constexpr uint32_t kShoulder = 1;
    constexpr uint32_t kElbow    = 2;
    constexpr uint32_t kHand     = 3;
    constexpr uint32_t kFinger   = 4;
    constexpr uint32_t kTail     = 5;
    constexpr uint32_t kTailTip  = 6;
    constexpr uint32_t kProp     = 7;

    // 1 world unit = 1 cm. Same rig as T5.4's suite, and deliberately so: the two suites' numbers are then
    // comparable, and a change that moves one and not the other is visible.
    //
    //   0 Spine (root) -> 1 Shoulder -> 2 Elbow -> 3 Hand -> 4 Finger
    //                  -> 5 Tail -> 6 TailTip
    //   7 Prop (a second root)
    Skeleton MakeArmRig()
    {
        const auto place = []( const glm::vec3& translation, float degrees, const glm::vec3& axis )
        {
            return glm::translate( glm::mat4( 1.0F ), translation ) *
                   glm::rotate( glm::mat4( 1.0F ), glm::radians( degrees ), axis );
        };

        std::vector<BoneInfo> bones( 8 );

        bones[0].Name               = "Spine";
        bones[0].LocalBindTransform = place( { 0.0F, 100.0F, 0.0F }, 15.0F, { 0.0F, 0.0F, 1.0F } );

        bones[1].Name               = "Shoulder";
        bones[1].ParentBoneID       = 0U;
        bones[1].LocalBindTransform = place( { 10.0F, 20.0F, 0.0F }, -10.0F, { 1.0F, 0.0F, 0.0F } );

        bones[2].Name               = "Elbow";
        bones[2].ParentBoneID       = 1U;
        bones[2].LocalBindTransform = place( { 0.0F, -40.0F, 3.0F }, 5.0F, { 0.0F, 1.0F, 0.0F } );

        bones[3].Name               = "Hand";
        bones[3].ParentBoneID       = 2U;
        bones[3].LocalBindTransform = place( { 0.0F, -30.0F, 0.0F }, -8.0F, { 0.0F, 0.0F, 1.0F } );

        bones[4].Name               = "Finger";
        bones[4].ParentBoneID       = 3U;
        bones[4].LocalBindTransform = place( { 0.0F, -5.0F, 0.0F }, 3.0F, { 1.0F, 0.0F, 0.0F } );

        bones[5].Name               = "Tail";
        bones[5].ParentBoneID       = 0U;
        bones[5].LocalBindTransform = place( { -20.0F, -10.0F, 0.0F }, 25.0F, { 0.0F, 1.0F, 0.0F } );

        bones[6].Name               = "TailTip";
        bones[6].ParentBoneID       = 5U;
        bones[6].LocalBindTransform = place( { 0.0F, -10.0F, 0.0F }, -12.0F, { 0.0F, 0.0F, 1.0F } );

        bones[7].Name               = "Prop";
        bones[7].LocalBindTransform = place( { 200.0F, 0.0F, 0.0F }, 40.0F, { 0.0F, 1.0F, 0.0F } );

        Skeleton rig( std::move( bones ) );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    AnimationClip ArmClip()
    {
        AnimationClip clip;
        clip.AnimationName = "wave";
        clip.DurationTicks = FrameNumber{ PROJECT_TICK_RATE.Numerator };

        BoneTrack shoulder;
        shoulder.BoneName = "Shoulder";
        shoulder.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 10.0F, 20.0F, 0.0F ) } );
        shoulder.PositionKeys.push_back(
             { FrameNumber{ PROJECT_TICK_RATE.Numerator }, glm::vec3( 40.0F, 55.0F, -12.0F ) } );
        shoulder.RotationKeys.push_back( { FrameNumber{ 0 }, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) } );
        shoulder.ScaleKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 1.0F ) } );
        clip.Tracks.push_back( shoulder );

        BoneTrack hand;
        hand.BoneName = "Hand";
        hand.PositionKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 0.0F, -30.0F, 0.0F ) } );
        hand.PositionKeys.push_back(
             { FrameNumber{ PROJECT_TICK_RATE.Numerator }, glm::vec3( 7.0F, -22.0F, 4.0F ) } );
        hand.RotationKeys.push_back( { FrameNumber{ 0 }, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) } );
        hand.ScaleKeys.push_back( { FrameNumber{ 0 }, glm::vec3( 1.0F ) } );
        clip.Tracks.push_back( hand );

        return clip;
    }

    BoneTransform Placed( const glm::vec3& translation, float degrees, const glm::vec3& axis )
    {
        BoneTransform out;
        out.Translation = translation;
        out.Rotation    = glm::angleAxis( glm::radians( degrees ), glm::normalize( axis ) );
        return out;
    }

    ControlElement MakeControl( std::string name, ControlSpace parent, BoneTransform offset, BoneTransform pose )
    {
        ControlElement element;
        element.Name   = std::move( name );
        element.Offset = offset;
        element.Pose   = pose;
        element.Parents.push_back( parent );
        return element;
    }

    uint32_t MustAdd( ControlHierarchy& rig, ControlElement element )
    {
        auto added = rig.Add( std::move( element ) );
        EXPECT_TRUE( added.IsSuccess() ) << added.GetError();
        return added.IsSuccess() ? added.GetValue() : ControlHierarchy::INVALID;
    }

    RigNodeInput Lit( float value )
    {
        RigNodeInput input;
        input.Literal = RigValue{ value };
        return input;
    }

    RigNodeInput Lit( const glm::vec3& value )
    {
        RigNodeInput input;
        input.Literal = RigValue{ value };
        return input;
    }

    RigNodeInput Lit( const BoneTransform& value )
    {
        RigNodeInput input;
        input.Literal = RigValue{ value };
        return input;
    }

    RigNodeInput From( uint32_t node, uint8_t pin = 0 )
    {
        RigNodeInput input;
        input.Node = node;
        input.Pin  = pin;
        return input;
    }

    RigNode MakeNode( std::string name, RigNodeKind kind, std::vector<RigNodeInput> inputs,
                      uint32_t        target = ControlHierarchy::INVALID,
                      RigControlSpace space  = RigControlSpace::Global )
    {
        RigNode node;
        node.Name   = std::move( name );
        node.Kind   = kind;
        node.Target = target;
        node.Space  = space;
        node.Inputs = std::move( inputs );
        return node;
    }

    ::testing::AssertionResult MatNear( const glm::mat4& a, const glm::mat4& b, float eps = 1e-3F )
    {
        for ( int c = 0; c < 4; ++c )
        {
            for ( int r = 0; r < 4; ++r )
            {
                if ( std::abs( a[c][r] - b[c][r] ) > eps )
                {
                    return ::testing::AssertionFailure()
                           << "mismatch at [" << c << "][" << r << "]: " << a[c][r] << " vs " << b[c][r];
                }
            }
        }
        return ::testing::AssertionSuccess();
    }

    bool SameMatrixBytes( const glm::mat4& a, const glm::mat4& b )
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

    bool SameBytes( const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b )
    {
        if ( a.size() != b.size() )
        {
            return false;
        }
        for ( size_t i = 0; i < a.size(); ++i )
        {
            if ( !SameMatrixBytes( a[i], b[i] ) )
            {
                return false;
            }
        }
        return true;
    }

    /// A stage with one Component-parented control driving the hand. Component space is the identity, so
    /// every expected value below is a product this test writes out rather than one the rig reports.
    std::unique_ptr<ControlRigStage> HandRig( const Skeleton& skeleton, const BoneTransform& offset,
                                              const BoneTransform& pose, uint32_t* outControl )
    {
        auto           stage   = std::make_unique<ControlRigStage>();
        const uint32_t control = MustAdd(
             stage->GetHierarchy(),
             MakeControl( "Hand_CTRL", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F }, offset, pose ) );
        if ( outControl != nullptr )
        {
            *outControl = control;
        }
        const auto drives = stage->SetDrives( skeleton, { ControlBoneDrive{ control, kHand } } );
        EXPECT_TRUE( drives.IsSuccess() ) << drives.GetError();
        return stage;
    }

    /// Runs one stage over the bind pose and hands back the driven bone's component transform.
    struct Ran
    {
        ::Common::BoolResultStr Result = ::Common::MakeSuccess( true );
        LocalPose               Pose;
    };

    Ran RunOverBindPose( ControlRigStage& stage, const Skeleton& skeleton )
    {
        Ran  out;
        auto bind = LocalPose::FromBindPose( skeleton );
        EXPECT_TRUE( bind.IsSuccess() ) << bind.GetError();
        out.Pose = bind.GetValue();

        ComponentPose component( skeleton, out.Pose );
        out.Result = stage.Evaluate( skeleton, out.Pose, component );
        return out;
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 0. The table is the single source of truth, and these are the assertions that keep it one.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( RigGraphTest, EveryKindHasExactlyOneRowAndTheCountIsDerivedFromTheTable )
{
    const std::span<const RigNodeDescriptor> table = RigNodeDescriptors();

    // The count is READ OFF the table, never typed beside it: a gate that pins a number can be satisfied by
    // editing the number. What is pinned is the relation — row i describes kind i — plus the last
    // enumerator, which is the one fact a new kind must move.
    ASSERT_EQ( table.size(), static_cast<size_t>( RigNodeKind::SetControl ) + 1U );

    std::set<std::string> spellings;
    for ( size_t i = 0; i < table.size(); ++i )
    {
        EXPECT_EQ( static_cast<size_t>( table[i].Kind ), i ) << "row " << i << " describes another kind";
        EXPECT_FALSE( table[i].Name.empty() );
        EXPECT_TRUE( spellings.insert( std::string( table[i].Name ) ).second )
             << "two kinds spelled '" << table[i].Name << "'";

        // The file's spelling round-trips, in both directions. A format that can write a word it cannot
        // read is the defect `kSpaceKinds` in the `.derig` layer already carries a comment about.
        const auto parsed = RigNodeKindFromText( table[i].Name );
        ASSERT_TRUE( parsed.has_value() ) << table[i].Name;
        EXPECT_EQ( parsed.value_or( table[i].Kind ), table[i].Kind );

        std::set<std::string> pins;
        for ( const auto& pin : table[i].Inputs )
        {
            EXPECT_TRUE( pins.insert( std::string( pin.Name ) ).second )
                 << table[i].Name << " has two inputs named '" << pin.Name << "'";
        }
        pins.clear();
        for ( const auto& pin : table[i].Outputs )
        {
            EXPECT_TRUE( pins.insert( std::string( pin.Name ) ).second )
                 << table[i].Name << " has two outputs named '" << pin.Name << "'";
        }
    }

    EXPECT_FALSE( RigNodeKindFromText( "SetTransform" ).has_value() )
         << "an unknown kind must come back empty rather than as the first row";
    EXPECT_TRUE( RigControlSpaceFromText( "Local" ).has_value() );
    EXPECT_TRUE( RigControlSpaceFromText( "Global" ).has_value() );
    EXPECT_FALSE( RigControlSpaceFromText( "World" ).has_value() );
}

TEST( RigGraphTest, EverySinkIsExactlyTheKindsWithNoOutputsAndThereIsAtLeastOne )
{
    // "A sink is a node with no outputs" is the DEFINITION the liveness rule and the no-sink refusal are
    // both written against. There is no `bool Writes` column to drift away from it, and this is the
    // assertion that keeps that true when a kind is added.
    size_t sinks = 0;
    for ( const RigNodeDescriptor& row : RigNodeDescriptors() )
    {
        EXPECT_EQ( row.IsSink(), row.Outputs.empty() );
        if ( row.IsSink() )
        {
            ++sinks;
            EXPECT_NE( row.Target, RigNodeTargetKind::None )
                 << row.Name << " writes something, so it has to say what";
        }
    }
    EXPECT_GT( sinks, 0U ) << "a graph language with no sink cannot change anything at all";
}

TEST( RigGraphTest, EveryKindIsExercisedByANamedTestInThisSuite )
{
    // ONE ROW PER KIND, NOT A COUNT. A census that asserted "ten kinds are covered" could be satisfied by
    // adding a kind and a stale number; this one cannot be satisfied without typing the test that covers
    // the new kind. Same shape as the project's other registers, and the count is derived from the rows.
    struct Covered
    {
        RigNodeKind Kind;
        const char* Test;
    };

    constexpr std::array<Covered, 10> kCoverage = { {
         { RigNodeKind::GetControl, "GetControlReadsThePoseInLocalAndTheCompositionInGlobal" },
         { RigNodeKind::GetBone, "TheDrivenBoneEndsUpAtTheProductTheGraphComputed" },
         { RigNodeKind::MakeTransform, "MakeAndBreakTransformAreEachOthersInverse" },
         { RigNodeKind::BreakTransform, "MakeAndBreakTransformAreEachOthersInverse" },
         { RigNodeKind::MultiplyTransform, "TheDrivenBoneEndsUpAtTheProductTheGraphComputed" },
         { RigNodeKind::InvertTransform, "InvertTransformUndoesAMultiply" },
         { RigNodeKind::BlendTransform, "BlendTransformIsTheMidpointAtAHalfAndIsClampedOutsideTheUnitRange" },
         { RigNodeKind::Distance, "DistanceIsCentimetresBetweenTheTwoOrigins" },
         { RigNodeKind::RemapFloat, "RemapFloatIsLinearInsideTheRangeAndFlatOutsideIt" },
         { RigNodeKind::SetControl, "TheDrivenBoneEndsUpAtTheProductTheGraphComputed" },
    } };

    ASSERT_EQ( kCoverage.size(), RigNodeDescriptors().size() )
         << "a kind was added without a test named beside it";

    std::set<size_t> seen;
    for ( const Covered& row : kCoverage )
    {
        EXPECT_TRUE( seen.insert( static_cast<size_t>( row.Kind ) ).second )
             << "kind listed twice: " << DescribeRigNode( row.Kind ).Name;
        EXPECT_NE( std::string( row.Test ), std::string() );
    }
    EXPECT_EQ( seen.size(), RigNodeDescriptors().size() );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 1. The graph executes, and the change is arithmetic.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( RigGraphTest, TheDrivenBoneEndsUpAtTheProductTheGraphComputed )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();

    Animator reference( skeleton );
    reference.Play( clip, false );
    reference.SetTick( FrameTime{ FrameNumber{ 0 } } );

    const glm::mat4 elbowWithoutRig = reference.GetBoneModelMatrix( kElbow );
    const glm::mat4 handWithoutRig  = reference.GetBoneModelMatrix( kHand );

    // The control's own offset and pose are NOT the answer here; the graph overwrites its global. That is
    // the whole difference between T5.4 and T5.5, and this is where it is visible.
    const BoneTransform offset = Placed( { 5.0F, 0.0F, -3.0F }, 12.0F, { 0.0F, 1.0F, 0.0F } );
    const BoneTransform pose   = Placed( { 150.0F, -120.0F, 80.0F }, -35.0F, { 1.0F, 0.0F, 1.0F } );

    uint32_t control = ControlHierarchy::INVALID;
    auto     stage   = HandRig( skeleton, offset, pose, &control );

    // "Put the hand control at the elbow, moved 25 cm along the elbow's own -Y and turned 30 degrees."
    // Three nodes and a literal, and the expectation below is the same product written in C++.
    const BoneTransform reach = Placed( { 0.0F, -25.0F, 0.0F }, 30.0F, { 0.0F, 0.0F, 1.0F } );

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "elbow", RigNodeKind::GetBone, {}, kElbow ) );
    nodes.push_back( MakeNode( "target", RigNodeKind::MultiplyTransform, { From( 0 ), Lit( reach ) } ) );
    nodes.push_back(
         MakeNode( "write", RigNodeKind::SetControl, { From( 1 ) }, control, RigControlSpace::Global ) );

    RigGraph graph;
    ASSERT_TRUE( graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), nodes ).IsSuccess() );
    const auto installed = stage->SetGraph( std::move( graph ) );
    ASSERT_TRUE( installed.IsSuccess() ) << installed.GetError();

    Animator animator( skeleton );
    animator.Play( clip, false );
    const auto attached = animator.AttachRig( std::move( stage ) );
    ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();
    animator.SetTick( FrameTime{ FrameNumber{ 0 } } );
    ASSERT_TRUE( animator.GetRig()->GetLastError().empty() ) << animator.GetRig()->GetLastError();

    // ARITHMETIC, STATED. The bone is where `elbowComponent * reach` is, to within float noise — which on
    // a 100-cm rig is ~1e-4 cm, three orders below the 1e-3 tolerance.
    const glm::mat4 expected = elbowWithoutRig * reach.ToMatrix();
    EXPECT_TRUE( MatNear( animator.GetBoneModelMatrix( kHand ), expected ) );

    // AND IT IS NOT WHERE THE CONTROL'S OWN COMPOSITION WOULD HAVE PUT IT. Without this the test would also
    // pass on a stage that ignored the graph and ran T5.4's identity solve.
    const glm::mat4 identitySolve = offset.ToMatrix() * pose.ToMatrix();
    EXPECT_GT( glm::length( glm::vec3( expected[3] ) - glm::vec3( identitySolve[3] ) ), 1.0F );
    EXPECT_GT( glm::length( glm::vec3( expected[3] ) - glm::vec3( handWithoutRig[3] ) ), 1.0F );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 2. The positive control, and its other half.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( RigGraphTest, WithoutAGraphTheSkinningMatricesAreByteForByteTheOnesTFiveFourProduced )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();

    const BoneTransform offset = Placed( { 5.0F, 0.0F, -3.0F }, 12.0F, { 0.0F, 1.0F, 0.0F } );
    const BoneTransform pose   = Placed( { 150.0F, -120.0F, 80.0F }, -35.0F, { 1.0F, 0.0F, 1.0F } );

    // THE CONTROL: the identical rig, evaluated by the identical stage, with the graph the only difference
    // between the two runs. Two Animators rather than one, because a stage cannot be un-graphed and a
    // second run over a mutated one would be comparing a rig to itself after a write.
    Animator withoutGraph( skeleton );
    withoutGraph.Play( clip, false );
    {
        uint32_t   control  = ControlHierarchy::INVALID;
        const auto attached = withoutGraph.AttachRig( HandRig( skeleton, offset, pose, &control ) );
        ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();
    }
    withoutGraph.SetTick( FrameTime{ FrameNumber{ 0 } } );
    const std::vector<glm::mat4> plain = withoutGraph.GetPose().Matrices;

    Animator withGraph( skeleton );
    withGraph.Play( clip, false );
    {
        uint32_t control = ControlHierarchy::INVALID;
        auto     stage   = HandRig( skeleton, offset, pose, &control );

        const BoneTransform reach = Placed( { 0.0F, -25.0F, 0.0F }, 30.0F, { 0.0F, 0.0F, 1.0F } );

        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "elbow", RigNodeKind::GetBone, {}, kElbow ) );
        nodes.push_back( MakeNode( "target", RigNodeKind::MultiplyTransform, { From( 0 ), Lit( reach ) } ) );
        nodes.push_back(
             MakeNode( "write", RigNodeKind::SetControl, { From( 1 ) }, control, RigControlSpace::Global ) );

        RigGraph graph;
        ASSERT_TRUE( graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), nodes ).IsSuccess() );
        ASSERT_TRUE( stage->SetGraph( std::move( graph ) ).IsSuccess() );

        const auto attached = withGraph.AttachRig( std::move( stage ) );
        ASSERT_TRUE( attached.IsSuccess() ) << attached.GetError();
    }
    withGraph.SetTick( FrameTime{ FrameNumber{ 0 } } );
    const std::vector<glm::mat4> graphed = withGraph.GetPose().Matrices;

    // THE CONTROL CONTROLS SOMETHING: with a graph the bytes are DIFFERENT. Without this half the "no
    // graph, same bytes" assertion below would be satisfied by a graph that never ran.
    ASSERT_FALSE( SameBytes( plain, graphed ) ) << "the graph moved no byte at all";
    EXPECT_FALSE( SameMatrixBytes( plain[kHand], graphed[kHand] ) );

    // AND THE UNDRIVEN BONES ARE BIT-IDENTICAL, both to each other and to a pipeline with no rig. A graph
    // that rewrote the whole pose would satisfy the assertion above exactly as well as one that works.
    Animator noRig( skeleton );
    noRig.Play( clip, false );
    noRig.SetTick( FrameTime{ FrameNumber{ 0 } } );
    const std::vector<glm::mat4> bare = noRig.GetPose().Matrices;

    for ( const uint32_t bone : { kSpine, kShoulder, kElbow, kTail, kTailTip, kProp } )
    {
        EXPECT_TRUE( SameMatrixBytes( bare[bone], plain[bone] ) ) << "rig moved undriven bone " << bone;
        EXPECT_TRUE( SameMatrixBytes( bare[bone], graphed[bone] ) ) << "graph moved undriven bone " << bone;
    }

    // The finger IS driven, indirectly: it is the hand's child and inherits the change. Asserted rather
    // than left out, because "which bones a rig touches" is the statement the negative control makes.
    EXPECT_FALSE( SameMatrixBytes( bare[kFinger], graphed[kFinger] ) );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 3. The arithmetic of each kind, and the one order rule.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
    /// Two Component-parented controls and a drive on the first, so any graph below reaches a bone. The
    /// second control is the scratch every "write it and read it back" assertion uses.
    std::unique_ptr<ControlRigStage> TwoControlRig( const Skeleton& skeleton, uint32_t* a, uint32_t* b )
    {
        auto stage = std::make_unique<ControlRigStage>();

        const ControlSpace world{ ControlSpaceKind::Component, 0, 1.0F };
        *a = MustAdd( stage->GetHierarchy(),
                      MakeControl( "A_CTRL", world, BoneTransform{},
                                   Placed( { 3.0F, 4.0F, 12.0F }, 20.0F, { 0.0F, 1.0F, 0.0F } ) ) );
        *b = MustAdd( stage->GetHierarchy(), MakeControl( "B_CTRL", world, BoneTransform{}, BoneTransform{} ) );

        const auto drives = stage->SetDrives( skeleton, { ControlBoneDrive{ *b, kHand } } );
        EXPECT_TRUE( drives.IsSuccess() ) << drives.GetError();
        return stage;
    }

    /// Installs `nodes` and runs the stage once over the bind pose, failing the test on any refusal.
    void RunGraph( ControlRigStage& stage, const Skeleton& skeleton, std::vector<RigNode> nodes )
    {
        RigGraph   graph;
        const auto built = graph.SetNodes( stage.GetHierarchy(), skeleton.GetBones().size(), std::move( nodes ) );
        ASSERT_TRUE( built.IsSuccess() ) << built.GetError();
        const auto installed = stage.SetGraph( std::move( graph ) );
        ASSERT_TRUE( installed.IsSuccess() ) << installed.GetError();

        const Ran ran = RunOverBindPose( stage, skeleton );
        ASSERT_TRUE( ran.Result.IsSuccess() ) << ran.Result.GetError();
    }
} // namespace

TEST( RigGraphTest, GetControlReadsThePoseInLocalAndTheCompositionInGlobal )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    const BoneTransform aPose = stage->GetHierarchy().Get( a ).Pose;

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "readA", RigNodeKind::GetControl, {}, a, RigControlSpace::Local ) );
    nodes.push_back( MakeNode( "writeB", RigNodeKind::SetControl, { From( 0 ) }, b, RigControlSpace::Local ) );
    RunGraph( *stage, skeleton, nodes );

    const BoneTransform& bPose = stage->GetHierarchy().Get( b ).Pose;
    EXPECT_FLOAT_EQ( bPose.Translation.x, aPose.Translation.x );
    EXPECT_FLOAT_EQ( bPose.Translation.y, aPose.Translation.y );
    EXPECT_FLOAT_EQ( bPose.Translation.z, aPose.Translation.z );

    // And Global is the resolved composition, not the pose: A is Component-parented with an identity
    // offset, so its global is exactly `Pose` as a matrix — a different QUANTITY that happens to agree
    // here, which is why the assertion is on the matrix and not on the two being unequal.
    EXPECT_TRUE( MatNear( stage->GetHierarchy().GetGlobalTransform( a ), aPose.ToMatrix() ) );
}

TEST( RigGraphTest, AReadAfterAWriteSeesTheWrittenValueBecauseOrderIsTheGraphsOwn )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    const BoneTransform before  = stage->GetHierarchy().Get( a ).Pose;
    const BoneTransform written = Placed( { -70.0F, 33.0F, 8.0F }, -45.0F, { 1.0F, 1.0F, 0.0F } );

    // THE TEST THAT A TOPOLOGICAL SORT WOULD FAIL. These three nodes have a link set that says nothing
    // about node 0 at all — it feeds nobody — so a sort over links is free to run the read first. It must
    // not: the hierarchy is state the links do not describe, and the file's order is the answer.
    std::vector<RigNode> nodes;
    nodes.push_back(
         MakeNode( "writeA", RigNodeKind::SetControl, { Lit( written ) }, a, RigControlSpace::Local ) );
    nodes.push_back( MakeNode( "readA", RigNodeKind::GetControl, {}, a, RigControlSpace::Local ) );
    nodes.push_back( MakeNode( "writeB", RigNodeKind::SetControl, { From( 1 ) }, b, RigControlSpace::Local ) );
    RunGraph( *stage, skeleton, nodes );

    const BoneTransform& bPose = stage->GetHierarchy().Get( b ).Pose;
    EXPECT_FLOAT_EQ( bPose.Translation.x, written.Translation.x );
    EXPECT_FLOAT_EQ( bPose.Translation.y, written.Translation.y );
    EXPECT_NE( bPose.Translation.x, before.Translation.x )
         << "B got A's ORIGINAL pose, so the read ran before the write";
}

TEST( RigGraphTest, MakeAndBreakTransformAreEachOthersInverse )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    BoneTransform source;
    source.Translation = { 11.0F, -4.0F, 27.0F };
    source.Rotation    = glm::angleAxis( glm::radians( 37.0F ), glm::normalize( glm::vec3( 1.0F, 2.0F, 3.0F ) ) );
    source.Scale       = { 2.0F, 0.5F, 1.25F };

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "break", RigNodeKind::BreakTransform, { Lit( source ) } ) );
    nodes.push_back(
         MakeNode( "make", RigNodeKind::MakeTransform, { From( 0, 0 ), From( 0, 1 ), From( 0, 2 ) } ) );
    nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 1 ) }, b, RigControlSpace::Local ) );
    RunGraph( *stage, skeleton, nodes );

    const BoneTransform& out = stage->GetHierarchy().Get( b ).Pose;
    EXPECT_TRUE( MatNear( out.ToMatrix(), source.ToMatrix() ) );
}

TEST( RigGraphTest, InvertTransformUndoesAMultiply )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    const BoneTransform left  = Placed( { 40.0F, -9.0F, 2.0F }, 22.0F, { 0.0F, 1.0F, 0.0F } );
    const BoneTransform right = Placed( { -3.0F, 18.0F, 6.0F }, -14.0F, { 1.0F, 0.0F, 1.0F } );

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "product", RigNodeKind::MultiplyTransform, { Lit( left ), Lit( right ) } ) );
    nodes.push_back( MakeNode( "inverseRight", RigNodeKind::InvertTransform, { Lit( right ) } ) );
    nodes.push_back( MakeNode( "back", RigNodeKind::MultiplyTransform, { From( 0 ), From( 1 ) } ) );
    nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 2 ) }, b, RigControlSpace::Local ) );
    RunGraph( *stage, skeleton, nodes );

    EXPECT_TRUE( MatNear( stage->GetHierarchy().Get( b ).Pose.ToMatrix(), left.ToMatrix(), 1e-2F ) );
}

TEST( RigGraphTest, BlendTransformIsTheMidpointAtAHalfAndIsClampedOutsideTheUnitRange )
{
    const Skeleton skeleton = MakeArmRig();

    BoneTransform from;
    from.Translation = { 0.0F, 0.0F, 0.0F };
    BoneTransform to;
    to.Translation = { 100.0F, 0.0F, 0.0F };

    const auto blendedAt = [&]( float alpha )
    {
        uint32_t a     = ControlHierarchy::INVALID;
        uint32_t b     = ControlHierarchy::INVALID;
        auto     stage = TwoControlRig( skeleton, &a, &b );

        std::vector<RigNode> nodes;
        nodes.push_back(
             MakeNode( "mix", RigNodeKind::BlendTransform, { Lit( from ), Lit( to ), Lit( alpha ) } ) );
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 0 ) }, b, RigControlSpace::Local ) );
        RunGraph( *stage, skeleton, nodes );
        return stage->GetHierarchy().Get( b ).Pose.Translation.x;
    };

    EXPECT_NEAR( blendedAt( 0.5F ), 50.0F, 1e-3F );
    EXPECT_NEAR( blendedAt( 0.0F ), 0.0F, 1e-3F );
    EXPECT_NEAR( blendedAt( 1.0F ), 100.0F, 1e-3F );

    // CLAMPED, AND `Blend` ITSELF IS NOT. An alpha of 1.4 would slerp past the target, which on a limb is
    // the joint turning inside out, and nothing authored a request for it.
    EXPECT_NEAR( blendedAt( 1.4F ), 100.0F, 1e-3F );
    EXPECT_NEAR( blendedAt( -2.0F ), 0.0F, 1e-3F );
}

TEST( RigGraphTest, DistanceIsCentimetresBetweenTheTwoOrigins )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    BoneTransform left;
    left.Translation = { 0.0F, 0.0F, 0.0F };
    BoneTransform right;
    right.Translation = { 3.0F, 4.0F, 0.0F }; // 5 cm, and a triangle nobody can get wrong by accident.

    const BoneTransform zero;
    BoneTransform       hundred;
    hundred.Translation = { 100.0F, 0.0F, 0.0F };

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "d", RigNodeKind::Distance, { Lit( left ), Lit( right ) } ) );
    // The distance is carried into a blend so it reaches a control: a Float has no sink of its own, which
    // is exactly the property that makes `Distance` a DRIVER rather than a readout.
    nodes.push_back( MakeNode( "t", RigNodeKind::RemapFloat,
                               { From( 0 ), Lit( 0.0F ), Lit( 10.0F ), Lit( 0.0F ), Lit( 1.0F ) } ) );
    nodes.push_back( MakeNode( "mix", RigNodeKind::BlendTransform, { Lit( zero ), Lit( hundred ), From( 1 ) } ) );
    nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 2 ) }, b, RigControlSpace::Local ) );
    RunGraph( *stage, skeleton, nodes );

    // 5 cm remapped from [0,10] to [0,1] is 0.5, and 0.5 of 100 cm is 50 cm.
    EXPECT_NEAR( stage->GetHierarchy().Get( b ).Pose.Translation.x, 50.0F, 1e-3F );
}

TEST( RigGraphTest, RemapFloatIsLinearInsideTheRangeAndFlatOutsideIt )
{
    const Skeleton skeleton = MakeArmRig();

    const BoneTransform zero;
    BoneTransform       hundred;
    hundred.Translation = { 100.0F, 0.0F, 0.0F };

    const auto remapped = [&]( float value )
    {
        uint32_t a     = ControlHierarchy::INVALID;
        uint32_t b     = ControlHierarchy::INVALID;
        auto     stage = TwoControlRig( skeleton, &a, &b );

        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "t", RigNodeKind::RemapFloat,
                                   { Lit( value ), Lit( 20.0F ), Lit( 40.0F ), Lit( 0.0F ), Lit( 1.0F ) } ) );
        nodes.push_back(
             MakeNode( "mix", RigNodeKind::BlendTransform, { Lit( zero ), Lit( hundred ), From( 0 ) } ) );
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 1 ) }, b, RigControlSpace::Local ) );
        RunGraph( *stage, skeleton, nodes );
        return stage->GetHierarchy().Get( b ).Pose.Translation.x;
    };

    EXPECT_NEAR( remapped( 30.0F ), 50.0F, 1e-3F );
    EXPECT_NEAR( remapped( 25.0F ), 25.0F, 1e-3F );
    // FLAT OUTSIDE, NOT EXTRAPOLATED. Extrapolation past the authored range is how a stretchy limb explodes.
    EXPECT_NEAR( remapped( 1000.0F ), 100.0F, 1e-3F );
    EXPECT_NEAR( remapped( -1000.0F ), 0.0F, 1e-3F );
}

TEST( RigGraphTest, RemapFromAnEmptyRangeIsRefusedAtExecuteRatherThanAnswered )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    const BoneTransform zero;
    BoneTransform       hundred;
    hundred.Translation = { 100.0F, 0.0F, 0.0F };

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "t", RigNodeKind::RemapFloat,
                               { Lit( 5.0F ), Lit( 7.0F ), Lit( 7.0F ), Lit( 0.0F ), Lit( 1.0F ) } ) );
    nodes.push_back( MakeNode( "mix", RigNodeKind::BlendTransform, { Lit( zero ), Lit( hundred ), From( 0 ) } ) );
    nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 1 ) }, b, RigControlSpace::Local ) );

    RigGraph graph;
    ASSERT_TRUE( graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), nodes ).IsSuccess() );
    ASSERT_TRUE( stage->SetGraph( std::move( graph ) ).IsSuccess() );

    const Ran ran = RunOverBindPose( *stage, skeleton );
    EXPECT_FALSE( ran.Result.IsSuccess() ) << "an empty input range has no mapping; answering invents one";
    EXPECT_NE( ran.Result.GetError().find( "empty range" ), std::string::npos ) << ran.Result.GetError();
    EXPECT_NE( ran.Result.GetError().find( "'t'" ), std::string::npos )
         << "the refusal must name the node: " << ran.Result.GetError();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 4. The state that must be unreachable, and the rest of the refusals.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
    /// Builds `nodes` against a two-control rig and returns the refusal, if any.
    std::string Refused( const Skeleton& skeleton, std::vector<RigNode> nodes )
    {
        uint32_t a     = ControlHierarchy::INVALID;
        uint32_t b     = ControlHierarchy::INVALID;
        auto     stage = TwoControlRig( skeleton, &a, &b );

        RigGraph   graph;
        const auto built = graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), std::move( nodes ) );
        return built.IsSuccess() ? std::string() : built.GetError();
    }
} // namespace

TEST( RigGraphTest, AGraphWithNoSinkIsRefusedBecauseItComputesAndDiscards )
{
    const Skeleton skeleton = MakeArmRig();

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "elbow", RigNodeKind::GetBone, {}, kElbow ) );
    nodes.push_back( MakeNode( "twice", RigNodeKind::MultiplyTransform, { From( 0 ), From( 0 ) } ) );

    const std::string error = Refused( skeleton, nodes );
    ASSERT_FALSE( error.empty() ) << "a graph that writes nothing cannot change the pose";
    EXPECT_NE( error.find( "writes nothing" ), std::string::npos ) << error;
}

TEST( RigGraphTest, ANodeThatFeedsNoSinkIsRefusedByName )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "orphan", RigNodeKind::GetBone, {}, kElbow ) );
    nodes.push_back(
         MakeNode( "write", RigNodeKind::SetControl, { Lit( BoneTransform{} ) }, b, RigControlSpace::Local ) );

    RigGraph   graph;
    const auto built = graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), nodes );
    ASSERT_FALSE( built.IsSuccess() );
    EXPECT_NE( built.GetError().find( "'orphan'" ), std::string::npos ) << built.GetError();
    EXPECT_NE( built.GetError().find( "feeds no sink" ), std::string::npos ) << built.GetError();
}

TEST( RigGraphTest, AForwardOrSelfLinkIsRefusedWhichIsWhyNoCycleCanBeWritten )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    {
        // Forward: node 0 reads node 1.
        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "early", RigNodeKind::MultiplyTransform, { From( 1 ), From( 1 ) } ) );
        nodes.push_back( MakeNode( "late", RigNodeKind::GetBone, {}, kElbow ) );
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 0 ) }, b, RigControlSpace::Local ) );
        RigGraph   graph;
        const auto built = graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), nodes );
        ASSERT_FALSE( built.IsSuccess() );
        EXPECT_NE( built.GetError().find( "comes later" ), std::string::npos ) << built.GetError();
    }
    {
        // Self: the tightest cycle there is, refused by the same rule and not by a cycle walk.
        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "me", RigNodeKind::MultiplyTransform, { From( 0 ), From( 0 ) } ) );
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 0 ) }, b, RigControlSpace::Local ) );
        RigGraph   graph;
        const auto built = graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), nodes );
        ASSERT_FALSE( built.IsSuccess() );
        EXPECT_NE( built.GetError().find( "itself" ), std::string::npos ) << built.GetError();
    }
}

TEST( RigGraphTest, AWireOrALiteralOfTheWrongTypeIsRefusedNamingBothTypes )
{
    const Skeleton skeleton = MakeArmRig();

    {
        std::vector<RigNode> nodes;
        nodes.push_back(
             MakeNode( "d", RigNodeKind::Distance, { Lit( BoneTransform{} ), Lit( BoneTransform{} ) } ) );
        // Float wired into a Transform pin.
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 0 ) }, 1, RigControlSpace::Local ) );
        const std::string error = Refused( skeleton, nodes );
        ASSERT_FALSE( error.empty() );
        EXPECT_NE( error.find( "Float" ), std::string::npos ) << error;
        EXPECT_NE( error.find( "Transform" ), std::string::npos ) << error;
    }
    {
        std::vector<RigNode> nodes;
        // A Vec3 literal on a Transform pin.
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { Lit( glm::vec3( 1.0F ) ) }, 1,
                                   RigControlSpace::Local ) );
        const std::string error = Refused( skeleton, nodes );
        ASSERT_FALSE( error.empty() );
        EXPECT_NE( error.find( "Vec3" ), std::string::npos ) << error;
    }
}

TEST( RigGraphTest, TheStructuralMistakesAKindCanCarryAreEachRefusedByName )
{
    const Skeleton skeleton = MakeArmRig();

    // The wrong number of inputs.
    {
        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, {}, 1, RigControlSpace::Local ) );
        const std::string error = Refused( skeleton, nodes );
        EXPECT_NE( error.find( "inputs" ), std::string::npos ) << error;
    }
    // Two nodes, one name.
    {
        std::vector<RigNode> nodes;
        nodes.push_back(
             MakeNode( "same", RigNodeKind::SetControl, { Lit( BoneTransform{} ) }, 1, RigControlSpace::Local ) );
        nodes.push_back(
             MakeNode( "same", RigNodeKind::SetControl, { Lit( BoneTransform{} ) }, 1, RigControlSpace::Local ) );
        const std::string error = Refused( skeleton, nodes );
        EXPECT_NE( error.find( "'same'" ), std::string::npos ) << error;
    }
    // A target on a kind that names nothing.
    {
        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "mul", RigNodeKind::MultiplyTransform,
                                   { Lit( BoneTransform{} ), Lit( BoneTransform{} ) }, 0 ) );
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 0 ) }, 1, RigControlSpace::Local ) );
        const std::string error = Refused( skeleton, nodes );
        EXPECT_NE( error.find( "target" ), std::string::npos ) << error;
    }
    // A space on a kind that has none.
    {
        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "mul", RigNodeKind::MultiplyTransform,
                                   { Lit( BoneTransform{} ), Lit( BoneTransform{} ) }, ControlHierarchy::INVALID,
                                   RigControlSpace::Local ) );
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 0 ) }, 1, RigControlSpace::Local ) );
        const std::string error = Refused( skeleton, nodes );
        EXPECT_NE( error.find( "no control space" ), std::string::npos ) << error;
    }
    // A bone this skeleton does not have.
    {
        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "bone", RigNodeKind::GetBone, {}, 99 ) );
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { From( 0 ) }, 1, RigControlSpace::Local ) );
        const std::string error = Refused( skeleton, nodes );
        EXPECT_NE( error.find( "different skeleton" ), std::string::npos ) << error;
    }
    // A control this rig does not have.
    {
        std::vector<RigNode> nodes;
        nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { Lit( BoneTransform{} ) }, 42,
                                   RigControlSpace::Local ) );
        const std::string error = Refused( skeleton, nodes );
        EXPECT_NE( error.find( "different rig" ), std::string::npos ) << error;
    }
    // An empty node list.
    {
        const std::string error = Refused( skeleton, {} );
        EXPECT_FALSE( error.empty() );
    }
}

TEST( RigGraphTest, AGraphWhoseWritesCannotReachADrivenBoneIsRefused )
{
    const Skeleton skeleton = MakeArmRig();

    // A, B: B drives the hand; A drives nothing and is nobody's parent. A graph that writes only A is a
    // stage that provably cannot change the pose — T5.4's refusal, one layer in.
    uint32_t a     = ControlHierarchy::INVALID;
    uint32_t b     = ControlHierarchy::INVALID;
    auto     stage = TwoControlRig( skeleton, &a, &b );

    std::vector<RigNode> nodes;
    nodes.push_back(
         MakeNode( "write", RigNodeKind::SetControl, { Lit( BoneTransform{} ) }, a, RigControlSpace::Local ) );

    RigGraph graph;
    ASSERT_TRUE( graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), nodes ).IsSuccess() )
         << "the graph is structurally fine; only the drive list makes it pointless";

    const auto installed = stage->SetGraph( std::move( graph ) );
    ASSERT_FALSE( installed.IsSuccess() );
    EXPECT_NE( installed.GetError().find( "A_CTRL" ), std::string::npos ) << installed.GetError();
    EXPECT_NE( installed.GetError().find( "drives a bone" ), std::string::npos ) << installed.GetError();
}

TEST( RigGraphTest, AGraphWritingAParentOfADrivenControlIsAccepted )
{
    // The other side of the reachability rule, and it is what stops it being "the graph must write a driven
    // control". Writing the root of an arm moves the hand that arm drives.
    const Skeleton skeleton = MakeArmRig();

    ControlRigStage stage;
    const uint32_t  root  = MustAdd( stage.GetHierarchy(),
                                     MakeControl( "Root_CTRL", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F },
                                                  BoneTransform{}, BoneTransform{} ) );
    const uint32_t  child = MustAdd(
         stage.GetHierarchy(), MakeControl( "Child_CTRL", ControlSpace{ ControlSpaceKind::Control, root, 1.0F },
                                             BoneTransform{}, BoneTransform{} ) );

    ASSERT_TRUE( stage.SetDrives( skeleton, { ControlBoneDrive{ child, kHand } } ).IsSuccess() );

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "write", RigNodeKind::SetControl,
                               { Lit( Placed( { 10.0F, 0.0F, 0.0F }, 0.0F, { 0.0F, 1.0F, 0.0F } ) ) }, root,
                               RigControlSpace::Local ) );

    RigGraph graph;
    ASSERT_TRUE( graph.SetNodes( stage.GetHierarchy(), skeleton.GetBones().size(), nodes ).IsSuccess() );
    const auto installed = stage.SetGraph( std::move( graph ) );
    EXPECT_TRUE( installed.IsSuccess() ) << installed.GetError();
}

TEST( RigGraphTest, RewritingTheDrivesSoTheGraphIsOrphanedIsRefusedAndTheStageIsUnchanged )
{
    const Skeleton skeleton = MakeArmRig();
    uint32_t       a        = ControlHierarchy::INVALID;
    uint32_t       b        = ControlHierarchy::INVALID;
    auto           stage    = TwoControlRig( skeleton, &a, &b );

    std::vector<RigNode> nodes;
    nodes.push_back(
         MakeNode( "write", RigNodeKind::SetControl, { Lit( BoneTransform{} ) }, b, RigControlSpace::Local ) );
    RigGraph graph;
    ASSERT_TRUE( graph.SetNodes( stage->GetHierarchy(), skeleton.GetBones().size(), nodes ).IsSuccess() );
    ASSERT_TRUE( stage->SetGraph( std::move( graph ) ).IsSuccess() );

    // Re-point every drive at A, which the graph does not write. Reachability is a statement about a PAIR,
    // so either half changing can falsify it.
    const auto rewritten = stage->SetDrives( skeleton, { ControlBoneDrive{ a, kHand } } );
    ASSERT_FALSE( rewritten.IsSuccess() );

    // ROLLED BACK. A refusal that left the stage half-updated would be a rig whose drives and graph
    // disagree, which is worse than either state.
    ASSERT_EQ( stage->GetDrives().size(), 1U );
    EXPECT_EQ( stage->GetDrives()[0].Control, b );
}

TEST( RigGraphTest, AGraphCannotBeInstalledBeforeTheDrivesAndAnEmptyOneIsNotSpelledThisWay )
{
    const Skeleton  skeleton = MakeArmRig();
    ControlRigStage stage;
    const uint32_t  control = MustAdd(
         stage.GetHierarchy(), MakeControl( "Hand_CTRL", ControlSpace{ ControlSpaceKind::Component, 0, 1.0F },
                                             BoneTransform{}, BoneTransform{} ) );

    std::vector<RigNode> nodes;
    nodes.push_back( MakeNode( "write", RigNodeKind::SetControl, { Lit( BoneTransform{} ) }, control,
                               RigControlSpace::Local ) );
    RigGraph graph;
    ASSERT_TRUE( graph.SetNodes( stage.GetHierarchy(), skeleton.GetBones().size(), nodes ).IsSuccess() );

    const auto tooEarly = stage.SetGraph( std::move( graph ) );
    EXPECT_FALSE( tooEarly.IsSuccess() );
    EXPECT_NE( tooEarly.GetError().find( "drives before its graph" ), std::string::npos ) << tooEarly.GetError();

    ASSERT_TRUE( stage.SetDrives( skeleton, { ControlBoneDrive{ control, kHand } } ).IsSuccess() );
    EXPECT_FALSE( stage.SetGraph( RigGraph{} ).IsSuccess() )
         << "an empty graph is spelled by never calling SetGraph, not by calling it with nothing";
    EXPECT_FALSE( stage.HasGraph() );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 5. The file and the walk refuse the same things, and a rig with a graph round-trips by value.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

namespace
{
    Serialization::RigTransformData Data( const BoneTransform& t )
    {
        Serialization::RigTransformData out;
        out.Translation = t.Translation;
        out.Rotation    = t.Rotation;
        out.Scale       = t.Scale;
        return out;
    }

    Serialization::RigGraphInputData LinkInput( std::string pin, std::string node, std::string outPin )
    {
        Serialization::RigGraphInputData input;
        input.Pin  = std::move( pin );
        input.Link = Serialization::RigLinkData{ std::move( node ), std::move( outPin ) };
        return input;
    }

    Serialization::RigGraphInputData TransformInput( std::string pin, const BoneTransform& value )
    {
        Serialization::RigGraphInputData input;
        input.Pin       = std::move( pin );
        input.Transform = Data( value );
        return input;
    }

    /// The `.derig` this section works from: two controls, one drive, and a three-node graph.
    Serialization::ControlRigData RigWithGraph()
    {
        Serialization::ControlRigData data;
        data.FormatVersion = Serialization::kControlRigVersion;
        data.Name          = "Graphed Arm";

        Serialization::ControlElementData hand;
        hand.Name      = "Hand_CTRL";
        hand.ShapeName = "CircleXY";
        hand.Parents.push_back( Serialization::ControlSpaceData{ "Component", "", 1.0f } );
        data.Controls.push_back( hand );

        Serialization::ControlElementData elbow;
        elbow.Name      = "Elbow_CTRL";
        elbow.ShapeName = "Diamond";
        elbow.Parents.push_back( Serialization::ControlSpaceData{ "Bone", "Shoulder", 1.0f } );
        data.Controls.push_back( elbow );

        data.Drives.push_back( Serialization::ControlDriveData{ "Hand_CTRL", "Hand" } );

        Serialization::RigGraphNodeData read;
        read.Name   = "elbowBone";
        read.Kind   = "GetBone";
        read.Target = "Elbow";

        Serialization::RigGraphNodeData product;
        product.Name = "reach";
        product.Kind = "MultiplyTransform";
        product.Inputs.push_back( LinkInput( "A", "elbowBone", "Transform" ) );
        product.Inputs.push_back(
             TransformInput( "B", Placed( { 0.0F, -25.0F, 0.0F }, 30.0F, { 0.0F, 0.0F, 1.0F } ) ) );

        Serialization::RigGraphNodeData write;
        write.Name   = "place";
        write.Kind   = "SetControl";
        write.Target = "Hand_CTRL";
        write.Space  = "Global";
        write.Inputs.push_back( LinkInput( "Transform", "reach", "Transform" ) );

        Serialization::RigGraphData graph;
        graph.Nodes.push_back( read );
        graph.Nodes.push_back( product );
        graph.Nodes.push_back( write );
        data.Graph = graph;

        return data;
    }
} // namespace

TEST( RigGraphTest, ARigWithAGraphRoundTripsByValueThroughTextAndThroughTheRuntime )
{
    const Skeleton                      skeleton = MakeArmRig();
    const Serialization::ControlRigData source   = RigWithGraph();

    const auto valid = Serialization::ValidateControlRigData( source );
    ASSERT_TRUE( valid.IsSuccess() ) << valid.GetError();

    const std::string text   = Serialization::WriteControlRig( source );
    const auto        parsed = Serialization::ParseControlRig( text );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    EXPECT_TRUE( parsed.GetValue() == source ) << "the text round trip lost or invented a field";

    // AND THROUGH THE RUNTIME, which is where a name could silently become an index and back again.
    ControlRigStage stage;
    const auto      built = Serialization::BuildControlRig( source, skeleton, stage );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();
    ASSERT_TRUE( stage.HasGraph() );
    EXPECT_EQ( stage.GetGraph().GetNodes().size(), 3U );

    const auto back = Serialization::BuildDataFromControlRig( source.Name, stage, skeleton );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_TRUE( back.GetValue() == source ) << "the runtime round trip changed the rig";
}

TEST( RigGraphTest, InputsMayBeListedInAnyOrderAndTheWalkStillReadsThemByPin )
{
    const Skeleton                source   = MakeArmRig();
    Serialization::ControlRigData shuffled = RigWithGraph();
    ASSERT_TRUE( shuffled.Graph.has_value() );
    std::swap( shuffled.Graph.value().Nodes[1].Inputs[0], shuffled.Graph.value().Nodes[1].Inputs[1] );

    ASSERT_TRUE( Serialization::ValidateControlRigData( shuffled ).IsSuccess() );

    ControlRigStage a;
    ControlRigStage b;
    ASSERT_TRUE( Serialization::BuildControlRig( RigWithGraph(), source, a ).IsSuccess() );
    ASSERT_TRUE( Serialization::BuildControlRig( shuffled, source, b ).IsSuccess() );

    const Ran ranA = RunOverBindPose( a, source );
    const Ran ranB = RunOverBindPose( b, source );
    ASSERT_TRUE( ranA.Result.IsSuccess() ) << ranA.Result.GetError();
    ASSERT_TRUE( ranB.Result.IsSuccess() ) << ranB.Result.GetError();

    for ( size_t bone = 0; bone < ranA.Pose.Size(); ++bone )
    {
        EXPECT_TRUE( MatNear( ranA.Pose[bone].ToMatrix(), ranB.Pose[bone].ToMatrix() ) )
             << "listing Alpha before A changed the meaning of the graph, at bone " << bone;
    }
}

TEST( RigGraphTest, ARigWithoutAGraphDoesNotGainTheFieldAndStillLoadsAsTheIdentitySolve )
{
    const Skeleton                skeleton = MakeArmRig();
    Serialization::ControlRigData plain    = RigWithGraph();
    plain.Graph.reset();

    ASSERT_TRUE( Serialization::ValidateControlRigData( plain ).IsSuccess() );

    const std::string text = Serialization::WriteControlRig( plain );
    // THE FIELD IS ABSENT FROM THE BYTES, not present-and-empty. That equivalence is the whole argument for
    // `kControlRigVersion` staying at 1: a generation-1 file has no Graph, and no Graph means what it has
    // always meant.
    EXPECT_EQ( text.find( "\"Graph\"" ), std::string::npos ) << text;
    EXPECT_NE( text.find( "\"FormatVersion\": 1" ), std::string::npos ) << text;

    const auto parsed = Serialization::ParseControlRig( text );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    EXPECT_FALSE( parsed.GetValue().Graph.has_value() );

    ControlRigStage stage;
    ASSERT_TRUE( Serialization::BuildControlRig( plain, skeleton, stage ).IsSuccess() );
    EXPECT_FALSE( stage.HasGraph() );

    const auto back = Serialization::BuildDataFromControlRig( plain.Name, stage, skeleton );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();
    EXPECT_FALSE( back.GetValue().Graph.has_value() ) << "a rig without a graph grew one on the way out";
}

TEST( RigGraphTest, TheFormatRefusesEverythingTheWalkRefusesAndNamesTheRow )
{
    const auto refusedFor = []( const std::function<void( Serialization::ControlRigData& )>& damage )
    {
        Serialization::ControlRigData data = RigWithGraph();
        damage( data );
        const auto valid = Serialization::ValidateControlRigData( data );
        return valid.IsSuccess() ? std::string() : valid.GetError();
    };

    // An empty graph: present-but-nothing is not a second spelling of absent.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes.clear(); } ).find( "no nodes" ),
               std::string::npos );

    // No sink — the same sentence `RefuseDiscardedWork` produces for the walk, because it IS that function.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes.pop_back(); } )
                    .find( "writes nothing" ),
               std::string::npos );

    // An unknown kind.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes[0].Kind = "GetSocket"; } )
                    .find( "does not know" ),
               std::string::npos );

    // A pin the kind does not have.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes[1].Inputs[0].Pin = "C"; } )
                    .find( "does not have" ),
               std::string::npos );

    // Two payloads on one pin: a precedence rule would make the loser invisible.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes[1].Inputs[1].Float = 1.0f; } )
                    .find( "payloads" ),
               std::string::npos );

    // No payload at all.
    EXPECT_NE(
         refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes[1].Inputs[1].Transform.reset(); } )
              .find( "neither a link nor a value" ),
         std::string::npos );

    // A link to a node that comes later — the rule that makes a cycle unwritable.
    {
        const std::string forward = refusedFor( []( Serialization::ControlRigData& d )
                                                { d.Graph->Nodes[1].Inputs[0].Link->Node = "place"; } );
        EXPECT_NE( forward.find( "comes later" ), std::string::npos ) << "[" << forward << "]";
    }

    // A link to an output pin the producing kind does not have.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d )
                           { d.Graph->Nodes[1].Inputs[0].Link->Pin = "Rotation"; } )
                    .find( "does not have" ),
               std::string::npos );

    // A Space on a kind that has none, and a missing one on a kind that needs it.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes[1].Space = "Local"; } )
                    .find( "no control space" ),
               std::string::npos );
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes[2].Space = "World"; } )
                    .find( "neither Local nor Global" ),
               std::string::npos );

    // A control this rig does not define.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes[2].Target = "Nose_CTRL"; } )
                    .find( "does not define" ),
               std::string::npos );

    // Two nodes with one name.
    EXPECT_NE( refusedFor( []( Serialization::ControlRigData& d ) { d.Graph->Nodes[1].Name = "elbowBone"; } )
                    .find( "two graph nodes" ),
               std::string::npos );
}

TEST( RigGraphTest, ABoneNameTheSkeletonDoesNotHaveIsRefusedAtBuildAndNamesTheSignature )
{
    const Skeleton                skeleton = MakeArmRig();
    Serialization::ControlRigData data     = RigWithGraph();
    ASSERT_TRUE( data.Graph.has_value() );
    data.Graph.value().Nodes[0].Target = "Tentacle";

    // The FILE cannot know; only a skeleton can, and that is where the refusal lives.
    ASSERT_TRUE( Serialization::ValidateControlRigData( data ).IsSuccess() );

    ControlRigStage stage;
    const auto      built = Serialization::BuildControlRig( data, skeleton, stage );
    ASSERT_FALSE( built.IsSuccess() );
    EXPECT_NE( built.GetError().find( "Tentacle" ), std::string::npos ) << built.GetError();
    EXPECT_NE( built.GetError().find( "elbowBone" ), std::string::npos ) << built.GetError();
}

TEST( RigGraphTest, EveryRigThisBuildShipsParsesAndAtLeastOneOfThemCarriesAGraph )
{
    // THE DEFECT THIS EXISTS FOR IS A HAND-AUTHORED CORPUS FILE NOBODY LOADS. Every assertion above is
    // over a rig this file constructs in C++, and every one of them would stay green while the `.derig`
    // on disk was unreadable — which is `PreloadCloudLayouts` again: a format tested, a corpus shipped,
    // and no test that ran the layer joining them.
    //
    // The repository root is found by walking up for a marker, the same trick the AnimGraphScript census
    // uses, because a corpus census has to read the tree it is testing.
    std::filesystem::path here = std::filesystem::current_path();
    std::filesystem::path root;
    for ( int i = 0; i < 12; ++i )
    {
        if ( std::filesystem::exists( here / "Desert" / "Desert" / "Source" / "Engine" ) )
        {
            root = here;
            break;
        }
        if ( !here.has_parent_path() || here.parent_path() == here )
        {
            break;
        }
        here = here.parent_path();
    }
    ASSERT_FALSE( root.empty() ) << "could not find the repository root from " << std::filesystem::current_path();

    const std::filesystem::path rigs = root / "Editor" / "Resources" / "Assets" / "Rigs";
    ASSERT_TRUE( std::filesystem::exists( rigs ) ) << rigs.string();

    size_t read    = 0;
    size_t graphed = 0;
    for ( const auto& entry : std::filesystem::directory_iterator( rigs ) )
    {
        if ( entry.path().extension() != ".derig" )
        {
            continue;
        }
        const auto rig = Serialization::LoadControlRigFile( entry.path() );
        ASSERT_TRUE( rig.IsSuccess() ) << entry.path().filename().string() << ": " << rig.GetError();
        ++read;
        if ( rig.GetValue().Graph.has_value() )
        {
            ++graphed;
        }
    }

    EXPECT_GT( read, 0U ) << "the corpus is empty, so this census proves nothing";
    // AND AT LEAST ONE OF THEM USES THE FEATURE. A format the corpus never exercises is a format whose
    // first real file is written by somebody with no example to copy — and the reachability of T5.5 from
    // content is the whole reason the `.derig` half of it was in scope at all.
    EXPECT_GE( graphed, 1U ) << "no shipped rig carries a graph, so nothing on disk runs T5.5";
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
