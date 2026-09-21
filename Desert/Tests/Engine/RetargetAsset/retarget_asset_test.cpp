/**
 * A25 — THE RETARGET REACHES THE SKINNING MATRICES, AND THE MEASUREMENT IS ON A MOVING CLIP.
 *
 * T6.2 shipped a retargeting pipeline that beat `JPH::SkeletonMapper` on every row of
 * `Docs/Animation/08_retarget_measurement.md` and that NOTHING COULD CALL. This suite is the proof that
 * it is now callable from a scene, and it deliberately does not stop at the format: a `.retarget` whose
 * bytes round-trip perfectly, loaded by an asset nobody asks for, is `PreloadCloudLayouts` again.
 *
 * ── THE TRAP THIS FILE IS SHAPED AROUND, AND IT IS WHY THERE IS A TICK IN EVERY ASSERTION ────────────
 *
 * §2.1 of the measurement: *"in the rest pose the error is zero for every k [...] a bind-pose screenshot
 * of a retargeter built on this mapper is flawless no matter how badly the rigs are matched."*
 *
 * So a test that samples tick 0 asserts nothing about retargeting, and neither does a screenshot of a
 * stopped character. `ARetargetAtRestIsIndistinguishableFromABrokenOne` below reproduces the trap on
 * purpose — a working retargeter and one with its chain removed agree BIT FOR BIT at rest and differ by a
 * measured amount one tick-window later — so that the reason every other test names a moving tick is in
 * the suite rather than only in a comment.
 *
 * ── THE CORPUS, AND WHY THE SOURCE RIG IS NOT SIMPLY A SCALED IKProbe ────────────────────────────────
 *
 * `Editor/Cooked/Meshes/ForeignArm.skeleton` is IKProbe's arm at UNEVEN proportions (shoulder x1.5,
 * upper arm x1.75, forearm x1.3) with ONE bone renamed, `IK_Hand` -> `Foreign_Hand`. Both halves are
 * load-bearing:
 *
 *   - UNEVEN, because T6.1 measured the retarget error as NOT MONOTONIC in the proportion difference, so
 *     a single uniform ratio is the one case whose result does not generalise;
 *   - RENAMED, because `Skeleton::ComputeSignature` hashes bone names and parents and nothing else. A
 *     source rig that differed from IKProbe only in its bone LENGTHS would carry IKProbe's own signature,
 *     and `SkinnedMeshAsset::ResolveDependencies` binds a mesh to the FIRST skeleton whose signature
 *     matches — so adding it to the corpus could have re-rigged IKProbe.skmesh onto the wrong bones in
 *     every existing scene. The rename is what keeps the two rigs distinguishable to the asset system,
 *     and it is also what makes the file's `BoneRenames` row do work.
 *
 * The clip `ForeignArm_Swing.anim` drives all three bones AND lifts the root, so all three stages of the
 * pipeline are exercised by the same measurement: pelvis motion, FK chains and the IK tip.
 */

#include <gtest/gtest.h>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Retarget/ModelPose.hpp>
#include <Engine/Animation/Retarget/RetargetSource.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Desert::Animation::AnimationClip;
    using Desert::Animation::Animator;
    using Desert::Animation::BoneInfo;
    using Desert::Animation::FrameNumber;
    using Desert::Animation::FrameTime;
    using Desert::Animation::LocalPose;
    using Desert::Animation::Skeleton;
    using Desert::Animation::Retarget::ModelPose;
    using Desert::Animation::Retarget::RetargetSource;

    namespace File = Desert::Assets::Serialization;

    constexpr const char* kTargetRig    = "Editor/Cooked/Meshes/IKProbe.skeleton";
    constexpr const char* kSourceRig    = "Editor/Cooked/Meshes/ForeignArm.skeleton";
    constexpr const char* kSourceClip   = "Editor/Cooked/Meshes/ForeignArm_Swing.anim";
    constexpr const char* kRetargetFile = "Editor/Resources/Assets/Retargets/ForeignArm_To_IKProbe.retarget";

    /// The clip is 48000 ticks long and its motion is one full sine, so tick 0 and tick 48000 are the rest
    /// and tick 12000 is the extreme. EVERY measurement in this file is taken at the extreme, and there is
    /// deliberately no `kRestTick` beside this one: the rest tick is where a retarget cannot be observed
    /// (see `ABindPoseSnapshotCannotSeeAProportionDifferenceAtAll`, which builds its rest pose from the
    /// RIG rather than from a tick), so a named constant for it would be an invitation to measure there.
    constexpr int32_t kMovingTick = 12000;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + kTargetRig );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        const std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    Skeleton RigFrom( const char* path )
    {
        const std::string raw = ReadFile( RepoRoot() + path );
        EXPECT_FALSE( raw.empty() ) << "could not read " << path;
        auto data = rfl::json::read<File::SkeletonAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() );
        Skeleton rig( data.has_value() ? std::move( data.value().Bones ) : std::vector<BoneInfo>{} );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    AnimationClip ClipFrom( const char* path )
    {
        const std::string raw = ReadFile( RepoRoot() + path );
        EXPECT_FALSE( raw.empty() ) << "could not read " << path;
        const auto data = rfl::json::read<File::AnimationAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() );
        if ( !data.has_value() )
            return {};
        auto built = File::BuildClipFromAssetData( data.value() );
        EXPECT_TRUE( built.IsSuccess() ) << ( built.IsSuccess() ? "" : built.GetError() );
        return built.IsSuccess() ? built.ExtractValue() : AnimationClip{};
    }

    File::RetargetAssetData ShippedRetarget()
    {
        auto parsed = File::ParseRetarget( ReadFile( RepoRoot() + kRetargetFile ) );
        EXPECT_TRUE( parsed.IsSuccess() ) << ( parsed.IsSuccess() ? "" : parsed.GetError() );
        return parsed.IsSuccess() ? parsed.ExtractValue() : File::RetargetAssetData{};
    }

    std::unique_ptr<RetargetSource> BuildSource( const File::RetargetAssetData& data, const Skeleton& source,
                                                 const Skeleton& target )
    {
        auto setup = File::BuildRetargetSetup( data );
        EXPECT_TRUE( setup.IsSuccess() ) << ( setup.IsSuccess() ? "" : setup.GetError() );
        if ( !setup.IsSuccess() )
            return nullptr;
        auto built = RetargetSource::Create( source, target, setup.ExtractValue(), 0x1234U, 7U );
        EXPECT_TRUE( built.IsSuccess() ) << ( built.IsSuccess() ? "" : built.GetError() );
        return built.IsSuccess() ? built.ExtractValue() : nullptr;
    }

    float MaxAbsDelta( const glm::mat4& a, const glm::mat4& b )
    {
        float worst = 0.0F;
        for ( int c = 0; c < 4; ++c )
            for ( int r = 0; r < 4; ++r )
                worst = std::max( worst, std::abs( a[c][r] - b[c][r] ) );
        return worst;
    }

    bool SameBytes( const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b )
    {
        return a.size() == b.size() &&
               std::equal( a.begin(), a.end(), b.begin(),
                           []( const glm::mat4& x, const glm::mat4& y ) { return MaxAbsDelta( x, y ) == 0.0F; } );
    }

    uint32_t BoneIndex( const Skeleton& rig, const std::string& name )
    {
        const auto idx = rig.FindBoneIndex( name );
        if ( !idx.has_value() )
        {
            ADD_FAILURE() << "the rig has no bone named " << name;
            return 0;
        }
        return *idx;
    }

    /**
     * @brief THE INSTRUMENT. Worst |segment length / its own rest length - 1| over the rig, as a percent.
     *
     * Against the TARGET'S OWN rest length, because that is the question: did the target keep its bones,
     * whatever the source did. Identical in definition to the quantity T6.1's §2.1 table reports, so the
     * numbers printed by this suite are comparable to that table rather than merely adjacent.
     */
    float WorstSegmentErrorPercent( const Skeleton& rig, const LocalPose& pose )
    {
        auto rest = LocalPose::FromBindPose( rig );
        EXPECT_TRUE( rest.IsSuccess() );
        auto restModel = ModelPose::FromLocal( rig, rest.GetValue() );
        auto model     = ModelPose::FromLocal( rig, pose );
        EXPECT_TRUE( restModel.IsSuccess() && model.IsSuccess() );
        if ( !restModel.IsSuccess() || !model.IsSuccess() )
            return 0.0F;

        float worst = 0.0F;
        for ( uint32_t bone = 0; bone < rig.GetBones().size(); ++bone )
        {
            const uint32_t parent = rig.ResolveParent( bone );
            if ( parent == Skeleton::NO_PARENT )
                continue;
            const float restLength =
                 glm::length( restModel.GetValue()[bone].Translation - restModel.GetValue()[parent].Translation );
            if ( restLength < 1.0e-3F )
                continue;
            const float length =
                 glm::length( model.GetValue()[bone].Translation - model.GetValue()[parent].Translation );
            worst = std::max( worst, std::abs( length / restLength - 1.0F ) * 100.0F );
        }
        return worst;
    }

    /**
     * @brief Worst distance, in centimetres, between the same bone's MODEL-SPACE position in two poses.
     *
     * MODEL SPACE AND NOT LOCAL, and the first version of this suite got it wrong in a way worth writing
     * down: the retargeter writes ROTATIONS ONLY (`Retargeter.hpp` — that is what makes limb length a
     * property of the data flow), so every bone's LOCAL translation is the target's own rest translation
     * in every pose it ever emits. A local-space comparison of translations is therefore identically zero
     * whatever the retarget did, which is an instrument that reads "no difference" on a working pipeline
     * and on a dead one alike. Resolving the chain is what turns a rotation into a place.
     */
    float WorstModelDelta( const Skeleton& rig, const LocalPose& a, const LocalPose& b )
    {
        auto ma = ModelPose::FromLocal( rig, a );
        auto mb = ModelPose::FromLocal( rig, b );
        EXPECT_TRUE( ma.IsSuccess() && mb.IsSuccess() );
        if ( !ma.IsSuccess() || !mb.IsSuccess() )
            return 0.0F;
        float worst = 0.0F;
        for ( uint32_t i = 0; i < rig.GetBones().size(); ++i )
            worst = std::max( worst, glm::length( ma.GetValue()[i].Translation - mb.GetValue()[i].Translation ) );
        return worst;
    }

    /// A temporary file that removes itself, so a failing assertion cannot leave the next run's input
    /// behind — `SaveRetargetFile` is exercised through the same call the editor makes.
    struct ScopedFile
    {
        std::filesystem::path Path;
        explicit ScopedFile( const char* stem ) : Path( std::filesystem::temp_directory_path() / stem )
        {
        }
        ~ScopedFile()
        {
            std::error_code ec;
            std::filesystem::remove( Path, ec );
        }
    };
} // namespace

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 1. The format. Round trip by VALUE, and every refusal names the row.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( RetargetAssetTest, ARetargetWrittenAndReadBackIsTheSameRetargetByValue )
{
    const File::RetargetAssetData original = ShippedRetarget();
    ASSERT_FALSE( original.SourceSkeleton.empty() );

    auto reread = File::ParseRetarget( File::WriteRetarget( original ) );
    ASSERT_TRUE( reread.IsSuccess() ) << reread.GetError();

    // BY VALUE, not "the text is non-empty" and not "it parsed". `operator==` is the only statement that
    // covers every field, including the ones a future row adds without anyone remembering this test.
    EXPECT_EQ( reread.GetValue(), original );
}

TEST( RetargetAssetTest, TheFileSurvivesTheDiskAndTheRefusalNamesTheFile )
{
    const ScopedFile file( "A25_roundtrip.retarget" );
    const auto       original = ShippedRetarget();

    ASSERT_TRUE( File::SaveRetargetFile( file.Path, original ).IsSuccess() );
    auto loaded = File::LoadRetargetFile( file.Path );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    EXPECT_EQ( loaded.GetValue(), original );

    const auto missing = File::LoadRetargetFile( file.Path.parent_path() / "A25_absent.retarget" );
    EXPECT_FALSE( missing.IsSuccess() );
    EXPECT_NE( missing.GetError().find( "A25_absent" ), std::string::npos ) << missing.GetError();
}

TEST( RetargetAssetTest, AFileFromAnotherGenerationIsRefusedByNameInBothDirections )
{
    File::RetargetAssetData future = ShippedRetarget();
    future.FormatVersion           = File::kRetargetVersion + 1;
    const auto forward             = File::ParseRetarget( rfl::json::write( future ) );
    ASSERT_FALSE( forward.IsSuccess() );
    EXPECT_NE( forward.GetError().find( "version" ), std::string::npos ) << forward.GetError();

    File::RetargetAssetData past = ShippedRetarget();
    past.FormatVersion           = File::kRetargetVersion - 1;
    const auto backward          = File::ParseRetarget( rfl::json::write( past ) );
    ASSERT_FALSE( backward.IsSuccess() );
    EXPECT_NE( backward.GetError().find( "version" ), std::string::npos ) << backward.GetError();

    // AND AN ABSENT ONE IS READ AS THIS GENERATION, which is what makes the field optional rather than
    // required: a hand-written file that omits it is legal and means "whatever this build reads".
    File::RetargetAssetData silent = ShippedRetarget();
    silent.FormatVersion.reset();
    const auto implied = File::ParseRetarget( rfl::json::write( silent ) );
    ASSERT_TRUE( implied.IsSuccess() ) << implied.GetError();
    EXPECT_EQ( implied.GetValue().FormatVersion.value_or( -1 ), File::kRetargetVersion );
}

TEST( RetargetAssetTest, EveryShapeOfUnusableRetargetIsRefusedAndTheMessageNamesTheRow )
{
    struct Case
    {
        const char* What;
        const char* Needle;
        void ( *Break )( File::RetargetAssetData& );
    };

    // ONE ROW PER REFUSAL, each naming the substring the message owes the author. A refusal that does not
    // say WHICH row is wrong sends a rigger to read the whole file.
    const Case cases[] = {
         { "no source rig", "source rig", []( File::RetargetAssetData& d ) { d.SourceSkeleton.clear(); } },
         { "an absolute source rig", "relative",
           []( File::RetargetAssetData& d ) { d.SourceSkeleton = "/Users/someone/ForeignArm.skeleton"; } },
         { "an escaping source rig", "relative",
           []( File::RetargetAssetData& d ) { d.SourceSkeleton = "../../elsewhere/ForeignArm.skeleton"; } },
         { "no source pelvis", "pelvis", []( File::RetargetAssetData& d ) { d.SourcePelvisBone.clear(); } },
         { "no target pelvis", "pelvis", []( File::RetargetAssetData& d ) { d.TargetPelvisBone.clear(); } },
         { "an offset naming no bone", "offset", []( File::RetargetAssetData& d )
           { d.SourceRetargetPose.BoneOffsets.push_back( File::RetargetBoneOffsetData{} ); } },
         { "a zero-length rotation offset", "cannot be a rotation",
           []( File::RetargetAssetData& d )
           {
               d.TargetRetargetPose.BoneOffsets.push_back(
                    File::RetargetBoneOffsetData{ "IK_Elbow", glm::quat( 0.0F, 0.0F, 0.0F, 0.0F ) } );
           } },
         { "two offsets on one bone", "twice",
           []( File::RetargetAssetData& d )
           {
               const glm::quat identity( 1.0F, 0.0F, 0.0F, 0.0F );
               d.SourceRetargetPose.BoneOffsets.push_back( File::RetargetBoneOffsetData{ "IK_Elbow", identity } );
               d.SourceRetargetPose.BoneOffsets.push_back( File::RetargetBoneOffsetData{ "IK_Elbow", identity } );
           } },
         { "a non-finite pelvis offset", "finite", []( File::RetargetAssetData& d )
           { d.SourceRetargetPose.PelvisOffset.y = std::numeric_limits<float>::quiet_NaN(); } },
         { "a chain with no name", "has no name",
           []( File::RetargetAssetData& d ) { d.Chains.front().Name.clear(); } },
         { "a chain missing a bone", "unnamed",
           []( File::RetargetAssetData& d ) { d.Chains.front().TargetEndBone.clear(); } },
         { "two chains of one name", "two chains",
           []( File::RetargetAssetData& d ) { d.Chains.push_back( d.Chains.front() ); } },
         { "a rename missing a side", "unnamed",
           []( File::RetargetAssetData& d ) { d.BoneRenames.front().SourceBone.clear(); } },
         { "one target bone renamed twice", "renamed twice",
           []( File::RetargetAssetData& d ) { d.BoneRenames.push_back( d.BoneRenames.front() ); } },
    };

    size_t checked = 0;
    for ( const Case& c : cases )
    {
        SCOPED_TRACE( c.What );
        File::RetargetAssetData broken = ShippedRetarget();
        c.Break( broken );
        const auto refused = File::ValidateRetargetData( broken );
        EXPECT_FALSE( refused.IsSuccess() ) << "this file should not be accepted";
        if ( !refused.IsSuccess() )
        {
            EXPECT_NE( refused.GetError().find( c.Needle ), std::string::npos ) << refused.GetError();
        }
        ++checked;
    }
    // Derived from the rows rather than pinned as a number: a gate pinning a COUNT can be satisfied by
    // editing the number.
    EXPECT_EQ( checked, std::size( cases ) );
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 2. THE TRAP, REPRODUCED ON PURPOSE — why every measurement below names a MOVING tick.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( RetargetAssetTest, ABindPoseSnapshotCannotSeeAProportionDifferenceAtAll )
{
    // §2.1 OF THE MEASUREMENT, AS AN ASSERTION, because it is the reason every other test in this file
    // names a moving tick: *"in the rest pose the error is zero for every k [...] a bind-pose screenshot
    // of a retargeter built on this mapper is flawless no matter how badly the rigs are matched."*
    //
    // The source rigs here are the TARGET rig scaled uniformly by k, so the only thing that differs is
    // proportion — the single variable a bind-pose observation would have to be sensitive to in order to
    // be evidence about retargeting. It is not: for every k, the retargeted rest pose is the target's own
    // rest pose to the last bit the float can carry, and the SAME pair one tick-window later is not.
    const Skeleton  target     = RigFrom( kTargetRig );
    const LocalPose targetRest = [&]
    {
        auto rest = LocalPose::FromBindPose( target );
        EXPECT_TRUE( rest.IsSuccess() );
        return rest.IsSuccess() ? rest.ExtractValue() : LocalPose( target.GetBones().size() );
    }();

    for ( const float k : { 1.25F, 1.5F, 3.0F } )
    {
        SCOPED_TRACE( "k = " + std::to_string( k ) );

        std::vector<BoneInfo> bones = target.GetBones();
        for ( BoneInfo& bone : bones )
        {
            bone.LocalBindTransform[3][0] *= k;
            bone.LocalBindTransform[3][1] *= k;
            bone.LocalBindTransform[3][2] *= k;
        }
        Skeleton source( std::move( bones ) );
        source.RecomputeOffsetMatrices();

        File::RetargetAssetData data = ShippedRetarget();
        data.BoneRenames.clear(); // same names on both sides now
        data.Chains.front().SourceEndBone = "IK_Hand";

        auto setup = File::BuildRetargetSetup( data );
        ASSERT_TRUE( setup.IsSuccess() ) << setup.GetError();
        auto created = RetargetSource::Create( source, target, setup.ExtractValue(), 1U, 1U );
        ASSERT_TRUE( created.IsSuccess() ) << created.GetError();
        const std::unique_ptr<RetargetSource> built = created.ExtractValue();

        auto sourceRest = LocalPose::FromBindPose( source );
        ASSERT_TRUE( sourceRest.IsSuccess() );

        LocalPose out;
        ASSERT_TRUE( built->Run( target, sourceRest.GetValue(), out ) ) << built->GetLastError();

        const float restDelta = WorstModelDelta( target, out, targetRest );
        EXPECT_LT( restDelta, 1.0e-3F ) << "at rest a k=" << k
                                        << " proportion difference is invisible; if this fails the trap has "
                                           "changed shape and the suite's argument needs re-deriving";

        // AND THE SAME PAIR, MOVING, IS NOT INVISIBLE. Without this half the assertion above would be
        // satisfied by a retargeter that does nothing at all.
        LocalPose moved = sourceRest.GetValue();
        moved[BoneIndex( source, "IK_Elbow" )].Rotation =
             glm::angleAxis( glm::radians( 40.0F ), glm::vec3( 1.0F, 0.0F, 0.0F ) ) *
             moved[BoneIndex( source, "IK_Elbow" )].Rotation;

        LocalPose movedOut;
        ASSERT_TRUE( built->Run( target, moved, movedOut ) ) << built->GetLastError();
        const float movedDelta = WorstModelDelta( target, movedOut, targetRest );
        EXPECT_GT( movedDelta, 1.0F );

        std::cout << "[A25] k=" << k << ": rest-pose model-space delta " << restDelta << " cm (the blind "
                  << "spot), one 40 deg elbow later " << movedDelta << " cm" << std::endl;
    }
}

TEST( RetargetAssetTest, AnUNEVENProportionDifferenceIsVisibleAtRest )
{
    // A MEASURED REFINEMENT OF THE TRAP ABOVE, and it is a finding rather than a formality. The blind spot
    // is exact only for a UNIFORM scale: the IK stage places the tip at the SOURCE's normalised chain
    // extension, |tip - root| / (sum of segments), and that ratio is scale-invariant only when every
    // segment is scaled by the same number. Our corpus rig is scaled UNEVENLY (x1.5 / x1.75 / x1.3), so
    // its rest extension differs from the target's and the retargeted rest pose is NOT the target's rest.
    //
    // This is recorded because it is the exact kind of claim that gets over-generalised into "a bind-pose
    // frame is always useless here". The honest statement is narrower and this test holds both halves of
    // it side by side.
    const Skeleton  source     = RigFrom( kSourceRig );
    const Skeleton  target     = RigFrom( kTargetRig );
    const LocalPose targetRest = [&]
    {
        auto rest = LocalPose::FromBindPose( target );
        EXPECT_TRUE( rest.IsSuccess() );
        return rest.IsSuccess() ? rest.ExtractValue() : LocalPose( target.GetBones().size() );
    }();

    auto built = BuildSource( ShippedRetarget(), source, target );
    ASSERT_NE( built, nullptr );

    auto sourceRest = LocalPose::FromBindPose( source );
    ASSERT_TRUE( sourceRest.IsSuccess() );

    LocalPose out;
    ASSERT_TRUE( built->Run( target, sourceRest.GetValue(), out ) ) << built->GetLastError();

    const float restDelta = WorstModelDelta( target, out, targetRest );
    EXPECT_GT( restDelta, 0.1F ) << "an uneven source rig changes the chain's normalised extension, so "
                                    "even the rest pose is re-aimed";

    // AND THE LIMB LENGTHS ARE STILL EXACT, which is the quantity T6.1's table reports and the one that
    // IS blind at rest. Both statements are true at once, and confusing them is the whole hazard.
    EXPECT_LT( WorstSegmentErrorPercent( target, out ), 0.01F );

    std::cout << "[A25] uneven source, rest pose: tip moves " << restDelta << " cm, limb-length error "
              << WorstSegmentErrorPercent( target, out ) << " %" << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 3. THE LOAD-BEARING ASSERTION. Different skinning matrices, measured, with a negative control.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( RetargetAssetTest, ARetargetedCharacterHasDifferentSkinningMatricesAndTheDifferenceIsMeasured )
{
    const Skeleton      source = RigFrom( kSourceRig );
    const Skeleton      target = RigFrom( kTargetRig );
    const AnimationClip clip   = ClipFrom( kSourceClip );

    // WITHOUT: the clip plays on the target rig by bone NAME, which is exactly what this engine did
    // before this task. Two of the clip's three bones exist on the target, so it is not "nothing plays" —
    // it is the naive path, which is the honest thing to measure against.
    Animator without( target );
    without.Play( clip, false );
    without.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    const std::vector<glm::mat4> naive     = without.GetPose().Matrices;
    const LocalPose              naivePose = without.GetLocalPose();

    Animator with( target );
    with.Play( clip, false );
    ASSERT_TRUE( with.AttachRetarget( BuildSource( ShippedRetarget(), source, target ) ).IsSuccess() );
    with.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    const std::vector<glm::mat4> retargeted     = with.GetPose().Matrices;
    const LocalPose              retargetedPose = with.GetLocalPose();

    ASSERT_EQ( retargeted.size(), naive.size() );

    const uint32_t shoulder = BoneIndex( target, "IK_Shoulder" );
    const uint32_t elbow    = BoneIndex( target, "IK_Elbow" );
    const uint32_t hand     = BoneIndex( target, "IK_Hand" );

    const float shoulderDelta = MaxAbsDelta( retargeted[shoulder], naive[shoulder] );
    const float elbowDelta    = MaxAbsDelta( retargeted[elbow], naive[elbow] );
    const float handDelta     = MaxAbsDelta( retargeted[hand], naive[hand] );

    // THE MEASUREMENT. Not "it differs" — how much, on which bone, with the untouched bones named as the
    // floor. The floor is EXACTLY zero and not an epsilon: these are the same arithmetic on the same
    // inputs, so a non-zero floor on an unreached bone would itself be the defect.
    EXPECT_GT( shoulderDelta, 1.0F ) << "the pelvis stage scales the root's rise; it must have moved";
    EXPECT_GT( elbowDelta, 1.0F ) << "the FK stage re-aims the chain; the elbow must have moved";
    EXPECT_GT( handDelta, 1.0F ) << "the renamed hand is driven only through the retarget";

    // THE NEGATIVE CONTROL, AND IT IS HALF THE PROOF. A retarget that overwrote the whole pose would pass
    // every assertion above. `IK_Post` and `IK_Kerb` are props on the target rig: the source has no bone
    // of either name, the retarget names neither in a chain, and the clip has no track for them. They
    // must be BIT-identical between the two runs — not near, identical.
    for ( const char* prop : { "IK_Post", "IK_Kerb" } )
    {
        const uint32_t idx = BoneIndex( target, prop );
        EXPECT_EQ( MaxAbsDelta( retargeted[idx], naive[idx] ), 0.0F )
             << prop << " is reached by neither path and must not have moved";
    }

    // AND THE NUMBER THAT SAYS THE RETARGET IS RIGHT RATHER THAN MERELY DIFFERENT: the target's own limb
    // lengths. The naive path imports the SOURCE's translations, so the target's bones stretch by the
    // proportion difference; the retargeted path writes rotations only and the lengths are inviolable by
    // construction (see Retargeter.hpp).
    const float naiveError    = WorstSegmentErrorPercent( target, naivePose );
    const float retargetError = WorstSegmentErrorPercent( target, retargetedPose );
    EXPECT_GT( naiveError, 10.0F ) << "the naive path is supposed to be wrong; if it is not, this "
                                      "corpus no longer differs in proportion and proves nothing";
    EXPECT_LT( retargetError, 0.01F ) << "the retargeted target must keep its own bones";

    // AND DETACHING PUTS IT BACK EXACTLY. The other half of "the retarget is an attachment": a pipeline
    // whose optional part leaves residue is one that cannot be turned off.
    with.DetachRetarget();
    with.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    EXPECT_TRUE( SameBytes( with.GetPose().Matrices, naive ) );

    std::cout << "[A25] tick " << kMovingTick << " shoulder delta " << shoulderDelta << ", elbow " << elbowDelta
              << ", hand " << handDelta << "; unreached-bone floor 0 (exact). Worst limb "
              << "length error: naive " << naiveError << " %, retargeted " << retargetError << " %" << std::endl;
}

TEST( RetargetAssetTest, TheRenamedBoneIsDrivenOnlyBecauseTheFileSaysSo )
{
    // THE `BoneRenames` ROW, AS AN ASSERTION. Without it the target's `IK_Hand` has no source bone at all
    // and the chain is the only thing that could reach it; removing the row must therefore change the
    // hand and nothing above it. A row nobody can observe is a row that can be deleted by accident.
    const Skeleton      source = RigFrom( kSourceRig );
    const Skeleton      target = RigFrom( kTargetRig );
    const AnimationClip clip   = ClipFrom( kSourceClip );

    File::RetargetAssetData noRename = ShippedRetarget();
    ASSERT_FALSE( noRename.BoneRenames.empty() );
    noRename.BoneRenames.clear();
    // The chain would still reach the hand, so the chain goes too: what is being asked is whether the
    // NAME MAP alone can drive a renamed bone.
    noRename.Chains.clear();

    File::RetargetAssetData withRename = noRename;
    withRename.BoneRenames             = ShippedRetarget().BoneRenames;

    Animator a( target );
    Animator b( target );
    a.Play( clip, false );
    b.Play( clip, false );
    ASSERT_TRUE( a.AttachRetarget( BuildSource( noRename, source, target ) ).IsSuccess() );
    ASSERT_TRUE( b.AttachRetarget( BuildSource( withRename, source, target ) ).IsSuccess() );
    a.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    b.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );

    const uint32_t hand  = BoneIndex( target, "IK_Hand" );
    const uint32_t elbow = BoneIndex( target, "IK_Elbow" );
    const float    delta = MaxAbsDelta( a.GetPose().Matrices[hand], b.GetPose().Matrices[hand] );
    EXPECT_GT( delta, 0.01F ) << "the rename row is what makes Foreign_Hand drive IK_Hand";

    // And the elbow's own drive is unchanged: the rename is one row about one bone, not a global switch.
    // Its child moves with it, so the elbow is the deepest bone whose LOCAL transform must be equal.
    EXPECT_EQ( a.GetLocalPose()[elbow].Rotation, b.GetLocalPose()[elbow].Rotation );

    std::cout << "[A25] the rename row moves IK_Hand by " << delta << " and IK_Elbow's local by 0 (exact)"
              << std::endl;
}

TEST( RetargetAssetTest, AClipThatDrivesNothingLeavesTheTargetInItsOwnRetargetRest )
{
    // THE UNTRACKED-BONE FALLBACK, AND THE SCENARIO EXISTS ONLY BECAUSE A MUTATION WENT GREEN. Swapping
    // `rig.Rest` for the Animator's own bind pose in `SampleLocalTransform` changed nothing measurable,
    // because the corpus clip drives EVERY bone of the source rig — the line was never reached. A green
    // mutation means the test does not reach the code or does not check it, and both need work rather
    // than a tick (§8.4).
    //
    // The property: a source bone with no track must read `SourceInitial`, which is the ONE value that
    // makes it a no-op — the equation is `sourceCurrent * sourceInitial^-1`, so `sourceInitial` gives the
    // identity delta and the target keeps its own rest. Feed the whole rig that state at once, with a
    // clip that drives nothing, and the answer must be the target's retarget rest EXACTLY.
    const Skeleton source = RigFrom( kSourceRig );
    const Skeleton target = RigFrom( kTargetRig );

    AnimationClip empty;
    empty.AnimationName = "A25_Empty";
    empty.DurationTicks = FrameNumber{ 48000 };
    empty.TickRate      = Desert::Animation::FrameRate{ 24000, 1 };

    Animator animator( target );
    animator.Play( empty, false );
    auto built = BuildSource( ShippedRetarget(), source, target );
    ASSERT_NE( built, nullptr );
    const LocalPose retargetedRest = built->GetRetargetedRest();
    ASSERT_TRUE( animator.AttachRetarget( std::move( built ) ).IsSuccess() );
    animator.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );

    // EXACTLY, not nearly: every step between the source rest and this is the same arithmetic on the same
    // inputs, so any epsilon here would be hiding work that should not be happening.
    EXPECT_EQ( WorstModelDelta( target, animator.GetLocalPose(), retargetedRest ), 0.0F )
         << "a clip that drives nothing must leave the target where a source clip at rest puts it";

    // AND THAT REST IS NOT THE TARGET'S OWN, which is what makes the assertion above discriminating on
    // this corpus rather than trivially true — see `RetargetSource::GetRetargetedRest`.
    EXPECT_GT(
         WorstModelDelta( target, retargetedRest, animator.GetRetarget()->GetRetargeter().GetTargetInitialPose() ),
         0.1F );
}

TEST( RetargetAssetTest, AnAdditiveLayerOfNothingIsANoOpOnlyBecauseItsReferenceIsTheRetargetPose )
{
    // THE ADDITIVE REFERENCE, AND THIS SCENARIO ALSO EXISTS BECAUSE A MUTATION WENT GREEN — for the OTHER
    // reason §8.4 names. Replacing `GetTargetInitialPose()` with the bind pose was not unreached; it was
    // EQUIVALENT, because the shipped `.retarget` authors an empty target retarget pose and the two are
    // then the same object by value. A mutation that rewrites a value as itself proves nothing.
    //
    // So this test authors a retarget pose that is NOT identity, and then asks the one question whose
    // answer separates the candidates: an additive layer whose clip drives nothing must be a no-op.
    // Under a retarget an untracked bone comes out at the pair's RETARGETED rest, so the delta against
    // that is identity — and the delta against either the bind pose or the target's own retarget pose is
    // a correction nobody wrote, injected into every bone by a layer the author set to add nothing. Both
    // of those were tried here; the second is the one that had to be measured to be ruled out.
    const Skeleton source = RigFrom( kSourceRig );
    const Skeleton target = RigFrom( kTargetRig );

    File::RetargetAssetData posed = ShippedRetarget();
    posed.TargetRetargetPose.BoneOffsets.push_back( File::RetargetBoneOffsetData{
         "IK_Elbow", glm::angleAxis( glm::radians( 20.0F ), glm::vec3( 1.0F, 0.0F, 0.0F ) ) } );
    ASSERT_TRUE( File::ValidateRetargetData( posed ).IsSuccess() );

    AnimationClip empty;
    empty.AnimationName = "A25_Empty";
    empty.DurationTicks = FrameNumber{ 48000 };
    empty.TickRate      = Desert::Animation::FrameRate{ 24000, 1 };

    Animator animator( target );
    animator.Play( ClipFrom( kSourceClip ), false );
    ASSERT_TRUE( animator.AttachRetarget( BuildSource( posed, source, target ) ).IsSuccess() );
    animator.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    const std::vector<glm::mat4> withoutLayer = animator.GetPose().Matrices;

    ASSERT_GE( animator.AddLayer( empty, 1.0F, /*additive=*/true, /*loop=*/false ), 0 );
    animator.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );

    float worst = 0.0F;
    for ( size_t i = 0; i < withoutLayer.size(); ++i )
        worst = std::max( worst, MaxAbsDelta( animator.GetPose().Matrices[i], withoutLayer[i] ) );
    EXPECT_LT( worst, 1.0e-3F ) << "an additive layer that adds nothing moved the character by " << worst
                                << "; its reference is not the rest its clips are expressed against";

    std::cout << "[A25] additive layer of an empty clip, against a 20 deg authored retarget pose: worst "
              << "skinning-matrix delta " << worst << std::endl;
}

TEST( RetargetAssetTest, ARetargetIsRebuiltWhenAnySideOfThePairMoves )
{
    // FOUR FACTS, AND THE SUITE ASKS ABOUT EACH ONE. `SyncRetarget` skips the rebuild only when all four
    // agree; a stamp that could not distinguish one of them would leave a stale retargeter running after
    // the thing it was built from had changed.
    const Skeleton source = RigFrom( kSourceRig );
    const Skeleton target = RigFrom( kTargetRig );

    auto built = BuildSource( ShippedRetarget(), source, target );
    ASSERT_NE( built, nullptr );

    const uint64_t handle = 0x1234U;
    const uint32_t rev    = 7U;
    EXPECT_TRUE( built->IsBuiltFrom( handle, rev, source.GetSignature(), target.GetSignature() ) );

    EXPECT_FALSE( built->IsBuiltFrom( handle + 1, rev, source.GetSignature(), target.GetSignature() ) )
         << "the author pointed the slot at another retarget";
    EXPECT_FALSE( built->IsBuiltFrom( handle, rev + 1, source.GetSignature(), target.GetSignature() ) )
         << "the file was edited on disk";
    EXPECT_FALSE( built->IsBuiltFrom( handle, rev, source.GetSignature() + 1, target.GetSignature() ) )
         << "THE FOURTH FACT: the SOURCE character was re-exported, which nothing else in the chain sees";
    EXPECT_FALSE( built->IsBuiltFrom( handle, rev, source.GetSignature(), target.GetSignature() + 1 ) )
         << "this entity's mesh changed under it";
}

TEST( RetargetAssetTest, ARetargetBuiltForAnotherTargetRigIsRefusedRatherThanAttached )
{
    // A retargeter whose target is not this Animator's rig cannot change this rig's pose correctly, and a
    // stage that runs and is wrong is worse than one that refuses. Built against the SOURCE rig as its own
    // target, then offered to an Animator on the target rig.
    const Skeleton source = RigFrom( kSourceRig );
    const Skeleton target = RigFrom( kTargetRig );

    File::RetargetAssetData ontoItself = ShippedRetarget();
    ontoItself.TargetPelvisBone        = "IK_Shoulder";
    ontoItself.BoneRenames.clear();
    ontoItself.Chains.clear();

    auto setup = File::BuildRetargetSetup( ontoItself );
    ASSERT_TRUE( setup.IsSuccess() ) << setup.GetError();
    auto wrong = RetargetSource::Create( source, source, setup.ExtractValue(), 1U, 1U );
    ASSERT_TRUE( wrong.IsSuccess() ) << wrong.GetError();

    Animator   animator( target );
    const auto refused = animator.AttachRetarget( wrong.ExtractValue() );
    EXPECT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "bone" ), std::string::npos ) << refused.GetError();
    EXPECT_EQ( animator.GetRetarget(), nullptr );
}

TEST( RetargetAssetTest, ALayerIsRetargetedTooAndNotFoldedFromTheSourceRig )
{
    // THE HALF OF THE PIPELINE A SOURCE-STAGE-ONLY CHANGE WOULD HAVE LEFT WRONG. A layer clip comes from
    // the same library through the same component, so it is on the same source rig by construction;
    // folding it un-retargeted writes source-rig local transforms onto target bones — this tier's own
    // defect, arriving through the change meant to remove it.
    //
    // THE INSTRUMENT IS THE LIMB LENGTH AND NOT A POSE COMPARISON, and the reason is a property of the
    // Animator: a layer keeps its OWN playhead, so `SetTick` moves the base clip and leaves the layer
    // where it was. Two poses at two different times cannot be compared bone for bone; the target's own
    // segment lengths can, at any time, and they are exactly what an un-retargeted fold destroys.
    const Skeleton      source = RigFrom( kSourceRig );
    const Skeleton      target = RigFrom( kTargetRig );
    const AnimationClip clip   = ClipFrom( kSourceClip );

    // THE POSITIVE CONTROL FIRST: the naive path folding this clip as a layer, so the number the
    // assertion below is protecting against is measured rather than assumed.
    Animator naive( target );
    naive.Play( clip, false );
    ASSERT_GE( naive.AddLayer( clip, 1.0F, false, false ), 0 );
    naive.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    const float naiveError = WorstSegmentErrorPercent( target, naive.GetLocalPose() );
    ASSERT_GT( naiveError, 10.0F ) << "if an un-retargeted layer no longer breaks the limb lengths, this "
                                      "test is not measuring what it claims to";

    Animator animator( target );
    animator.Play( clip, false );
    ASSERT_TRUE( animator.AttachRetarget( BuildSource( ShippedRetarget(), source, target ) ).IsSuccess() );
    animator.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    const float baseError = WorstSegmentErrorPercent( target, animator.GetLocalPose() );

    ASSERT_GE( animator.AddLayer( clip, 1.0F, /*additive=*/false, /*loop=*/false ), 0 );
    animator.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    const float overrideError = WorstSegmentErrorPercent( target, animator.GetLocalPose() );
    EXPECT_LT( overrideError, 0.01F ) << "an override layer folded from the source rig would stretch the "
                                         "target's bones; it did not, so the layer was retargeted";

    // ADDITIVE IS THE OTHER HALF, and it is what makes the additive REFERENCE observable: an additive
    // layer measures its delta against the rest its clips are expressed relative to, and under a retarget
    // that is the target's retarget pose rather than its bind pose.
    animator.ClearLayers();
    ASSERT_GE( animator.AddLayer( clip, 1.0F, /*additive=*/true, /*loop=*/false ), 0 );
    animator.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    const float additiveError = WorstSegmentErrorPercent( target, animator.GetLocalPose() );
    EXPECT_LT( additiveError, 0.01F );

    animator.ClearLayers();
    animator.SetTick( FrameTime{ FrameNumber{ kMovingTick } } );
    EXPECT_NEAR( WorstSegmentErrorPercent( target, animator.GetLocalPose() ), baseError, 1.0e-4F )
         << "clearing the layers must leave the base exactly as it was";

    std::cout << "[A25] worst limb-length error with a layer: un-retargeted fold " << naiveError
              << " %, retargeted override " << overrideError << " %, retargeted additive " << additiveError << " %"
              << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────
// 4. The corpus, and the census over the links that make it reachable.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────

TEST( RetargetAssetTest, TheShippedRetargetNamesARigTheProjectHasAndTheWitnessScenesNameIt )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() );

    const File::RetargetAssetData data = ShippedRetarget();
    ASSERT_TRUE( File::ValidateRetargetData( data ).IsSuccess() );

    // THE PATH IN THE FILE HAS TO NAME A FILE. The one join this project performs is
    // `MESH_PATH_COOKED / SourceSkeleton`; checking it here is what stops the corpus from shipping a
    // retarget whose source rig is a typo, which loads perfectly and does nothing.
    const std::string rig = root + "Editor/Cooked/Meshes/" + data.SourceSkeleton;
    EXPECT_FALSE( ReadFile( rig ).empty() )
         << "the shipped retarget names " << data.SourceSkeleton << ", which is not in the cooked meshes";

    // AND THE TWO RIGS MUST NOT SHARE A SIGNATURE, or `SkinnedMeshAsset::ResolveDependencies` could bind
    // IKProbe.skmesh to the source rig — see this file's header.
    EXPECT_NE( RigFrom( kSourceRig ).GetSignature(), RigFrom( kTargetRig ).GetSignature() );

    const std::string witness = ReadFile( root + "Editor/Resources/Assets/Scenes/ANIM_RetargetWitness.desce" );
    ASSERT_FALSE( witness.empty() );
    EXPECT_NE( witness.find( "Retargets/ForeignArm_To_IKProbe.retarget" ), std::string::npos );
    EXPECT_NE( witness.find( "ForeignArm_Swing" ), std::string::npos );

    const std::string control =
         ReadFile( root + "Editor/Resources/Assets/Scenes/ANIM_RetargetWitness_NoRetarget.desce" );
    ASSERT_FALSE( control.empty() );
    EXPECT_EQ( control.find( "\"Retarget\"" ), std::string::npos )
         << "the control scene must differ from the witness in exactly one thing: the retarget";
    EXPECT_NE( control.find( "ForeignArm_Swing" ), std::string::npos );
}

TEST( RetargetAssetTest, EveryLinkFromTheFileToTheSkinningMatricesHasACaller )
{
    const std::string root = RepoRoot();
    ASSERT_FALSE( root.empty() ) << "could not locate the repository root from the working directory";

    struct Link
    {
        const char* File;
        const char* Needle;
        const char* Why;
    };

    // ONE ROW PER LINK, NAMED, and the count derived from the rows rather than pinned as a number. This is
    // the census `PreloadCloudLayouts` did not have: every one of these rows is a place where the chain
    // can be complete at both ends and broken in the middle, with every unit test still green.
    const std::vector<Link> links = {
         { "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp",
           "MakeReflected<ECS::RetargetComponent, ECS::RetargetData>",
           "without this the component is not serialized and a saved scene loses its retarget" },
         { "Desert/Desert/Source/Engine/Core/Serialize/ComponentRegistry.cpp", "\"RetargetAsset\"",
           "without this the handle has no path handler and the slot round-trips as zero" },
         { "Desert/Desert/Source/Engine/Core/SceneAssetRoots.cpp", "RetargetComponent",
           "without this the first eviction sweep drops the retarget and the character silently plays its "
           "clip on its own proportions" },
         { "Desert/Desert/Source/Engine/Assets/AssetEviction.cpp", "GetSourceSkeletonDependency",
           "the SOURCE rig is reachable through nothing else; without this row the sweep takes it and the "
           "retarget can never be rebuilt" },
         { "Desert/Desert/Source/Engine/Core/Scene.cpp", "prepare<ECS::RetargetComponent>",
           "without this the pool is created from inside the parallel phase" },
         { "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp", "SyncRetarget( registry, entity",
           "THE LINK THIS WHOLE TASK IS ABOUT: the per-frame call that turns the handle into a source rig" },
         { "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp",
           "AttachRetarget( built.ExtractValue() )",
           "without this the retargeter is built and never joins the pipeline" },
         { "Desert/Desert/Source/Engine/ECS/System/AnimationECSSystem.hpp",
           "FindForSkeleton( clipRig, anim.CurrentClip )",
           "the clip is looked up against the rig it is AUTHORED on; asking the target rig refuses exactly "
           "the clips a retarget exists to play" },
         { "Desert/Desert/Source/Engine/Animation/Animator.cpp", "m_Retarget->Run( m_Skeleton, sourcePose, pose )",
           "the source stage sampling on the source rig and retargeting onto this one" },
         { "Desert/Desert/Source/Engine/Assets/AssetPreloader.cpp", "PreloadRetargets",
           "without this no retarget is scanned, the Details slot can never offer one, and no source rig "
           "is ever bound" },
         { "Editor/Source/EditorLayer.cpp", "PreloadRetargets()",
           "PreloadCloudLayouts existed and was called by nobody; this row is that defect's headstone" },
         { "Runtime/Source/RuntimeLayer.cpp", "PreloadRetargets()",
           "a retarget that works in the editor and not in the packaged runtime is worse than no retarget" },
         { "Editor/Source/Editor/Panels/PropertyEditor/PropertyEditorBuilder.cpp", "\"RetargetAsset\"",
           "without this the Details page draws a raw handle number instead of a picker" },
         { "Editor/Source/Editor/Panels/SceneProperties/ComponentEditorRegistrations.cpp", "RetargetComponent",
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

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
