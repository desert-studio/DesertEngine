/**
 * T6.2 — THE RETARGET PIPELINE, STAGE BY STAGE, EACH WITH ITS OWN NUMBER.
 *
 * `08_retarget_measurement.md` (T6.1) is this suite's baseline, not its background reading. It measured
 * `JPH::SkeletonMapper` on these same two rigs and these same two clips and reported, per stage:
 *
 *     limb length   target x1.25 -> 1.325 %, x1.5 -> 0.390 %, x2.0 -> 2.763 %, x3.0 -> 7.934 %
 *                   -- and NOT MONOTONIC, so the error cannot be bounded by testing one pair of rigs
 *     pelvis        a target 1.5 times taller rises 100 cm where the source rises 100. Ratio 1.00
 *     chains        only the START of the chain is re-aimed; no parameterisation along it
 *
 * Every row above has a test below that measures the same quantity the same way, so the two documents
 * are directly comparable rather than merely adjacent.
 *
 * TWO CONSEQUENCES OF THE NON-MONOTONICITY, BOTH OF WHICH SHAPE THIS FILE:
 *
 *   1. the proportion tests take SEVERAL ratios, INCLUDING ADJACENT ONES (1.25 next to 1.5, where the
 *      measured error went DOWN), and one rig whose segments are scaled UNEVENLY;
 *   2. a frame is not evidence here in either direction. §2.1: "in the rest pose the error is zero for
 *      every k [...] a bind-pose screenshot of a retargeter built on this mapper is flawless no matter
 *      how badly the rigs are matched." The instrument is a length ratio on a MOVING clip, and it is
 *      named in each test.
 */

#include <gtest/gtest.h>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Retarget/ModelPose.hpp>
#include <Engine/Animation/Retarget/RetargetPose.hpp>
#include <Engine/Animation/Retarget/Retargeter.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Desert::Animation::BoneInfo;
    using Desert::Animation::BoneTransform;
    using Desert::Animation::ComponentPose;
    using Desert::Animation::FrameTime;
    using Desert::Animation::LocalPose;
    using Desert::Animation::Skeleton;
    using Desert::Animation::Retarget::ModelPose;
    using Desert::Animation::Retarget::RetargetChain;
    using Desert::Animation::Retarget::Retargeter;
    using Desert::Animation::Retarget::RetargetSetup;

    constexpr const char* kRigPath      = "Editor/Cooked/Meshes/IKProbe.skeleton";
    constexpr const char* kClipPath     = "Editor/Cooked/Meshes/IKProbe_Swing.anim";
    constexpr const char* kTwoBoneRig   = "Editor/Cooked/Meshes/TwoBoneProbe.skeleton";
    constexpr const char* kTwoBoneClip  = "Editor/Cooked/Meshes/TwoBoneProbe_Wave.anim";

    // The probe limb, and the only three-bone chain in the corpus. A limb is what a retargeter is judged
    // on, and IK_Shoulder is also the rig's root, so it doubles as the pelvis.
    constexpr const char* kRoot = "IK_Shoulder";
    constexpr const char* kMid  = "IK_Elbow";
    constexpr const char* kTip  = "IK_Hand";

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + kRigPath );
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

    std::vector<BoneInfo> BonesFrom( const char* path )
    {
        const std::string raw = ReadFile( RepoRoot() + path );
        EXPECT_FALSE( raw.empty() ) << "could not read " << path;
        auto data =
             rfl::json::read<Desert::Assets::Serialization::SkeletonAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() );
        return data.has_value() ? data.value().Bones : std::vector<BoneInfo>{};
    }

    Desert::Animation::AnimationClip ClipFrom( const char* path )
    {
        const std::string raw = ReadFile( RepoRoot() + path );
        EXPECT_FALSE( raw.empty() ) << "could not read " << path;
        const auto data =
             rfl::json::read<Desert::Assets::Serialization::AnimationAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() );
        if ( !data.has_value() )
            return {};
        auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data.value() );
        EXPECT_TRUE( built.IsSuccess() ) << ( built.IsSuccess() ? "" : built.GetError() );
        if ( !built.IsSuccess() )
            return {};
        return built.ExtractValue();
    }

    Skeleton MakeRig( std::vector<BoneInfo> bones )
    {
        Skeleton rig( std::move( bones ) );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    // The same rig, every bone offset scaled by `k`: the same character taller or shorter. Note that
    // Skeleton::ComputeSignature derives from names and parents only, so this rig has the SAME signature
    // as its source -- the clip<->rig binding already treats them as one rig, which is exactly why
    // nothing in the engine today stops a clip being played on the wrong proportions.
    std::vector<BoneInfo> Scaled( const std::vector<BoneInfo>& source, float k )
    {
        std::vector<BoneInfo> bones = source;
        for ( BoneInfo& b : bones )
        {
            b.LocalBindTransform[3][0] *= k;
            b.LocalBindTransform[3][1] *= k;
            b.LocalBindTransform[3][2] *= k;
        }
        return bones;
    }

    /// Scale ONE bone's offset from its parent, i.e. one SEGMENT of a limb. The uneven-proportion case --
    /// long upper arm, ordinary forearm -- which §7 of the measurement names as the one where its own
    /// numbers would not carry over.
    std::vector<BoneInfo> ScaledSegment( const std::vector<BoneInfo>& source, const std::string& bone, float k )
    {
        std::vector<BoneInfo> bones = source;
        for ( BoneInfo& b : bones )
        {
            if ( b.Name != bone )
                continue;
            b.LocalBindTransform[3][0] *= k;
            b.LocalBindTransform[3][1] *= k;
            b.LocalBindTransform[3][2] *= k;
        }
        return bones;
    }

    uint32_t BoneIndex( const Skeleton& rig, const std::string& name )
    {
        // An explicit check that RETURNS a value rather than ASSERT_TRUE: the assert's early return is a
        // macro clang-tidy's dataflow does not model, so every `*idx` after one still reads as an
        // unchecked optional access. The failure is on the record before 0 is handed back.
        const auto idx = rig.FindBoneIndex( name );
        if ( !idx.has_value() )
        {
            ADD_FAILURE() << "the rig has no bone named " << name;
            return 0;
        }
        return *idx;
    }

    LocalPose BindPose( const Skeleton& rig )
    {
        auto bind = LocalPose::FromBindPose( rig );
        EXPECT_TRUE( bind.IsSuccess() ) << ( bind.IsSuccess() ? "" : bind.GetError() );
        return bind.IsSuccess() ? bind.ExtractValue() : LocalPose( rig.GetBones().size() );
    }

    ModelPose ModelOf( const Skeleton& rig, const LocalPose& local )
    {
        auto model = ModelPose::FromLocal( rig, local );
        EXPECT_TRUE( model.IsSuccess() ) << ( model.IsSuccess() ? "" : model.GetError() );
        return model.IsSuccess() ? model.ExtractValue() : ModelPose{};
    }

    /// The clip's pose at `ticks` on top of the rig's bind pose -- what `Animator` does, minus the layers
    /// and the crossfade this measurement does not need.
    LocalPose PoseAt( const Skeleton& rig, const Desert::Animation::AnimationClip& clip, double ticks )
    {
        LocalPose       local = BindPose( rig );
        const FrameTime at{ Desert::Animation::FrameNumber{ static_cast<int32_t>( ticks ) }, 0.0F };
        for ( const auto& track : clip.Tracks )
        {
            if ( !track.HasKeys() )
                continue;
            const auto idx = rig.FindBoneIndex( track.BoneName );
            if ( idx )
                local[*idx] = track.Sample( at, clip.TickRate );
        }
        return local;
    }

    /// Eleven samples across the clip, endpoints included. The same eleven the T6.1 measurement used, so
    /// the worst-case numbers below are comparable to its table rather than merely similar.
    std::vector<double> SampleTicks( const Desert::Animation::AnimationClip& clip )
    {
        std::vector<double> out;
        const auto          duration = static_cast<double>( clip.DurationTicks.Value );
        for ( int i = 0; i <= 10; ++i )
            out.push_back( duration * i / 10.0 );
        return out;
    }

    /**
     * @brief THE INSTRUMENT. Worst |segment length / its own rest length - 1| over the rig, as a percent.
     *
     * A ratio against the TARGET'S OWN rest length, because that is the question: did the target keep its
     * bones, whatever the source did. Identical in definition to the quantity T6.1's §2.1 table reports.
     */
    float WorstSegmentErrorPercent( const Skeleton& rig, const LocalPose& rest, const LocalPose& pose )
    {
        const ModelPose restModel = ModelOf( rig, rest );
        const ModelPose model     = ModelOf( rig, pose );

        float worst = 0.0F;
        for ( uint32_t bone = 0; bone < rig.GetBones().size(); ++bone )
        {
            const uint32_t parent = rig.ResolveParent( bone );
            if ( parent == Skeleton::NO_PARENT )
                continue;
            const float restLength =
                 glm::length( restModel[bone].Translation - restModel[parent].Translation );
            if ( restLength < 1.0e-3F )
                continue;
            const float length = glm::length( model[bone].Translation - model[parent].Translation );
            worst              = std::max( worst, std::abs( length / restLength - 1.0F ) * 100.0F );
        }
        return worst;
    }

    RetargetSetup SetupFor( const char* sourcePelvis, const char* targetPelvis )
    {
        RetargetSetup setup;
        setup.SourcePelvisBone = sourcePelvis;
        setup.TargetPelvisBone = targetPelvis;
        return setup;
    }

    float DegreesBetween( const glm::quat& a, const glm::quat& b )
    {
        const float dot = std::abs( glm::dot( glm::normalize( a ), glm::normalize( b ) ) );
        return glm::degrees( 2.0F * std::acos( std::min( 1.0F, dot ) ) );
    }

    float AngleDegrees( const glm::quat& q )
    {
        return DegreesBetween( q, glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) );
    }

    /// IKProbe with one extra bone between the elbow and the hand -- the twist bone of a purchased rig
    /// that our source skeleton does not have. Its rest is ROTATED, and that is not decoration: T6.1
    /// §4 recorded a mutation that failed to redden because the first version of its scenario put the
    /// extra joint exactly where a direct mapping would have put it, making the mutation EQUIVALENT
    /// rather than harmless. A rotated rest is what makes the chain's arithmetic observable.
    std::vector<BoneInfo> RigWithTwist( const std::vector<BoneInfo>& source )
    {
        std::vector<BoneInfo> bones = source;

        uint32_t elbow = 0;
        uint32_t hand  = 0;
        for ( uint32_t i = 0; i < bones.size(); ++i )
        {
            if ( bones[i].Name == kMid )
                elbow = i;
            if ( bones[i].Name == kTip )
                hand = i;
        }

        const glm::mat4 handLocal = bones[hand].LocalBindTransform;
        const glm::vec3 offset( handLocal[3][0], handLocal[3][1], handLocal[3][2] );

        BoneInfo twist;
        twist.Name               = "IK_Twist";
        twist.ParentBoneID       = elbow;
        twist.LocalBindTransform = glm::rotate( glm::mat4( 1.0F ), glm::radians( 30.0F ),
                                                glm::vec3( 0.0F, 1.0F, 0.0F ) );
        twist.LocalBindTransform[3][0] = offset.x * 0.5F;
        twist.LocalBindTransform[3][1] = offset.y * 0.5F;
        twist.LocalBindTransform[3][2] = offset.z * 0.5F;

        bones.push_back( twist );
        const auto twistIndex = static_cast<uint32_t>( bones.size() - 1 );

        bones[hand].ParentBoneID          = twistIndex;
        bones[hand].LocalBindTransform[3][0] = offset.x * 0.5F;
        bones[hand].LocalBindTransform[3][1] = offset.y * 0.5F;
        bones[hand].LocalBindTransform[3][2] = offset.z * 0.5F;
        return bones;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. THE CURRENCY. Two claims, and the second is the reason the first one's type exists.
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, ModelPoseAgreesWithComponentPose )
{
    // TWO RESOLVERS FOR ONE QUESTION IS HOW TWO ANSWERS COME TO EXIST. ComponentPose resolves to mat4,
    // ModelPose resolves to TRS, and this is the relation assert that keeps them the same resolver.
    const Skeleton rig   = MakeRig( BonesFrom( kRigPath ) );
    const auto     clip  = ClipFrom( kClipPath );
    const LocalPose local = PoseAt( rig, clip, 12000.0 );

    const ModelPose trs = ModelOf( rig, local );
    ComponentPose   mats( rig, local );
    mats.ConvertAll();

    float worst = 0.0F;
    for ( uint32_t bone = 0; bone < rig.GetBones().size(); ++bone )
    {
        const glm::mat4 fromTrs = trs[bone].ToMatrix();
        const glm::mat4 fromMat = mats.Get( bone );
        for ( int col = 0; col < 4; ++col )
            for ( int row = 0; row < 4; ++row )
                worst = std::max( worst, std::abs( fromTrs[col][row] - fromMat[col][row] ) );
    }

    std::cout << "[ MEASURED ] ModelPose vs ComponentPose, worst matrix element: " << worst << "\n";
    EXPECT_LT( worst, 1.0e-3F );
}

TEST( RetargetPipeline, AMirroredBoneRoundTripsWhereTheMatrixRouteRefuses )
{
    // THE MEASURED REASON THE PIPELINE IS NOT BUILT ON ComponentPose (T6.1 §3.1): "FromMatrix is a
    // FAILURE PATH -- it rejects a non-positive determinant [...] embedding the mapper as a stage gives
    // us a pipeline that can refuse EVERY FRAME where today there is nothing to refuse."
    const Skeleton rig = MakeRig( BonesFrom( kRigPath ) );

    LocalPose local = BindPose( rig );
    local[BoneIndex( rig, kMid )].Scale = glm::vec3( -1.0F, 1.0F, 1.0F );

    ComponentPose mats( rig, local );
    mats.ConvertAll();
    const glm::mat4 handMatrix = mats.Get( BoneIndex( rig, kTip ) );
    const float     det        = glm::determinant( glm::mat3( handMatrix ) );
    const auto      decomposed = BoneTransform::FromMatrix( handMatrix );

    std::cout << "[ MEASURED ] mirrored hand, basis determinant " << det << "; FromMatrix "
              << ( decomposed.IsSuccess() ? "ACCEPTED" : "REFUSED" ) << "\n";
    ASSERT_FALSE( decomposed.IsSuccess() )
         << "the matrix route is supposed to refuse here; if it no longer does, this whole argument moves";

    const ModelPose model     = ModelOf( rig, local );
    auto            roundTrip = model.ToLocal( rig );
    ASSERT_TRUE( roundTrip.IsSuccess() ) << roundTrip.GetError();

    const LocalPose back  = roundTrip.ExtractValue();
    float           worst = 0.0F;
    for ( size_t bone = 0; bone < local.Size(); ++bone )
    {
        worst = std::max( worst, glm::length( back[bone].Translation - local[bone].Translation ) );
        worst = std::max( worst, glm::length( back[bone].Scale - local[bone].Scale ) );
        worst = std::max( worst, DegreesBetween( back[bone].Rotation, local[bone].Rotation ) );
    }
    std::cout << "[ MEASURED ] TRS round trip through a mirrored bone, worst disagreement: " << worst << "\n";
    EXPECT_LT( worst, 1.0e-3F );
}

// ---------------------------------------------------------------------------------------------------
// 2. THE CONDUCT ITSELF. If a rig retargeted onto ITSELF is not the identity, nothing below means
//    anything -- every later number would be measuring a broken pipeline rather than a stage.
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, ARigRetargetedOntoItselfIsTheIdentity )
{
    const Skeleton rig  = MakeRig( BonesFrom( kRigPath ) );
    const auto     clip = ClipFrom( kClipPath );

    Retargeter retargeter;
    ASSERT_TRUE( retargeter.Initialize( rig, rig, SetupFor( kRoot, kRoot ) ).IsSuccess() );

    float     worst = 0.0F;
    LocalPose out;
    for ( const double tick : SampleTicks( clip ) )
    {
        const LocalPose source = PoseAt( rig, clip, tick );
        const auto      done   = retargeter.Retarget( source, out );
        ASSERT_TRUE( done.IsSuccess() ) << done.GetError();

        const ModelPose want = ModelOf( rig, source );
        const ModelPose got  = ModelOf( rig, out );
        for ( size_t bone = 0; bone < out.Size(); ++bone )
            worst = std::max( worst, glm::length( got[bone].Translation - want[bone].Translation ) );
    }
    std::cout << "[ MEASURED ] identity retarget, worst bone displacement: " << worst << " cm\n";
    EXPECT_LT( worst, 1.0e-2F );
}

// ---------------------------------------------------------------------------------------------------
// 3. STAGE 2, THE FK DELTA: LIMB LENGTH ACROSS SEVERAL PROPORTION RATIOS, ADJACENT ONES INCLUDED.
//
//    This is the row T6.1 §2.1 measured at 1.325 % / 0.390 % / 2.763 % / 7.934 % and found NOT MONOTONIC
//    in k. The k = 1.0 row is this instrument's own noise floor: same rig, same arithmetic, no
//    proportion difference at all.
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, LimbLengthSurvivesEveryProportionRatio )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const auto     clip   = ClipFrom( kClipPath );

    struct Row
    {
        const char*           Label;
        std::vector<BoneInfo> Bones;
    };
    std::vector<Row> rows;
    rows.push_back( { "k=1.0  (noise floor)", Scaled( bones, 1.0F ) } );
    rows.push_back( { "k=1.25", Scaled( bones, 1.25F ) } );
    rows.push_back( { "k=1.5 ", Scaled( bones, 1.5F ) } );
    rows.push_back( { "k=2.0 ", Scaled( bones, 2.0F ) } );
    rows.push_back( { "k=3.0 ", Scaled( bones, 3.0F ) } );
    rows.push_back( { "upper x2.0, forearm x1.0 (uneven)", ScaledSegment( bones, kMid, 2.0F ) } );
    rows.push_back( { "upper x1.0, forearm x2.5 (uneven)", ScaledSegment( bones, kTip, 2.5F ) } );

    for ( const Row& row : rows )
    {
        const Skeleton target = MakeRig( row.Bones );

        Retargeter retargeter;
        const auto ready = retargeter.Initialize( source, target, SetupFor( kRoot, kRoot ) );
        ASSERT_TRUE( ready.IsSuccess() ) << ready.GetError();

        const LocalPose rest = retargeter.GetTargetInitialPose();

        float     worst     = 0.0F;
        float     worstRest = 0.0F;
        LocalPose out;
        for ( const double tick : SampleTicks( clip ) )
        {
            const auto done = retargeter.Retarget( PoseAt( source, clip, tick ), out );
            ASSERT_TRUE( done.IsSuccess() ) << done.GetError();
            worst = std::max( worst, WorstSegmentErrorPercent( target, rest, out ) );
        }
        worstRest = WorstSegmentErrorPercent( target, rest, rest );

        std::cout << "[ MEASURED ] " << row.Label << "  worst limb-length error over the clip: " << worst
                  << " %   (at rest: " << worstRest << " %)\n";

        // 0.001 % on a 60 cm forearm is 0.6 micrometres. T6.1's smallest non-zero row was 0.390 %.
        EXPECT_LT( worst, 1.0e-3F ) << "target rig: " << row.Label;
    }
}

// ---------------------------------------------------------------------------------------------------
// 4. RETARGET POSES: A DIFFERENT REST ORIENTATION IS ABSORBED, AND IT IS THE DELTA THAT ABSORBS IT.
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, ADifferentRestOrientationIsAbsorbedExactly )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const auto     clip   = ClipFrom( kClipPath );

    // The target differs ONLY in rest orientation: its elbow rests rotated 45 degrees. T6.1 measured
    // 2.98e-05 % here through Jolt, and this is the one place the two implementations should agree --
    // the equation is the same equation.
    std::vector<BoneInfo> targetBones = bones;
    for ( BoneInfo& b : targetBones )
    {
        if ( b.Name != kMid )
            continue;
        const glm::vec3 offset( b.LocalBindTransform[3][0], b.LocalBindTransform[3][1],
                                b.LocalBindTransform[3][2] );
        b.LocalBindTransform = b.LocalBindTransform *
                               glm::rotate( glm::mat4( 1.0F ), glm::radians( 45.0F ),
                                            glm::vec3( 1.0F, 0.0F, 0.0F ) );
        b.LocalBindTransform[3][0] = offset.x;
        b.LocalBindTransform[3][1] = offset.y;
        b.LocalBindTransform[3][2] = offset.z;
    }
    const Skeleton target = MakeRig( targetBones );

    Retargeter retargeter;
    ASSERT_TRUE( retargeter.Initialize( source, target, SetupFor( kRoot, kRoot ) ).IsSuccess() );

    const LocalPose targetRest = retargeter.GetTargetInitialPose();
    const ModelPose targetRestModel = ModelOf( target, targetRest );
    const ModelPose sourceRestModel = ModelOf( source, retargeter.GetSourceInitialPose() );

    float     worstLength = 0.0F;
    float     worstDelta  = 0.0F;
    float     restSpread  = 0.0F;
    LocalPose out;
    for ( const double tick : SampleTicks( clip ) )
    {
        const LocalPose sourcePose = PoseAt( source, clip, tick );
        ASSERT_TRUE( retargeter.Retarget( sourcePose, out ).IsSuccess() );
        worstLength = std::max( worstLength, WorstSegmentErrorPercent( target, targetRest, out ) );

        const ModelPose sourceModel = ModelOf( source, sourcePose );
        const ModelPose targetModel = ModelOf( target, out );
        for ( const char* name : { kRoot, kMid, kTip } )
        {
            const uint32_t s = BoneIndex( source, name );
            const uint32_t t = BoneIndex( target, name );
            const glm::quat sourceDelta =
                 sourceModel[s].Rotation * glm::inverse( sourceRestModel[s].Rotation );
            const glm::quat targetDelta =
                 targetModel[t].Rotation * glm::inverse( targetRestModel[t].Rotation );
            worstDelta = std::max( worstDelta, DegreesBetween( sourceDelta, targetDelta ) );
        }
    }
    // The negative control for "absorbed": the two rest poses really do differ, so the instrument is
    // looking at something. Without this the number above is indistinguishable from two identical rigs.
    restSpread = DegreesBetween( sourceRestModel[BoneIndex( source, kMid )].Rotation,
                                 targetRestModel[BoneIndex( target, kMid )].Rotation );

    std::cout << "[ MEASURED ] rest orientations differ by " << restSpread
              << " deg; worst limb-length error " << worstLength << " %, worst FK delta disagreement "
              << worstDelta << " deg\n";
    EXPECT_GT( restSpread, 30.0F );
    EXPECT_LT( worstLength, 1.0e-3F );
    EXPECT_LT( worstDelta, 1.0e-2F );
}

TEST( RetargetPipeline, AnAuthoredRetargetPoseMovesTheRestItIsAppliedTo )
{
    const Skeleton rig = MakeRig( BonesFrom( kRigPath ) );

    Desert::Animation::Retarget::RetargetPose pose;
    pose.SetBoneRotationOffset( kMid, glm::angleAxis( glm::radians( 40.0F ), glm::vec3( 1.0F, 0.0F, 0.0F ) ) );
    pose.SetPelvisTranslationOffset( glm::vec3( 0.0F, 25.0F, 0.0F ) );

    const uint32_t pelvis  = BoneIndex( rig, kRoot );
    auto           applied = pose.Apply( rig, pelvis );
    ASSERT_TRUE( applied.IsSuccess() ) << applied.GetError();

    const LocalPose moved = applied.ExtractValue();
    const LocalPose bind  = BindPose( rig );

    const float elbowTurn =
         DegreesBetween( moved[BoneIndex( rig, kMid )].Rotation, bind[BoneIndex( rig, kMid )].Rotation );
    const float pelvisRise = moved[pelvis].Translation.y - bind[pelvis].Translation.y;

    std::cout << "[ MEASURED ] retarget pose: elbow turned " << elbowTurn << " deg, pelvis raised "
              << pelvisRise << " cm\n";
    EXPECT_NEAR( elbowTurn, 40.0F, 1.0e-2F );
    EXPECT_NEAR( pelvisRise, 25.0F, 1.0e-3F );

    // Bone lengths are untouched by a rotation offset -- a retarget pose turns a rig, it does not
    // rebuild it.
    EXPECT_LT( WorstSegmentErrorPercent( rig, bind, moved ), 1.0e-3F );
}

TEST( RetargetPipeline, ARetargetPoseNamingAnUnknownBoneIsRefused )
{
    const Skeleton rig = MakeRig( BonesFrom( kRigPath ) );

    Desert::Animation::Retarget::RetargetPose pose;
    pose.SetBoneRotationOffset( "IK_Elbwo", glm::quat( 1.0F, 0.0F, 0.0F, 0.0F ) );

    const auto applied = pose.Apply( rig, BoneIndex( rig, kRoot ) );
    ASSERT_FALSE( applied.IsSuccess() );
    std::cout << "[ REFUSED ] " << applied.GetError() << "\n";
    EXPECT_NE( applied.GetError().find( "IK_Elbwo" ), std::string::npos );
}

// ---------------------------------------------------------------------------------------------------
// 5. STAGE 1, THE PELVIS. T6.1 §2.2 measured 1.00 here, on this rig and this clip, and named what it
//    costs: "a step over a 50 cm kerb stays a 50 cm step on a character half again as tall."
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, ThePelvisRisesByTheRatioOfTheTwoRigsHeights )
{
    const auto     bones  = BonesFrom( kTwoBoneRig );
    const Skeleton source = MakeRig( bones );
    const auto     clip   = ClipFrom( kTwoBoneClip );

    // TwoBoneProbe + TwoBoneProbe_Wave is the ONLY pair in the corpus whose root actually translates;
    // IKProbe_Swing's position keys are constant, so any pelvis number taken from it would be a
    // statement about the clip rather than about the stage. T6.1 made the same choice for the same
    // reason.
    for ( const float k : { 1.5F, 2.0F, 0.5F } )
    {
        const Skeleton target = MakeRig( Scaled( bones, k ) );

        Retargeter retargeter;
        const auto ready = retargeter.Initialize( source, target, SetupFor( "Base", "Base" ) );
        ASSERT_TRUE( ready.IsSuccess() ) << ready.GetError();
        EXPECT_NEAR( retargeter.GetPelvisHeightScale(), k, 1.0e-4F );

        const uint32_t  sourcePelvis    = BoneIndex( source, "Base" );
        const uint32_t  targetPelvis    = BoneIndex( target, "Base" );
        const ModelPose sourceRestModel = ModelOf( source, retargeter.GetSourceInitialPose() );
        const ModelPose targetRestModel = ModelOf( target, retargeter.GetTargetInitialPose() );

        float     bestSourceRise = 0.0F;
        float     matchingRatio  = 0.0F;
        LocalPose out;
        for ( const double tick : SampleTicks( clip ) )
        {
            const LocalPose sourcePose = PoseAt( source, clip, tick );
            ASSERT_TRUE( retargeter.Retarget( sourcePose, out ).IsSuccess() );

            const float sourceRise = ModelOf( source, sourcePose )[sourcePelvis].Translation.y -
                                     sourceRestModel[sourcePelvis].Translation.y;
            const float targetRise = ModelOf( target, out )[targetPelvis].Translation.y -
                                     targetRestModel[targetPelvis].Translation.y;
            if ( std::abs( sourceRise ) > std::abs( bestSourceRise ) )
            {
                bestSourceRise = sourceRise;
                matchingRatio  = targetRise / sourceRise;
            }
        }

        std::cout << "[ MEASURED ] target x" << k << ": source root rises " << bestSourceRise
                  << " cm, target root rises " << ( bestSourceRise * matchingRatio ) << " cm, ratio "
                  << matchingRatio << "  (T6.1 measured 1.00 through JPH::SkeletonMapper)\n";
        EXPECT_GT( std::abs( bestSourceRise ), 50.0F ) << "the clip must actually move the root";
        EXPECT_NEAR( matchingRatio, k, 1.0e-3F );
    }
}

TEST( RetargetPipeline, APelvisAtZeroHeightIsRefused )
{
    auto bones = BonesFrom( kTwoBoneRig );
    for ( BoneInfo& b : bones )
    {
        if ( b.Name == "Base" )
            b.LocalBindTransform[3][1] = 0.0F;
    }
    const Skeleton flat   = MakeRig( bones );
    const Skeleton normal = MakeRig( BonesFrom( kTwoBoneRig ) );

    Retargeter retargeter;
    const auto ready = retargeter.Initialize( flat, normal, SetupFor( "Base", "Base" ) );
    ASSERT_FALSE( ready.IsSuccess() );
    std::cout << "[ REFUSED ] " << ready.GetError() << "\n";
    EXPECT_NE( ready.GetError().find( "height" ), std::string::npos );

    // And the refusal is load-bearing rather than decorative: a retargeter that refused still refuses
    // when asked to run, instead of quietly emitting a rest pose.
    LocalPose  out;
    const auto ran = retargeter.Retarget( BindPose( flat ), out );
    EXPECT_FALSE( ran.IsSuccess() );
}

// ---------------------------------------------------------------------------------------------------
// 6. CHAINS. T6.1 §4: Jolt "re-aims the START of the chain [...] there is no parameterisation along it",
//    and its own header admits "very extreme animation poses will show artifacts".
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, AChainRetargetsAlongItsWholeLengthAndNotOnlyItsStart )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( RigWithTwist( bones ) );
    const auto     clip   = ClipFrom( kClipPath );

    RetargetSetup setup = SetupFor( kRoot, kRoot );
    setup.Chains.push_back( RetargetChain{ "arm", kRoot, kTip, kRoot, kTip, false } );

    Retargeter retargeter;
    const auto ready = retargeter.Initialize( source, target, setup );
    ASSERT_TRUE( ready.IsSuccess() ) << ready.GetError();

    ASSERT_EQ( retargeter.GetChains().size(), 1U );
    const auto& chain = retargeter.GetChains().front();
    EXPECT_EQ( chain.SourceRun.size(), 3U );
    EXPECT_EQ( chain.TargetRun.size(), 4U ) << "the twist bone must be inside the run";

    const ModelPose sourceRest = ModelOf( source, retargeter.GetSourceInitialPose() );
    const ModelPose targetRest = ModelOf( target, retargeter.GetTargetInitialPose() );

    // The scenario's own strength, on the record. T6.1's mutation failed to redden because its first
    // scenario made the re-aim identity; a chain whose source ends move by the same amount would make
    // every parameterisation identical and this test vacuous.
    // THE SCENARIO HAD TO BE STRENGTHENED, AND THE FIRST VERSION'S FAILURE IS THE EVIDENCE THAT THE
    // INSTRUMENT IS ALIVE. `IKProbe_Swing` animates the shoulder and the elbow and leaves the HAND on its
    // bind transform -- and an unanimated child inherits its parent's model delta exactly, so the source
    // chain's last two deltas were IDENTICAL. A slerp between two equal quaternions is constant, every
    // bone in the target run came out at 40.1555 deg, and the test below reported "the twist got the end
    // delta" for a reason that had nothing to do with the parameterisation. That is character for
    // character the equivalence trap T6.1 §4 recorded against its own first draft. Turning the source's
    // hand makes the three source deltas genuinely distinct.
    LocalPose sourcePose                             = PoseAt( source, clip, 12000.0 );
    sourcePose[BoneIndex( source, kTip )].Rotation =
         glm::normalize( sourcePose[BoneIndex( source, kTip )].Rotation *
                         glm::angleAxis( glm::radians( 25.0F ), glm::vec3( 0.0F, 0.0F, 1.0F ) ) );
    const ModelPose sourceModel = ModelOf( source, sourcePose );
    const glm::quat startDelta  = sourceModel[chain.SourceRun.front()].Rotation *
                                 glm::inverse( sourceRest[chain.SourceRun.front()].Rotation );
    const glm::quat endDelta = sourceModel[chain.SourceRun.back()].Rotation *
                               glm::inverse( sourceRest[chain.SourceRun.back()].Rotation );
    const float spread = DegreesBetween( startDelta, endDelta );
    std::cout << "[ MEASURED ] source chain: start and end deltas differ by " << spread << " deg\n";
    ASSERT_GT( spread, 5.0F ) << "a chain whose ends agree cannot show a parameterisation";

    LocalPose out;
    ASSERT_TRUE( retargeter.Retarget( sourcePose, out ).IsSuccess() );
    const ModelPose targetModel = ModelOf( target, out );

    std::vector<float> applied;
    for ( size_t i = 0; i < chain.TargetRun.size(); ++i )
    {
        const uint32_t  bone  = chain.TargetRun[i];
        const glm::quat delta = targetModel[bone].Rotation * glm::inverse( targetRest[bone].Rotation );
        applied.push_back( AngleDegrees( delta ) );
        std::cout << "[ MEASURED ]   " << target.GetBones()[bone].Name << " param "
                  << chain.TargetParams[i] << " re-aimed " << applied.back() << " deg\n";
    }

    // EVERY bone in the run moves, and the intermediate one -- which has NO 1:1 partner on the source
    // rig at all -- moves by an amount of its own. That second clause is what Jolt cannot express.
    for ( const float angle : applied )
        EXPECT_GT( angle, 0.5F );

    const uint32_t  twist      = BoneIndex( target, "IK_Twist" );
    const glm::quat twistDelta = targetModel[twist].Rotation * glm::inverse( targetRest[twist].Rotation );
    std::cout << "[ MEASURED ] the twist bone, which has NO source partner, sits "
              << DegreesBetween( twistDelta, startDelta ) << " deg from the chain start's delta and "
              << DegreesBetween( twistDelta, endDelta ) << " deg from the chain end's\n";
    EXPECT_GT( DegreesBetween( twistDelta, startDelta ), 1.0F )
         << "the twist bone got the chain START's delta: the parameterisation is not running";
    EXPECT_GT( DegreesBetween( twistDelta, endDelta ), 1.0F )
         << "the twist bone got the chain END's delta: it is not being placed along the chain";

    // And where a target bone's parameter COINCIDES with a source bone's, it must get that source bone's
    // delta exactly -- the parameterisation has to reduce to the direct mapping at the points where the
    // two chains agree, or it is a different function that merely looks similar.
    const uint32_t  targetElbow = BoneIndex( target, kMid );
    const glm::quat elbowDelta =
         targetModel[targetElbow].Rotation * glm::inverse( targetRest[targetElbow].Rotation );
    const uint32_t  sourceElbow = BoneIndex( source, kMid );
    const glm::quat sourceElbowDelta =
         sourceModel[sourceElbow].Rotation * glm::inverse( sourceRest[sourceElbow].Rotation );
    std::cout << "[ MEASURED ] target elbow (param " << chain.TargetParams[1] << ") vs source elbow (param "
              << chain.SourceParams[1] << "): " << DegreesBetween( elbowDelta, sourceElbowDelta )
              << " deg apart\n";
    EXPECT_LT( DegreesBetween( elbowDelta, sourceElbowDelta ), 1.0e-2F );

    // And the whole point of doing it this way: lengths survive it.
    std::cout << "[ MEASURED ] chained target, worst limb-length error: "
              << WorstSegmentErrorPercent( target, retargeter.GetTargetInitialPose(), out ) << " %\n";
    EXPECT_LT( WorstSegmentErrorPercent( target, retargeter.GetTargetInitialPose(), out ), 1.0e-3F );
}

TEST( RetargetPipeline, AChainOverEquallyLongRunsAgreesWithTheNameMap )
{
    // Declaring a chain where the names already match must not change the answer, or "should I declare a
    // chain here" becomes a question with a silently different answer for each choice.
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( Scaled( bones, 1.4F ) );
    const auto     clip   = ClipFrom( kClipPath );

    Retargeter plain;
    ASSERT_TRUE( plain.Initialize( source, target, SetupFor( kRoot, kRoot ) ).IsSuccess() );

    RetargetSetup chained = SetupFor( kRoot, kRoot );
    chained.Chains.push_back( RetargetChain{ "arm", kRoot, kTip, kRoot, kTip, false } );
    Retargeter withChain;
    ASSERT_TRUE( withChain.Initialize( source, target, chained ).IsSuccess() );

    float     worst = 0.0F;
    LocalPose a;
    LocalPose b;
    for ( const double tick : SampleTicks( clip ) )
    {
        const LocalPose sourcePose = PoseAt( source, clip, tick );
        ASSERT_TRUE( plain.Retarget( sourcePose, a ).IsSuccess() );
        ASSERT_TRUE( withChain.Retarget( sourcePose, b ).IsSuccess() );

        const ModelPose ma = ModelOf( target, a );
        const ModelPose mb = ModelOf( target, b );
        for ( size_t bone = 0; bone < a.Size(); ++bone )
            worst = std::max( worst, glm::length( ma[bone].Translation - mb[bone].Translation ) );
    }
    std::cout << "[ MEASURED ] chain over equal-length runs vs the name map, worst displacement: " << worst
              << " cm\n";
    EXPECT_LT( worst, 1.0e-2F );
}

// ---------------------------------------------------------------------------------------------------
// 7. STAGE 3, THE NORMALISED LIMB EXTENSION -- the half of the equation `SkeletonMapper` has no concept
//    of at all (T6.1 §5: "NO. There is no such concept; it requires an IK solver").
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, TheNormalisedLimbExtensionIsRestoredOnUnevenProportions )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const auto     clip   = ClipFrom( kClipPath );

    // A long upper arm on an ordinary forearm. FK alone reproduces the source's JOINT ANGLES, and equal
    // angles on unequal segments are NOT equal reach -- which is why this stage exists and why a rig
    // scaled uniformly could never show it.
    const Skeleton target = MakeRig( ScaledSegment( bones, kMid, 2.0F ) );

    const auto build = [&]( bool driveWithIK )
    {
        RetargetSetup setup = SetupFor( kRoot, kRoot );
        setup.Chains.push_back( RetargetChain{ "arm", kRoot, kTip, kRoot, kTip, driveWithIK } );
        return setup;
    };

    Retargeter fkOnly;
    ASSERT_TRUE( fkOnly.Initialize( source, target, build( false ) ).IsSuccess() );
    Retargeter withIK;
    const auto ready = withIK.Initialize( source, target, build( true ) );
    ASSERT_TRUE( ready.IsSuccess() ) << ready.GetError();

    const auto&    chain        = withIK.GetChains().front();
    const uint32_t sourceStart  = BoneIndex( source, kRoot );
    const uint32_t  sourceEnd   = BoneIndex( source, kTip );
    const uint32_t  targetStart = BoneIndex( target, kRoot );
    const uint32_t  targetEnd   = BoneIndex( target, kTip );

    std::cout << "[ MEASURED ] rest chain length: source " << chain.SourceRestLength << " cm, target "
              << chain.TargetRestLength << " cm\n";

    float     worstFK = 0.0F;
    float     worstIK = 0.0F;
    float     worstLength = 0.0F;
    LocalPose fk;
    LocalPose ik;
    for ( const double tick : SampleTicks( clip ) )
    {
        const LocalPose sourcePose = PoseAt( source, clip, tick );
        ASSERT_TRUE( fkOnly.Retarget( sourcePose, fk ).IsSuccess() );
        ASSERT_TRUE( withIK.Retarget( sourcePose, ik ).IsSuccess() );

        const ModelPose sourceModel = ModelOf( source, sourcePose );
        const float     sourceExtension =
             glm::length( sourceModel[sourceEnd].Translation - sourceModel[sourceStart].Translation ) /
             chain.SourceRestLength;

        const auto extensionOf = [&]( const LocalPose& pose )
        {
            const ModelPose model = ModelOf( target, pose );
            return glm::length( model[targetEnd].Translation - model[targetStart].Translation ) /
                   chain.TargetRestLength;
        };

        worstFK = std::max( worstFK, std::abs( extensionOf( fk ) - sourceExtension ) / sourceExtension );
        worstIK = std::max( worstIK, std::abs( extensionOf( ik ) - sourceExtension ) / sourceExtension );
        worstLength = std::max(
             worstLength, WorstSegmentErrorPercent( target, withIK.GetTargetInitialPose(), ik ) );
    }

    std::cout << "[ MEASURED ] worst normalised-extension error: FK alone " << ( worstFK * 100.0F )
              << " %, FK+IK " << ( worstIK * 100.0F ) << " %\n";
    std::cout << "[ MEASURED ] worst limb-length error with IK running: " << worstLength << " %\n";

    // The positive control for the stage, stated as a number rather than as a mutation: FK alone has a
    // real error here for the IK stage to remove. If this ever stopped being true the scenario would
    // have gone degenerate and the row below would prove nothing.
    EXPECT_GT( worstFK, 0.01F ) << "the scenario is degenerate: FK alone already has the right extension";
    EXPECT_LT( worstIK, 1.0e-3F );
    EXPECT_LT( worstLength, 1.0e-3F ) << "the solver must not stretch the target's bones to reach";
}

// ---------------------------------------------------------------------------------------------------
// 8. WHAT IS REFUSED. Each of these is a configuration that would otherwise run, report success, and be
//    silently wrong -- which is the one failure mode a test downstream cannot see.
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, ARetargeterThatCouldOnlyEmitTheRestPoseIsRefused )
{
    const auto bones = BonesFrom( kRigPath );

    std::vector<BoneInfo> renamed = bones;
    for ( BoneInfo& b : renamed )
        b.Name = "T_" + b.Name;

    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( renamed );

    Retargeter retargeter;
    const auto ready = retargeter.Initialize( source, target, SetupFor( kRoot, "T_IK_Shoulder" ) );
    ASSERT_FALSE( ready.IsSuccess() );
    std::cout << "[ REFUSED ] " << ready.GetError() << "\n";
    EXPECT_NE( ready.GetError().find( "rest pose" ), std::string::npos );

    // One authored rename is enough to make it a working retargeter -- the refusal is about "nothing is
    // mapped", not about names being different.
    RetargetSetup setup = SetupFor( kRoot, "T_IK_Shoulder" );
    setup.BoneRenames["T_IK_Elbow"] = kMid;
    Retargeter fixed;
    const auto second = fixed.Initialize( source, target, setup );
    EXPECT_TRUE( second.IsSuccess() ) << second.GetError();
}

TEST( RetargetPipeline, ARenameOntoAMissingSourceBoneIsRefused )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( bones );

    RetargetSetup setup      = SetupFor( kRoot, kRoot );
    setup.BoneRenames[kMid]  = "IK_Elbwo";

    Retargeter retargeter;
    const auto ready = retargeter.Initialize( source, target, setup );
    ASSERT_FALSE( ready.IsSuccess() );
    std::cout << "[ REFUSED ] " << ready.GetError() << "\n";
}

TEST( RetargetPipeline, AnIKChainThatIsNotTwoBonesIsRefused )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( RigWithTwist( bones ) );

    RetargetSetup setup = SetupFor( kRoot, kRoot );
    setup.Chains.push_back( RetargetChain{ "arm", kRoot, kTip, kRoot, kTip, true } );

    Retargeter retargeter;
    const auto ready = retargeter.Initialize( source, target, setup );
    ASSERT_FALSE( ready.IsSuccess() );
    std::cout << "[ REFUSED ] " << ready.GetError() << "\n";
    EXPECT_NE( ready.GetError().find( "TWO-BONE" ), std::string::npos );
}

TEST( RetargetPipeline, AChainWhoseEndIsNotADescendantOfItsStartIsRefused )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( bones );

    RetargetSetup setup = SetupFor( kRoot, kRoot );
    setup.Chains.push_back( RetargetChain{ "nonsense", kRoot, kTip, "IK_Post", kTip, false } );

    Retargeter retargeter;
    const auto ready = retargeter.Initialize( source, target, setup );
    ASSERT_FALSE( ready.IsSuccess() );
    std::cout << "[ REFUSED ] " << ready.GetError() << "\n";
    EXPECT_NE( ready.GetError().find( "descendant" ), std::string::npos );
}

TEST( RetargetPipeline, TwoChainsClaimingTheSameTargetBoneAreRefused )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( bones );

    RetargetSetup setup = SetupFor( kRoot, kRoot );
    setup.Chains.push_back( RetargetChain{ "arm", kRoot, kTip, kRoot, kTip, false } );
    setup.Chains.push_back( RetargetChain{ "forearm", kMid, kTip, kMid, kTip, false } );

    Retargeter retargeter;
    const auto ready = retargeter.Initialize( source, target, setup );
    ASSERT_FALSE( ready.IsSuccess() );
    std::cout << "[ REFUSED ] " << ready.GetError() << "\n";
}

TEST( RetargetPipeline, ASourcePoseOfTheWrongSizeIsRefused )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( bones );

    Retargeter retargeter;
    ASSERT_TRUE( retargeter.Initialize( source, target, SetupFor( kRoot, kRoot ) ).IsSuccess() );

    LocalPose  tooShort( 2 );
    LocalPose  out;
    const auto ran = retargeter.Retarget( tooShort, out );
    ASSERT_FALSE( ran.IsSuccess() );
    std::cout << "[ REFUSED ] " << ran.GetError() << "\n";
}

TEST( RetargetPipeline, AnUninitialisedRetargeterRefuses )
{
    const Skeleton rig = MakeRig( BonesFrom( kRigPath ) );

    Retargeter retargeter;
    LocalPose  out;
    const auto ran = retargeter.Retarget( BindPose( rig ), out );
    ASSERT_FALSE( ran.IsSuccess() );
    std::cout << "[ REFUSED ] " << ran.GetError() << "\n";
}

// ---------------------------------------------------------------------------------------------------
// 9. WHAT IS DELIBERATELY NOT CARRIED ACROSS. A source clip's scale keys describe the SOURCE rig's
//    proportions; carrying them onto a differently proportioned target is the defect this task removes.
// ---------------------------------------------------------------------------------------------------

TEST( RetargetPipeline, SourceScaleIsNotCarriedOntoTheTarget )
{
    const auto     bones  = BonesFrom( kRigPath );
    const Skeleton source = MakeRig( bones );
    const Skeleton target = MakeRig( Scaled( bones, 1.5F ) );

    Retargeter retargeter;
    ASSERT_TRUE( retargeter.Initialize( source, target, SetupFor( kRoot, kRoot ) ).IsSuccess() );

    LocalPose sourcePose                       = BindPose( source );
    sourcePose[BoneIndex( source, kMid )].Scale = glm::vec3( 2.0F );

    LocalPose out;
    ASSERT_TRUE( retargeter.Retarget( sourcePose, out ).IsSuccess() );

    const glm::vec3 elbowScale = out[BoneIndex( target, kMid )].Scale;
    std::cout << "[ MEASURED ] source elbow scale 2.0 -> target elbow scale (" << elbowScale.x << ", "
              << elbowScale.y << ", " << elbowScale.z << ")\n";
    EXPECT_NEAR( elbowScale.x, 1.0F, 1.0e-4F );
    EXPECT_NEAR( elbowScale.y, 1.0F, 1.0e-4F );
    EXPECT_NEAR( elbowScale.z, 1.0F, 1.0e-4F );

    EXPECT_LT( WorstSegmentErrorPercent( target, retargeter.GetTargetInitialPose(), out ), 1.0e-3F );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
