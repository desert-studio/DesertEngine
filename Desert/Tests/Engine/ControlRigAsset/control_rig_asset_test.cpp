// A12 — THE RIG BECOMES A FILE, A COMPONENT AND A FRAME, AND THE FOUR THINGS THAT HAVE TO BE TRUE.
//
//   1. A RIG WRITTEN AND READ BACK IS THE SAME RIG, BY VALUE. Not "the file is non-empty" and not "it
//      parsed" — `operator==` over the whole document, plus the same comparison after a trip through the
//      RUNTIME form (data -> ControlRigStage -> data), because that is the trip a rig actually takes and
//      it is where a name could silently become an index.
//   2. A SKELETON POSED WITH THE RIG PRODUCES DIFFERENT SKINNING MATRICES FROM THE SAME ONE WITHOUT IT,
//      and the difference is MEASURED. Every one of T5's four tiers could say "the stage ran"; none of
//      them could say this, because nothing built a rig from a file.
//   3. THE UNDRIVEN BONES ARE BIT-IDENTICAL. The negative control, and it is not decoration: a rig that
//      wrote the WHOLE pose would pass assertion 2 exactly as well as a rig that works.
//   4. THE PATH FROM THE FILE TO THE FRAME IS COMPLETE. A census over the source text, because this is
//      the defect this task exists to avoid: `PreloadCloudLayouts` scanned a directory, registered what it
//      found and was called by NOBODY — and every format test and every panel test passed, because not one
//      of them ran the layer. A suite that only tests the format cannot see that.

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Rig/ControlHierarchy.hpp>
#include <Engine/Animation/Rig/ControlRigStage.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/ControlRig.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using Desert::Animation::AnimationClip;
using Desert::Animation::Animator;
using Desert::Animation::BoneInfo;
using Desert::Animation::BoneTrack;
using Desert::Animation::BoneTransform;
using Desert::Animation::ControlHierarchy;
using Desert::Animation::ControlRigStage;
using Desert::Animation::ControlSpaceKind;
using Desert::Animation::FrameNumber;
using Desert::Animation::FrameTime;
using Desert::Animation::PROJECT_TICK_RATE;
using Desert::Animation::Skeleton;

namespace RigFile = Desert::Assets::Serialization;

namespace
{
    // The same eight-bone arm T5.4's suite uses, and for its reason: a one- or two-bone rig cannot tell
    // "wrote the driven bone" from "wrote the whole pose", because the two produce the same bytes. Every
    // bind carries a rotation as well as a translation, so a stage that composes spaces wrongly cannot
    // hide behind a pure-translation chain.
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

    // A DIFFERENT skeleton with the SAME bone names in a DIFFERENT ORDER, plus two bones the arm does not
    // have. This is the whole argument for a name-keyed file: a rig saved against the arm and loaded here
    // must drive the bone CALLED "Hand", not the bone that happened to be at index 3.
    Skeleton MakeReorderedRig()
    {
        const auto place = []( const glm::vec3& translation, float degrees, const glm::vec3& axis )
        {
            return glm::translate( glm::mat4( 1.0F ), translation ) *
                   glm::rotate( glm::mat4( 1.0F ), glm::radians( degrees ), axis );
        };

        std::vector<BoneInfo> bones( 10 );
        bones[0].Name               = "Pelvis";
        bones[0].LocalBindTransform = place( { 0.0F, 50.0F, 0.0F }, 3.0F, { 1.0F, 0.0F, 0.0F } );
        bones[1].Name               = "Spine";
        bones[1].ParentBoneID       = 0U;
        bones[1].LocalBindTransform = place( { 0.0F, 100.0F, 0.0F }, 15.0F, { 0.0F, 0.0F, 1.0F } );
        bones[2].Name               = "Tail";
        bones[2].ParentBoneID       = 1U;
        bones[2].LocalBindTransform = place( { -20.0F, -10.0F, 0.0F }, 25.0F, { 0.0F, 1.0F, 0.0F } );
        bones[3].Name               = "TailTip";
        bones[3].ParentBoneID       = 2U;
        bones[3].LocalBindTransform = place( { 0.0F, -10.0F, 0.0F }, -12.0F, { 0.0F, 0.0F, 1.0F } );
        bones[4].Name               = "Shoulder";
        bones[4].ParentBoneID       = 1U;
        bones[4].LocalBindTransform = place( { 10.0F, 20.0F, 0.0F }, -10.0F, { 1.0F, 0.0F, 0.0F } );
        bones[5].Name               = "Elbow";
        bones[5].ParentBoneID       = 4U;
        bones[5].LocalBindTransform = place( { 0.0F, -40.0F, 3.0F }, 5.0F, { 0.0F, 1.0F, 0.0F } );
        bones[6].Name               = "Hand";
        bones[6].ParentBoneID       = 5U;
        bones[6].LocalBindTransform = place( { 0.0F, -30.0F, 0.0F }, -8.0F, { 0.0F, 0.0F, 1.0F } );
        bones[7].Name               = "Finger";
        bones[7].ParentBoneID       = 6U;
        bones[7].LocalBindTransform = place( { 0.0F, -5.0F, 0.0F }, 3.0F, { 1.0F, 0.0F, 0.0F } );
        bones[8].Name               = "Prop";
        bones[8].LocalBindTransform = place( { 200.0F, 0.0F, 0.0F }, 40.0F, { 0.0F, 1.0F, 0.0F } );
        bones[9].Name               = "Cape";
        bones[9].ParentBoneID       = 1U;
        bones[9].LocalBindTransform = place( { 0.0F, -3.0F, -12.0F }, 7.0F, { 1.0F, 0.0F, 0.0F } );

        Skeleton rig( std::move( bones ) );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    constexpr uint32_t kHand    = 3;
    constexpr uint32_t kTail    = 5;
    constexpr uint32_t kTailTip = 6;

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

    RigFile::RigTransformData Placed( const glm::vec3& translation, float degrees, const glm::vec3& axis )
    {
        RigFile::RigTransformData out;
        out.Translation = translation;
        out.Rotation    = glm::angleAxis( glm::radians( degrees ), glm::normalize( axis ) );
        return out;
    }

    RigFile::ControlSpaceData Space( const char* kind, const char* target, float weight )
    {
        RigFile::ControlSpaceData out;
        out.Kind   = kind;
        out.Target = target;
        out.Weight = weight;
        return out;
    }

    /**
     * @brief A rig with every shape of reference the format can express, so a round trip that drops one is
     *        visible.
     *
     * Three controls: one in COMPONENT space, one following a BONE, and one following another CONTROL with
     * TWO weighted slots. Two drives, on two bones that are not adjacent in the chain. The controls are
     * written CHILD FIRST on purpose — a file is content, and its order must not decide whether it loads.
     */
    RigFile::ControlRigData ArmRigFile()
    {
        RigFile::ControlRigData data;
        data.Name = "ArmRig";

        RigFile::ControlElementData wrist;
        wrist.Name      = "Wrist_CTRL";
        wrist.ShapeName = "CircleXZ";
        wrist.Offset    = Placed( { 2.0F, 1.0F, 0.0F }, 5.0F, { 0.0F, 0.0F, 1.0F } );
        wrist.Pose      = Placed( { 11.0F, -4.0F, 6.0F }, -22.0F, { 1.0F, 0.0F, 0.0F } );
        wrist.Parents.push_back( Space( "Control", "Hand_CTRL", 0.75F ) );
        wrist.Parents.push_back( Space( "Bone", "Elbow", 0.25F ) );
        data.Controls.push_back( wrist );

        RigFile::ControlElementData hand;
        hand.Name      = "Hand_CTRL";
        hand.ShapeName = "CircleXY";
        hand.Offset    = Placed( { 5.0F, 0.0F, -3.0F }, 12.0F, { 0.0F, 1.0F, 0.0F } );
        hand.Pose      = Placed( { 45.0F, -18.0F, 27.0F }, 33.0F, { 0.0F, 0.0F, 1.0F } );
        hand.Parents.push_back( Space( "Component", "", 1.0F ) );
        data.Controls.push_back( hand );

        RigFile::ControlElementData tail;
        tail.Name      = "Tail_CTRL";
        tail.ShapeName = "Cube";
        tail.Offset    = Placed( { 0.0F, 0.0F, 0.0F }, 0.0F, { 0.0F, 1.0F, 0.0F } );
        tail.Pose      = Placed( { -33.0F, 14.0F, -9.0F }, 48.0F, { 0.0F, 1.0F, 0.0F } );
        tail.Parents.push_back( Space( "Bone", "Spine", 1.0F ) );
        data.Controls.push_back( tail );

        RigFile::ControlDriveData driveHand;
        driveHand.Control = "Hand_CTRL";
        driveHand.Bone    = "Hand";
        data.Drives.push_back( driveHand );

        RigFile::ControlDriveData driveTail;
        driveTail.Control = "Tail_CTRL";
        driveTail.Bone    = "Tail";
        data.Drives.push_back( driveTail );

        return data;
    }

    bool SameBytes( const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b )
    {
        if ( a.size() != b.size() )
        {
            return false;
        }
        for ( size_t i = 0; i < a.size(); ++i )
        {
            for ( int c = 0; c < 4; ++c )
            {
                for ( int r = 0; r < 4; ++r )
                {
                    if ( a[i][c][r] != b[i][c][r] )
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    float MaxAbsDelta( const glm::mat4& a, const glm::mat4& b )
    {
        float worst = 0.0F;
        for ( int c = 0; c < 4; ++c )
        {
            for ( int r = 0; r < 4; ++r )
            {
                worst = std::max( worst, std::abs( a[c][r] - b[c][r] ) );
            }
        }
        return worst;
    }

    // The repository root, found by walking up from the working directory until a marker is seen — the
    // same trick the AnimGraphScript census uses, because a source-text census has to read the tree.
    std::string RepoRoot()
    {
        std::filesystem::path here = std::filesystem::current_path();
        for ( int i = 0; i < 12; ++i )
        {
            if ( std::filesystem::exists( here / "Desert" / "Desert" / "Source" / "Engine" ) )
            {
                return here.string() + "/";
            }
            if ( !here.has_parent_path() || here.parent_path() == here )
            {
                break;
            }
            here = here.parent_path();
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
        {
            return {};
        }
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }
} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 1. Round trip, BY VALUE.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigAssetTest, ARigWrittenAndReadBackIsTheSameRigByValue )
{
    const RigFile::ControlRigData original = ArmRigFile();
    ASSERT_TRUE( RigFile::ValidateControlRigData( original ).IsSuccess() );

    const std::string text = RigFile::WriteControlRig( original );
    ASSERT_FALSE( text.empty() );

    auto reread = RigFile::ParseControlRig( text );
    ASSERT_TRUE( reread.IsSuccess() ) << reread.GetError();

    // THE COMPARISON IS `operator==` OVER THE WHOLE DOCUMENT. "the file parsed" would pass over a control
    // whose second parent slot was dropped, and the symptom of that is a control that follows one space
    // when it should blend two — which looks like a rigging mistake, not a serializer one.
    RigFile::ControlRigData expected = original;
    expected.FormatVersion           = RigFile::kControlRigVersion;
    EXPECT_EQ( reread.GetValue(), expected );

    // AND THE NAMES SURVIVED AS NAMES. Spelled out rather than left to operator== because this is the
    // decision the format is built on, and an equality that happened to hold while both sides stored
    // indices would be just as green.
    ASSERT_EQ( reread.GetValue().Controls.size(), 3U );
    EXPECT_EQ( reread.GetValue().Controls[0].Parents[0].Kind, "Control" );
    EXPECT_EQ( reread.GetValue().Controls[0].Parents[0].Target, "Hand_CTRL" );
    EXPECT_EQ( reread.GetValue().Controls[0].Parents[1].Kind, "Bone" );
    EXPECT_EQ( reread.GetValue().Controls[0].Parents[1].Target, "Elbow" );
    EXPECT_EQ( reread.GetValue().Drives[0].Bone, "Hand" );
}

TEST( ControlRigAssetTest, ARigThatHasBeenThroughTheRuntimeFormIsStillTheSameRig )
{
    const Skeleton                skeleton = MakeArmRig();
    const RigFile::ControlRigData original = ArmRigFile();

    ControlRigStage stage;
    const auto      built = RigFile::BuildControlRig( original, skeleton, stage );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();

    auto back = RigFile::BuildDataFromControlRig( original.Name, stage, skeleton );
    ASSERT_TRUE( back.IsSuccess() ) << back.GetError();

    // THIS IS THE TRIP A RIG ACTUALLY TAKES, and the one where a name could quietly become an index: the
    // stage holds indices, and the only thing that can turn them back into names is this skeleton. A
    // suite that only round-tripped the TEXT would be green over a writer that emitted bone 3 as "3".
    //
    // The comparison is order-insensitive in exactly one place: the drives, because SetDrives SORTS them
    // into the skeleton's resolve order by design. Everything else is compared as written.
    const RigFile::ControlRigData& result = back.GetValue();
    ASSERT_EQ( result.Controls.size(), original.Controls.size() );
    for ( const RigFile::ControlElementData& want : original.Controls )
    {
        const auto found = std::find_if( result.Controls.begin(), result.Controls.end(),
                                         [&want]( const RigFile::ControlElementData& have )
                                         { return have.Name == want.Name; } );
        ASSERT_NE( found, result.Controls.end() ) << "control '" << want.Name << "' did not survive";
        EXPECT_EQ( *found, want ) << "control '" << want.Name << "' came back different";
    }
    ASSERT_EQ( result.Drives.size(), original.Drives.size() );
    for ( const RigFile::ControlDriveData& want : original.Drives )
    {
        EXPECT_NE( std::find( result.Drives.begin(), result.Drives.end(), want ), result.Drives.end() )
             << "drive " << want.Control << " -> " << want.Bone << " did not survive";
    }
}

TEST( ControlRigAssetTest, TheFileSurvivesTheDiskAndTheRefusalsNameTheFile )
{
    const std::filesystem::path dir =
         std::filesystem::temp_directory_path() / "desert_a12_rig_roundtrip";
    std::error_code ec;
    std::filesystem::remove_all( dir, ec );
    std::filesystem::create_directories( dir, ec );
    const std::filesystem::path path = dir / "Arm.derig";

    const RigFile::ControlRigData original = ArmRigFile();
    ASSERT_TRUE( RigFile::SaveControlRigFile( path, original ).IsSuccess() );

    auto loaded = RigFile::LoadControlRigFile( path );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();

    RigFile::ControlRigData expected = original;
    expected.FormatVersion           = RigFile::kControlRigVersion;
    EXPECT_EQ( loaded.GetValue(), expected );

    // A RIG THE FORMAT REFUSES IS NEVER WRITTEN. The alternative is a `.derig` on disk that no build can
    // read, produced by the tool that was supposed to author it.
    RigFile::ControlRigData broken = original;
    broken.Drives.clear();
    const auto refused = RigFile::SaveControlRigFile( dir / "Broken.derig", broken );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_FALSE( std::filesystem::exists( dir / "Broken.derig" ) );

    std::filesystem::remove_all( dir, ec );
}

TEST( ControlRigAssetTest, AFileFromAnotherGenerationIsRefusedByNameInBothDirections )
{
    RigFile::ControlRigData future = ArmRigFile();
    future.FormatVersion           = RigFile::kControlRigVersion + 1;

    // Written through reflect-cpp directly, because WriteControlRig deliberately stamps the CURRENT
    // version — a writer that could emit another generation would be a second way for the number to be
    // wrong.
    const std::string text = RigFile::WriteControlRig( future );
    auto              ok   = RigFile::ParseControlRig( text );
    ASSERT_TRUE( ok.IsSuccess() ) << "WriteControlRig must stamp the current generation, not carry one in";

    const std::string tampered =
         "{\"FormatVersion\":99,\"Name\":\"X\",\"Controls\":[],\"Drives\":[]}";
    auto refused = RigFile::ParseControlRig( tampered );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "99" ), std::string::npos ) << refused.GetError();
    EXPECT_NE( refused.GetError().find( std::to_string( RigFile::kControlRigVersion ) ), std::string::npos )
         << refused.GetError();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 2 + 3. The rig reaches the skinning matrices, and only the bones it drives.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigAssetTest, ASkeletonPosedWithTheRigHasDifferentSkinningMatricesAndTheDifferenceIsMeasured )
{
    const Skeleton      skeleton = MakeArmRig();
    const AnimationClip clip     = ArmClip();

    Animator without( skeleton );
    without.Play( clip, false );
    without.SetTick( FrameTime{ FrameNumber{ 0 } } );
    const std::vector<glm::mat4> reference = without.GetPose().Matrices;
    ASSERT_EQ( reference.size(), 8U );

    Animator with( skeleton );
    with.Play( clip, false );

    auto       stage = std::make_unique<ControlRigStage>();
    const auto built = RigFile::BuildControlRig( ArmRigFile(), skeleton, *stage );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();
    ASSERT_TRUE( with.AttachRig( std::move( stage ) ).IsSuccess() );

    with.SetTick( FrameTime{ FrameNumber{ 0 } } );
    const std::vector<glm::mat4> posed = with.GetPose().Matrices;
    ASSERT_EQ( posed.size(), reference.size() );

    // THE MEASUREMENT. Not "it differs" — how much, on which bone, with the untouched bones named as the
    // floor. The floor here is EXACTLY zero and not an epsilon, because these are the same arithmetic on
    // the same inputs: a non-zero "noise floor" on an undriven bone would itself be the defect.
    const float handDelta    = MaxAbsDelta( posed[kHand], reference[kHand] );
    const float tailDelta    = MaxAbsDelta( posed[kTail], reference[kTail] );
    const float tailTipDelta = MaxAbsDelta( posed[kTailTip], reference[kTailTip] );

    EXPECT_GT( handDelta, 1.0F ) << "the rig drives Hand and the skinning matrix did not move";
    EXPECT_GT( tailDelta, 1.0F ) << "the rig drives Tail and the skinning matrix did not move";

    // A DRIVEN BONE'S CHILD MOVES TOO, and that is correct rather than incidental: the override is applied
    // in COMPONENT space and the pose is resolved through the chain, so TailTip follows Tail. Asserting it
    // is what distinguishes "the rig wrote a matrix" from "the rig posed a skeleton".
    EXPECT_GT( tailTipDelta, 1.0F ) << "TailTip follows Tail, so a rig that posed Tail must have moved it";

    // THE NEGATIVE CONTROL, AND IT IS HALF THE PROOF. A rig that overwrote the whole pose would pass every
    // assertion above. These four bones are not driven and are not downstream of anything driven, so they
    // must be BIT-identical — not near, identical.
    for ( const uint32_t untouched : { 0U, 1U, 2U, 7U } )
    {
        EXPECT_EQ( MaxAbsDelta( posed[untouched], reference[untouched] ), 0.0F )
             << "bone " << untouched << " is not driven by this rig and must not have moved";
    }

    // AND DETACHING PUTS IT BACK EXACTLY. The other half of "the stage is a stage": a pipeline whose
    // optional stage leaves residue is one that cannot be turned off.
    with.DetachRig();
    with.SetTick( FrameTime{ FrameNumber{ 0 } } );
    EXPECT_TRUE( SameBytes( with.GetPose().Matrices, reference ) );

    std::cout << "[A12] hand delta " << handDelta << ", tail delta " << tailDelta << ", tailtip delta "
              << tailTipDelta << "; undriven-bone floor 0 (exact)" << std::endl;
}

TEST( ControlRigAssetTest, TheSameFileOnAReorderedSkeletonDrivesTheBonesItNAMES )
{
    // THE WHOLE ARGUMENT FOR A NAME-KEYED FILE, as an assertion. This skeleton has the same bone names in
    // a different order and two extra bones; a file storing indices would drive "Pelvis" and "Elbow" here
    // and look perfectly healthy doing it.
    const Skeleton reordered = MakeReorderedRig();

    ControlRigStage stage;
    const auto      built = RigFile::BuildControlRig( ArmRigFile(), reordered, stage );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();

    ASSERT_EQ( stage.GetDrives().size(), 2U );
    for ( const auto& drive : stage.GetDrives() )
    {
        const std::string& bone    = reordered.GetBones()[drive.Bone].Name;
        const std::string& control = stage.GetHierarchy().Get( drive.Control ).Name;
        if ( control == "Hand_CTRL" )
        {
            EXPECT_EQ( bone, "Hand" );
        }
        else
        {
            EXPECT_EQ( control, "Tail_CTRL" );
            EXPECT_EQ( bone, "Tail" );
        }
    }

    // The BONE SPACE a control follows is resolved by name too, and its index here is not the arm's.
    const uint32_t tailControl = stage.GetHierarchy().Find( "Tail_CTRL" );
    ASSERT_NE( tailControl, ControlHierarchy::INVALID );
    const auto& parents = stage.GetHierarchy().Get( tailControl ).Parents;
    ASSERT_EQ( parents.size(), 1U );
    EXPECT_EQ( parents[0].Kind, ControlSpaceKind::Bone );
    EXPECT_EQ( reordered.GetBones()[parents[0].Index].Name, "Spine" );
    EXPECT_NE( parents[0].Index, 0U ) << "'Spine' is bone 0 on the arm and bone 1 here; an index-keyed "
                                         "file would be indistinguishable from a correct one without this";
}

TEST( ControlRigAssetTest, ABoneTheSkeletonDoesNotHaveIsRefusedByNameAndNotResolvedToIdentity )
{
    std::vector<BoneInfo> bones( 2 );
    bones[0].Name = "Root";
    bones[1].Name = "Only";
    bones[1].ParentBoneID = 0U;
    Skeleton stub( std::move( bones ) );
    stub.RecomputeOffsetMatrices();

    ControlRigStage stage;
    const auto      built = RigFile::BuildControlRig( ArmRigFile(), stub, stage );

    // REFUSED, NOT RESOLVED TO IDENTITY. The silent version of this puts every control at the origin and
    // gives the animator a character that poses wrongly with nothing in the log.
    ASSERT_FALSE( built.IsSuccess() );
    EXPECT_NE( built.GetError().find( "Elbow" ), std::string::npos ) << built.GetError();
    EXPECT_NE( built.GetError().find( "ArmRig" ), std::string::npos ) << built.GetError();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// The refusals the format owes, one assertion each.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigAssetTest, EveryShapeOfUnusableRigIsRefusedAndTheMessageNamesTheRow )
{
    struct Case
    {
        const char*                                     What;
        std::function<void( RigFile::ControlRigData& )> Break;
        const char*                                     MustMention;
    };

    const std::vector<Case> cases = {
         { "no controls", []( RigFile::ControlRigData& d ) { d.Controls.clear(); }, "controls" },
         { "no drives", []( RigFile::ControlRigData& d ) { d.Drives.clear(); }, "drives no bones" },
         { "empty control name", []( RigFile::ControlRigData& d ) { d.Controls[1].Name.clear(); }, "empty name" },
         { "duplicate control name",
           []( RigFile::ControlRigData& d ) { d.Controls[2].Name = d.Controls[1].Name; }, "Hand_CTRL" },
         { "unknown space kind",
           []( RigFile::ControlRigData& d ) { d.Controls[1].Parents[0].Kind = "Socket"; }, "Socket" },
         { "component space naming a target",
           []( RigFile::ControlRigData& d ) { d.Controls[1].Parents[0].Target = "Spine"; }, "Component space" },
         { "bone space naming nothing",
           []( RigFile::ControlRigData& d ) { d.Controls[2].Parents[0].Target.clear(); }, "names nothing" },
         { "control space naming an unknown control",
           []( RigFile::ControlRigData& d ) { d.Controls[0].Parents[0].Target = "Ghost_CTRL"; }, "Ghost_CTRL" },
         { "a control parented to itself",
           []( RigFile::ControlRigData& d ) { d.Controls[0].Parents[0].Target = d.Controls[0].Name; },
           "parented to itself" },
         { "a parent cycle",
           []( RigFile::ControlRigData& d )
           { d.Controls[1].Parents[0] = Space( "Control", "Wrist_CTRL", 1.0F ); },
           "cycle" },
         { "no parent space at all",
           []( RigFile::ControlRigData& d ) { d.Controls[1].Parents.clear(); }, "no parent space" },
         { "all weights zero",
           []( RigFile::ControlRigData& d )
           {
               d.Controls[0].Parents[0].Weight = 0.0F;
               d.Controls[0].Parents[1].Weight = 0.0F;
           },
           "all zero" },
         { "a non-finite pose",
           []( RigFile::ControlRigData& d )
           { d.Controls[1].Pose.Translation.y = std::numeric_limits<float>::quiet_NaN(); },
           "non-finite" },
         { "a zero scale",
           []( RigFile::ControlRigData& d ) { d.Controls[1].Offset.Scale.z = 0.0F; }, "zero component" },
         { "a drive naming an unknown control",
           []( RigFile::ControlRigData& d ) { d.Drives[0].Control = "Ghost_CTRL"; }, "Ghost_CTRL" },
         { "two drives on one bone",
           []( RigFile::ControlRigData& d ) { d.Drives[1].Bone = d.Drives[0].Bone; }, "Two drives" },
    };

    for ( const Case& c : cases )
    {
        SCOPED_TRACE( c.What );
        RigFile::ControlRigData data = ArmRigFile();
        c.Break( data );

        const auto verdict = RigFile::ValidateControlRigData( data );
        ASSERT_FALSE( verdict.IsSuccess() ) << "this rig was accepted and should not have been";

        // THE MESSAGE HAS TO NAME THE ROW. "the rig is invalid" is a morning spent bisecting a file; the
        // name of the control or the bone IS the defect, spelled out.
        std::string lowered = verdict.GetError();
        std::string needle  = c.MustMention;
        std::transform( lowered.begin(), lowered.end(), lowered.begin(),
                        []( unsigned char ch ) { return static_cast<char>( std::tolower( ch ) ); } );
        std::transform( needle.begin(), needle.end(), needle.begin(),
                        []( unsigned char ch ) { return static_cast<char>( std::tolower( ch ) ); } );
        EXPECT_NE( lowered.find( needle ), std::string::npos )
             << "the refusal does not mention '" << c.MustMention << "': " << verdict.GetError();
    }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 4. The path from the file to the frame is COMPLETE. The PreloadCloudLayouts census.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( ControlRigAssetTest, EveryLinkFromTheFileToTheSkinningMatricesHasACaller )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    struct Link
    {
        const char* File;
        const char* Needle;
        const char* Why;
    };

    // ONE ROW PER LINK, NAMED, and the count derived from the rows rather than pinned as a number — a gate
    // pinning a COUNT can be satisfied by editing the number.
    const std::vector<Link> links = {
         { "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp",
           "MakeReflected<ECS::ControlRigComponent, ECS::ControlRigData>",
           "without this the component is not serialized and a saved scene loses its rig" },
         { "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp", "\"ControlRigAsset\"",
           "without this the handle has no path handler and the slot round-trips as zero" },
         { "Desert/Desert/Source/Engine/Core/SceneAssetRoots.cpp", "ControlRigComponent",
           "without this the first eviction sweep drops the rig and the character silently poses from its "
           "clip alone" },
         { "Desert/Desert/Source/Engine/Core/Scene.cpp", "prepare<ECS::ControlRigComponent>",
           "without this the pool is created from inside the parallel phase" },
         { "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp", "SyncControlRig( registry, entity",
           "THE LINK THIS WHOLE TASK IS ABOUT: the per-frame call that turns the handle into a stage" },
         { "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp", "AttachRig( std::move( stage ) )",
           "without this the stage is built and never joins the pipeline" },
         { "Desert/Desert/Source/Engine/Assets/AssetPreloader.cpp", "PreloadControlRigs",
           "without this no rig is scanned and the Details slot can never offer one" },
         { "Editor/Source/EditorLayer.cpp", "PreloadControlRigs()",
           "PreloadCloudLayouts existed and was called by nobody; this row is that defect's headstone" },
         { "Editor/Source/EditorLayer.cpp",
           "AddSystem<ECS::AnimationECSSystem>( m_AnimationLibrary.get(), m_AssetManager.get() )",
           "without the manager the system cannot resolve a rig handle at all" },
         { "Runtime/Source/RuntimeLayer.cpp",
           "AddSystem<ECS::AnimationECSSystem>( m_AnimationLibrary.get(), m_AssetManager.get() )",
           "a rig that works in the editor and not in the packaged runtime is worse than no rig" },
         { "Editor/Source/EditorLayer.cpp", "ControlRigPanel",
           "without this the panel exists as a file nobody opens" },
         { "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp", "Animation::BuildFrame(",
           "THE CALLER ControlManipulator NEVER HAD: without it the controls are never drawn" },
         { "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp", "Animation::HitTest(",
           "without it a control cannot be selected in the viewport" },
         { "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp", "m_ControlDrag.Begin(",
           "without it a control cannot be moved, which is the third thing the panel owes" },
         { "Editor/Source/Editor/Panels/ViewportPanel/LightGizmoRenderer.cpp", "RenderControlRig( camera",
           "a Render function nobody calls is the exact shape of the defect this census exists for" },
         { "Editor/Source/Editor/Panels/PropertyEditor/PropertyEditorBuilder.cpp", "\"ControlRigAsset\"",
           "without this the Details page draws a raw handle number instead of a picker" },
         { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditorRegistrations.cpp",
           "ControlRigComponent",
           "without this the component has no Details page and cannot be added to an entity at all" },
    };

    size_t checked = 0;
    for ( const Link& link : links )
    {
        SCOPED_TRACE( std::string( link.File ) + " :: " + link.Needle );
        const std::string text = ReadFile( root + link.File );
        ASSERT_FALSE( text.empty() ) << "could not read " << link.File;
        EXPECT_NE( text.find( link.Needle ), std::string::npos ) << link.Why;
        ++checked;
    }
    EXPECT_EQ( checked, links.size() );
}
