// T6.1 — WHAT `JPH::SkeletonMapper` ACTUALLY DOES TO OUR RIG AND OUR CLIP.
//
// Docs/Animation/06_gap_analysis.md §3.19 records that we vendor, build and never call a skeleton
// retarget mapper, and Tier 6 makes "evaluate it before writing our own chain-mapping layer" a decision
// rather than a suggestion. Reading its header is not an evaluation: the header says "map a low detail
// (ragdoll) skeleton to a high detail (animation) skeleton", which is a claim about a use we do not have,
// and the only way to learn what it does to the use we DO have is to drive it with our own data.
//
// So this suite is the conduct, not a description of one. It reads the shipped `IKProbe.skeleton` and
// `IKProbe_Swing.anim` off disk through the engine's own reader, builds a JPH::Skeleton and a neutral pose
// from them, and runs Initialize()/Map() for real. Every number below came out of that run.
//
// WHAT IT MEASURES, and why each is the question a retargeter lives or dies on:
//
//   1. Conduct at all — does our rig survive the round trip? (positive control: map a rig onto ITSELF and
//      the model-space pose must come back bit-comparable.)
//   2. Does a clip reach the other side — the negative-control's partner: a mapper that returns the bind
//      pose would pass (1) and be useless.
//   3. THE DECIDING ONE: map onto a rig with DIFFERENT PROPORTIONS. Retargeting exists for exactly this.
//      A retargeter must preserve the TARGET's bone lengths. Measured as a RATIO of lengths, never in
//      absolute centimetres, so the assertion means the same thing on any rig and under any sanitizer.
//   4. Pelvis/root height — the item T6.2 names as "pelvis height scaling". Measured, not argued.
//   5. Scale. Our BoneTransform carries one; Jolt's JointState does not.
//   6. The chain path, which is the one piece of §3.19's promise that is real, driven with an extra
//      intermediate joint (the Mixamo twist-bone case).
//   7. Two PRECONDITIONS that are JPH_ASSERT and therefore silent in Release: parents-before-children by
//      ARRAY INDEX, which our Skeleton deliberately does not promise, and n1 <= n2, which the real
//      retarget direction (a 65-bone purchased rig onto our 5-bone probe) violates.
//
// JOLT'S ALLOCATOR IS A GLOBAL FUNCTION POINTER AND IT IS NULL UNTIL SOMEBODY REGISTERS IT.
// `JPH::Array` is `std::vector<T, STLAllocator<T>>` and STLAllocator::allocate calls `JPH::Allocate`,
// which `Memory.cpp` initialises to `nullptr`. Today the only caller of `RegisterDefaultAllocator()` in
// this tree is `PhysicsWorld::Init`. Using SkeletonMapper from the animation pipeline therefore means the
// animation pipeline's first allocation is a null call unless physics has started first — that is an
// integration cost, and main() below pays it explicitly so that the cost is visible rather than inherited.

#include <gtest/gtest.h>

#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Pose.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonMapper.h>

#include <glm/gtc/matrix_transform.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Desert::Animation::BoneInfo;
    using Desert::Animation::ComponentPose;
    using Desert::Animation::FrameTime;
    using Desert::Animation::LocalPose;
    using Desert::Animation::Skeleton;

    constexpr const char* kRigPath  = "Editor/Cooked/Meshes/IKProbe.skeleton";
    constexpr const char* kClipPath = "Editor/Cooked/Meshes/IKProbe_Swing.anim";

    // The probe limb. IK_Shoulder -> IK_Elbow -> IK_Hand is the only three-bone chain in the corpus, and
    // a limb is the thing a retargeter is judged on.
    constexpr const char* kRoot = "IK_Shoulder";
    constexpr const char* kMid  = "IK_Elbow";
    constexpr const char* kTip  = "IK_Hand";

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            std::ifstream probe( prefix + kRigPath );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        std::ifstream in( path, std::ios::binary );
        if ( !in )
            return {};
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::vector<BoneInfo> ProbeBones()
    {
        const std::string raw = ReadFile( RepoRoot() + kRigPath );
        EXPECT_FALSE( raw.empty() ) << "could not read " << kRigPath;
        auto data =
             rfl::json::read<Desert::Assets::Serialization::SkeletonAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() );
        return data.has_value() ? data.value().Bones : std::vector<BoneInfo>{};
    }

    Desert::Animation::AnimationClip ProbeClip()
    {
        const std::string raw = ReadFile( RepoRoot() + kClipPath );
        EXPECT_FALSE( raw.empty() ) << "could not read " << kClipPath;
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

    // A rig with the same bone NAMES and the same parent links but every bone offset scaled by `k`:
    // a taller (or shorter) character built from the same skeleton template. This is the purchased-clip
    // case reduced to its one essential difference, with every other variable held fixed -- including the
    // signature, which Skeleton::ComputeSignature derives from names and parents only and therefore does
    // NOT change here. That is worth stating: our clip<->rig binding already treats these two rigs as the
    // same rig, so nothing in the engine stops the clip being played on the taller one today.
    Skeleton ScaledRig( const std::vector<BoneInfo>& source, float k )
    {
        std::vector<BoneInfo> bones = source;
        for ( BoneInfo& b : bones )
        {
            b.LocalBindTransform[3][0] *= k;
            b.LocalBindTransform[3][1] *= k;
            b.LocalBindTransform[3][2] *= k;
        }
        Skeleton rig( std::move( bones ) );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    JPH::Mat44 ToJolt( const glm::mat4& m )
    {
        return JPH::Mat44(
             JPH::Vec4( m[0][0], m[0][1], m[0][2], m[0][3] ), JPH::Vec4( m[1][0], m[1][1], m[1][2], m[1][3] ),
             JPH::Vec4( m[2][0], m[2][1], m[2][2], m[2][3] ), JPH::Vec4( m[3][0], m[3][1], m[3][2], m[3][3] ) );
    }

    glm::vec3 TranslationOf( const JPH::Mat44& m )
    {
        const JPH::Vec3 t = m.GetTranslation();
        return { t.GetX(), t.GetY(), t.GetZ() };
    }

    // Our Skeleton -> JPH::Skeleton, in OUR bone array order. The order is carried over unchanged on
    // purpose: the whole point of measurement 8a is what Jolt does with an order our type permits.
    //
    // THE PARENT-NAME OVERLOAD, NOT THE PARENT-INDEX ONE, AND THIS COST A CRASH TO FIND.
    // `AddJoint(name, int parentIndex)` is written `mJoints[inParentIndex].mName` -- it reads the
    // parent's name out of the array position the parent is ASSUMED to already occupy. On a child-first
    // rig, which our Skeleton explicitly permits, that index has not been filled yet and the read runs
    // off the end of a vector with no bounds check and no assert. The suite died silently on exactly
    // that before this was changed. The name overload plus CalculateParentJointIndices() is
    // order-independent and is the only safe conversion from our bone array.
    void FillJoltSkeleton( const Skeleton& rig, JPH::Skeleton& out )
    {
        const auto& bones = rig.GetBones();
        for ( const BoneInfo& b : bones )
            out.AddJoint( b.Name,
                          b.IsRoot() ? JPH::string_view() : JPH::string_view( bones[b.GetParentID()].Name ) );
        out.CalculateParentJointIndices();
    }

    std::vector<glm::mat4> ModelSpace( const Skeleton& rig, const LocalPose& local )
    {
        ComponentPose          pose( rig, local );
        std::vector<glm::mat4> out( rig.GetBones().size(), glm::mat4( 1.0F ) );
        pose.ConvertAll();
        for ( size_t i = 0; i < out.size(); ++i )
            out[i] = pose.Get( static_cast<uint32_t>( i ) );
        return out;
    }

    std::vector<JPH::Mat44> ToJoltArray( const std::vector<glm::mat4>& in )
    {
        std::vector<JPH::Mat44> out;
        out.reserve( in.size() );
        for ( const glm::mat4& m : in )
            out.push_back( ToJolt( m ) );
        return out;
    }

    LocalPose BindPose( const Skeleton& rig )
    {
        auto bind = LocalPose::FromBindPose( rig );
        EXPECT_TRUE( bind.IsSuccess() ) << ( bind.IsSuccess() ? "" : bind.GetError() );
        return bind.IsSuccess() ? bind.ExtractValue() : LocalPose( rig.GetBones().size() );
    }

    // The clip's pose at `t`, on top of the rig's bind pose -- exactly what Animator does, minus the
    // layers and the crossfade neither of which this measurement needs.
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

    float Distance( const glm::vec3& a, const glm::vec3& b )
    {
        return glm::length( a - b );
    }

    // Eleven samples across the clip, endpoints included. Enough to catch a property that only holds at
    // rest -- which is exactly the property measurement 3 turns out to find.
    std::vector<double> SampleTicks( const Desert::Animation::AnimationClip& clip )
    {
        std::vector<double> out;
        const double        duration = static_cast<double>( clip.DurationTicks.Value );
        for ( int i = 0; i <= 10; ++i )
            out.push_back( duration * i / 10.0 );
        return out;
    }
} // namespace

// ---------------------------------------------------------------------------------------------------
// 1. THE CONDUCT ITSELF, and it is the positive control.
//
// A rig mapped onto ITSELF must come back unchanged. If this fails, nothing below means anything: every
// later number would be measuring a broken conversion rather than the mapper.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, OurRigMapsOntoItselfUnchanged )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";

    const auto bones = ProbeBones();
    ASSERT_EQ( bones.size(), 5u ) << "IKProbe.skeleton is the five-bone probe this suite was written for";

    const Skeleton rig{ std::vector<BoneInfo>( bones ) };
    ASSERT_TRUE( rig.GetStructureError().empty() ) << rig.GetStructureError();

    JPH::Skeleton joltRig;
    FillJoltSkeleton( rig, joltRig );
    ASSERT_EQ( joltRig.GetJointCount(), 5 );

    const auto bind      = BindPose( rig );
    const auto bindModel = ModelSpace( rig, bind );
    const auto bindJolt  = ToJoltArray( bindModel );

    // Local-space matrices for skeleton 2, which Map() needs for the joints it cannot map directly.
    std::vector<JPH::Mat44> localJolt;
    for ( size_t i = 0; i < bind.Size(); ++i )
        localJolt.push_back( ToJolt( bind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltRig, bindJolt.data(), &joltRig, bindJolt.data() );

    EXPECT_EQ( mapper.GetMappings().size(), 5u ) << "every bone name matches itself; all five map 1:1";
    EXPECT_EQ( mapper.GetChains().size(), 0u ) << "identical rigs leave no unmappable run between mapped joints";
    EXPECT_EQ( mapper.GetUnmapped().size(), 0u );

    std::vector<JPH::Mat44> out( 5, JPH::Mat44::sIdentity() );
    mapper.Map( bindJolt.data(), localJolt.data(), out.data() );

    for ( int i = 0; i < 5; ++i )
    {
        const glm::vec3 got  = TranslationOf( out[i] );
        const glm::vec3 want = glm::vec3( bindModel[i][3] );
        EXPECT_LT( Distance( got, want ), 1e-3F )
             << "bone " << bones[i].Name << " moved under an identity mapping: got (" << got.x << ", " << got.y
             << ", " << got.z << "), expected (" << want.x << ", " << want.y << ", " << want.z << ")";
    }
}

// ---------------------------------------------------------------------------------------------------
// 2. A CLIP REACHES THE OTHER SIDE.
//
// The partner of measurement 1: a mapper that silently returned the neutral pose would pass that test and
// be worthless. Asserted as travel far above rounding, in the units the corpus is authored in.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, OurClipDrivesTheMappedRig )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones = ProbeBones();
    const Skeleton rig{ std::vector<BoneInfo>( bones ) };
    const auto     clip = ProbeClip();
    ASSERT_FALSE( clip.Tracks.empty() ) << "the probe clip carries no tracks";
    ASSERT_EQ( clip.SkeletonSignature, rig.GetSignature() )
         << "the clip does not claim this rig; every number below would be a picture of a bind pose";

    JPH::Skeleton joltRig;
    FillJoltSkeleton( rig, joltRig );

    const auto bind      = BindPose( rig );
    const auto bindModel = ModelSpace( rig, bind );
    const auto bindJolt  = ToJoltArray( bindModel );

    std::vector<JPH::Mat44> localJolt;
    for ( size_t i = 0; i < bind.Size(); ++i )
        localJolt.push_back( ToJolt( bind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltRig, bindJolt.data(), &joltRig, bindJolt.data() );

    const auto tipIdx = rig.FindBoneIndex( kTip );
    ASSERT_TRUE( tipIdx.has_value() );

    float maxTravel = 0.0F;
    for ( const double t : SampleTicks( clip ) )
    {
        const auto local = PoseAt( rig, clip, t );
        const auto model = ModelSpace( rig, local );
        const auto src   = ToJoltArray( model );

        std::vector<JPH::Mat44> out( src.size(), JPH::Mat44::sIdentity() );
        mapper.Map( src.data(), localJolt.data(), out.data() );

        maxTravel =
             std::max( maxTravel, Distance( TranslationOf( out[*tipIdx] ), glm::vec3( bindModel[*tipIdx][3] ) ) );
    }

    // The corpus suite pins this clip as one that MOVES; 10 cm is far above the 1e-3 cm floor measurement
    // 1 uses and far below what the clip actually travels.
    EXPECT_GT( maxTravel, 10.0F ) << "the mapped tip barely moved (" << maxTravel
                                  << " cm): the clip is not reaching the far side of the mapper";
}

// ---------------------------------------------------------------------------------------------------
// 3. THE DECIDING MEASUREMENT: A TARGET WITH DIFFERENT PROPORTIONS.
//
// This is what retargeting IS, and the first draft of this test asserted the wrong thing. It predicted
// that Map() would destroy the target's bone lengths, because Map() writes every mapped joint
// INDEPENDENTLY in model space and never consults a bone length. The run said otherwise: 0.39 % worst
// error on a 1.5x-taller rig. The algebra explains it, and the explanation is the deliverable:
//
//   Map() writes outPose2[b] = pose1[a] * (neutral1[a]^-1 * neutral2[b]) = D[a] * neutral2[b],
//   where D[a] = pose1[a] * neutral1[a]^-1 is the source joint's model-space delta from ITS rest pose.
//   For two joints i, j: if their delta ROTATIONS are equal and the source's own segment is rigid, the
//   whole expression collapses to |dR * (neutral2[j].t - neutral2[i].t)| -- the TARGET's rest length,
//   exactly. The error is therefore proportional to the product of two things: how much the two rest
//   poses differ at that joint, and how much the joint BENDS.
//
// So the honest instrument is not one number but the law: sweep the proportion factor k and watch the
// error grow with it. Asserted as RATIOS (mapped length / the target's own rest length) throughout --
// the property is scale-free and a centimetre threshold would be a different assertion on every rig.
// ---------------------------------------------------------------------------------------------------
namespace
{
    // Worst |ratio - 1| of a mapped limb segment to the target's own rest length, over the whole clip.
    // `worstAtRest` comes back separately because "correct at rest, wrong under animation" is the shape
    // of the defect and a single worst-case number would hide it.
    struct LimbError
    {
        float Worst       = 0.0F;
        float WorstAtRest = 0.0F;
    };

    LimbError MeasureLimbError( const Skeleton& source, const Skeleton& target,
                                const Desert::Animation::AnimationClip& clip )
    {
        JPH::Skeleton joltSource;
        JPH::Skeleton joltTarget;
        FillJoltSkeleton( source, joltSource );
        FillJoltSkeleton( target, joltTarget );

        const auto srcBindJolt  = ToJoltArray( ModelSpace( source, BindPose( source ) ) );
        const auto tgtBind      = BindPose( target );
        const auto tgtBindModel = ModelSpace( target, tgtBind );
        const auto tgtBindJolt  = ToJoltArray( tgtBindModel );

        std::vector<JPH::Mat44> tgtLocalJolt;
        for ( size_t i = 0; i < tgtBind.Size(); ++i )
            tgtLocalJolt.push_back( ToJolt( tgtBind[i].ToMatrix() ) );

        JPH::SkeletonMapper mapper;
        mapper.Initialize( &joltSource, srcBindJolt.data(), &joltTarget, tgtBindJolt.data() );

        const auto rootIdx = target.FindBoneIndex( kRoot );
        const auto midIdx  = target.FindBoneIndex( kMid );
        const auto tipIdx  = target.FindBoneIndex( kTip );
        EXPECT_TRUE( rootIdx && midIdx && tipIdx );
        if ( !( rootIdx && midIdx && tipIdx ) )
            return {};

        const float upperRest =
             Distance( glm::vec3( tgtBindModel[*rootIdx][3] ), glm::vec3( tgtBindModel[*midIdx][3] ) );
        const float lowerRest =
             Distance( glm::vec3( tgtBindModel[*midIdx][3] ), glm::vec3( tgtBindModel[*tipIdx][3] ) );

        LimbError out;
        bool      first = true;
        for ( const double t : SampleTicks( clip ) )
        {
            const auto src = ToJoltArray( ModelSpace( source, PoseAt( source, clip, t ) ) );

            std::vector<JPH::Mat44> mapped( target.GetBones().size(), JPH::Mat44::sIdentity() );
            mapper.Map( src.data(), tgtLocalJolt.data(), mapped.data() );

            const float upper =
                 Distance( TranslationOf( mapped[*rootIdx] ), TranslationOf( mapped[*midIdx] ) ) / upperRest;
            const float lower =
                 Distance( TranslationOf( mapped[*midIdx] ), TranslationOf( mapped[*tipIdx] ) ) / lowerRest;
            const float worst = std::max( std::fabs( upper - 1.0F ), std::fabs( lower - 1.0F ) );
            out.Worst         = std::max( out.Worst, worst );
            if ( first )
            {
                out.WorstAtRest = worst;
                first           = false;
            }
        }
        return out;
    }

    // The target rig with `bone`'s rest transform pre-rotated about X. This is the A-pose-versus-T-pose
    // difference in its smallest honest form: the two rigs share names and proportions and disagree only
    // about what "rest" looks like, which is the difference a retarget POSE exists to absorb.
    Skeleton RestRotatedRig( const std::vector<BoneInfo>& source, const std::string& bone, float radians )
    {
        std::vector<BoneInfo> bones = source;
        for ( BoneInfo& b : bones )
            if ( b.Name == bone )
                b.LocalBindTransform =
                     b.LocalBindTransform * glm::rotate( glm::mat4( 1.0F ), radians, glm::vec3( 1, 0, 0 ) );
        Skeleton rig( std::move( bones ) );
        rig.RecomputeOffsetMatrices();
        return rig;
    }
} // namespace

TEST( SkeletonMapperFit, LimbLengthErrorGrowsWithTheRestPoseDifference )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones = ProbeBones();
    const Skeleton source{ std::vector<BoneInfo>( bones ) };
    const auto     clip = ProbeClip();
    ASSERT_FALSE( clip.Tracks.empty() );

    // The two rigs are the SAME rig as far as this engine is concerned, at every k: ComputeSignature is
    // over names and parents. Nothing in the clip<->rig binding stops the taller one playing this clip.
    EXPECT_EQ( source.GetSignature(), ScaledRig( bones, 3.0F ).GetSignature() );

    // NOT MONOTONE, AND THE SECOND DRAFT OF THIS TEST ASSERTED THAT IT WAS. Measured: k=1.25 gives more
    // error than k=1.5. The formula says why and the shape is worth keeping rather than smoothing over --
    // the mapped segment is |P1[j].t - P1[i].t + (k-1)(dR_j p_j - dR_i p_i)| over k|p_j - p_i|, a norm of
    // a SUM, so it can dip on the way to its large-k asymptote. A retarget error that is not monotone in
    // the proportion difference is a retarget error you cannot bound by testing one pair of rigs.
    float atSmallest = -1.0F;
    float atLargest  = -1.0F;
    for ( const float k : { 1.0F, 1.25F, 1.5F, 2.0F, 3.0F } )
    {
        const Skeleton  target = ScaledRig( bones, k );
        const LimbError err    = MeasureLimbError( source, target, clip );

        std::cout << "[ MEASURED ] proportion factor k=" << k << "  worst limb-length error " << err.Worst * 100.0F
                  << " %  (at rest " << err.WorstAtRest * 100.0F << " %)\n";

        // CORRECT AT REST, AT EVERY k. This is the half that makes the defect dangerous: a bind-pose
        // screenshot of a retarget built on this mapper is perfect no matter how badly proportioned the
        // two rigs are.
        EXPECT_LT( err.WorstAtRest, 1e-4F ) << "k=" << k << ": the rest pose itself is already wrong";

        if ( k == 1.0F )
        {
            EXPECT_LT( err.Worst, 1e-4F )
                 << "identical rigs: the mapper must be exact, otherwise every number above is noise";
            continue;
        }

        EXPECT_GT( err.Worst, 1e-3F )
             << "k=" << k
             << ": a rig with different proportions kept its limb lengths to within 0.1 %. If this ever "
                "becomes true, Map() has acquired a notion of the target's bone lengths and T6.2's "
                "normalised-limb-extension item would have to be revisited.";

        if ( atSmallest < 0.0F )
            atSmallest = err.Worst;
        atLargest = err.Worst;
    }

    // The one growth statement the data supports: the worst proportion difference measured is the worst
    // error measured. Not "it grows at every step" -- see the note above.
    EXPECT_GT( atLargest, atSmallest )
         << "a 3x rig did not err more than a 1.25x rig; the rest-pose difference is not what drives this";
}

// THE OTHER AXIS, AND HERE THE MAPPER WINS OUTRIGHT. Same proportions, different rest ORIENTATION: a
// purchased clip authored on an A-pose driving a character modelled in a T-pose. This was written
// expecting a second failure and measured 3e-7 % -- EXACT.
//
// The algebra again: outPose2[j] = D[j] * neutral2[j] carries the TARGET's own rest transform, so a rest
// pose that differs only in orientation cancels identically. That is the FK-delta semantics UE's
// retargeter implements with an explicitly authored retarget pose, and Jolt gets it for free out of the
// model-space delta formulation. It is the single strongest argument in this document for reusing the
// idea even if the class is not reused.
TEST( SkeletonMapperFit, ADifferentRestOrientationIsAbsorbedExactly )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones = ProbeBones();
    const Skeleton source{ std::vector<BoneInfo>( bones ) };
    const auto     clip = ProbeClip();

    // 45 degrees at the elbow: the whole forearm sits somewhere else at rest, nothing else changes.
    const Skeleton  target = RestRotatedRig( bones, kMid, glm::radians( 45.0F ) );
    const LimbError err    = MeasureLimbError( source, target, clip );

    std::cout << "[ MEASURED ] 45 deg rest-orientation difference at the elbow: worst limb-length error "
              << err.Worst * 100.0F << " %  (at rest " << err.WorstAtRest * 100.0F << " %)\n";

    EXPECT_LT( err.WorstAtRest, 1e-4F ) << "the rest pose is exact here too";
    EXPECT_LT( err.Worst, 1e-4F )
         << "a rest-orientation difference produced limb-length error. The whole reason to reuse this "
            "mapper is that the model-space delta absorbs it exactly; if that stops being true there is "
            "nothing left to reuse.";

    // The negative control this needs: the same measurement with a rest-POSITION difference is NOT zero
    // (see the k sweep above). Without it "absorbed exactly" would be indistinguishable from an
    // instrument that cannot see anything.
    const LimbError proportions = MeasureLimbError( source, ScaledRig( bones, 2.0F ), clip );
    EXPECT_GT( proportions.Worst, 1e-3F )
         << "the instrument reports zero for a proportion difference too, so it is measuring nothing";

    // AND THE SECOND NEGATIVE CONTROL, which a length ratio cannot give: a rotation about the elbow does
    // not change |hand - elbow|, so the measurement above would read zero even if the mapper ignored the
    // target's rest pose entirely and copied the source's joint positions. So assert where the hand
    // actually LANDS: on the target's rest offset carried by the source's delta, which is a different
    // place from the source's own hand.
    JPH::Skeleton joltSource;
    JPH::Skeleton joltTarget;
    FillJoltSkeleton( source, joltSource );
    FillJoltSkeleton( target, joltTarget );

    const auto srcBindModel = ModelSpace( source, BindPose( source ) );
    const auto tgtBind      = BindPose( target );
    const auto tgtBindModel = ModelSpace( target, tgtBind );

    std::vector<JPH::Mat44> tgtLocalJolt;
    for ( size_t i = 0; i < tgtBind.Size(); ++i )
        tgtLocalJolt.push_back( ToJolt( tgtBind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltSource, ToJoltArray( srcBindModel ).data(), &joltTarget,
                       ToJoltArray( tgtBindModel ).data() );

    const auto tipIdx = target.FindBoneIndex( kTip );
    ASSERT_TRUE( tipIdx.has_value() );

    const auto srcModel = ModelSpace( source, PoseAt( source, clip, clip.DurationTicks.Value * 0.3 ) );

    std::vector<JPH::Mat44> out( target.GetBones().size(), JPH::Mat44::sIdentity() );
    mapper.Map( ToJoltArray( srcModel ).data(), tgtLocalJolt.data(), out.data() );

    const glm::vec3 mappedTip = TranslationOf( out[*tipIdx] );
    const glm::vec3 sourceTip = glm::vec3( srcModel[*tipIdx][3] );
    const glm::vec3 wanted =
         glm::vec3( ( srcModel[*tipIdx] * glm::inverse( srcBindModel[*tipIdx] ) * tgtBindModel[*tipIdx] )[3] );

    EXPECT_LT( Distance( mappedTip, wanted ), 1e-2F )
         << "the mapped tip is not D[j] * neutral2[j]; the formula this whole document rests on is wrong";
    EXPECT_GT( Distance( mappedTip, sourceTip ), 10.0F )
         << "the mapped tip landed on the SOURCE's tip, so the target's rest pose was ignored and the "
            "length ratio above was blind to it";
}

// ---------------------------------------------------------------------------------------------------
// 4. PELVIS HEIGHT SCALING — T6.2 names it; Map() has no notion of it, and this measures the size of
//    the omission rather than asserting it from the source.
//
// IKProbe_Swing cannot answer this: its two tracks carry CONSTANT position keys, so the probe's root
// never translates and any root-height number taken from it would be a statement about the clip. The
// corpus's root-translating clip is TwoBoneProbe_Wave (Base rises 100 -> 150 cm), so the measurement is
// taken on the rig that clip was authored for. Using the other probe is the point, not a shortcut.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, ARootTranslationIsCopiedUnscaledOntoATallerRig )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const std::string raw = ReadFile( RepoRoot() + "Editor/Cooked/Meshes/TwoBoneProbe.skeleton" );
    ASSERT_FALSE( raw.empty() );
    auto data = rfl::json::read<Desert::Assets::Serialization::SkeletonAssetData, rfl::DefaultIfMissing>( raw );
    ASSERT_TRUE( data.has_value() );
    const std::vector<BoneInfo> twoBones = data.value().Bones;

    const Skeleton source{ std::vector<BoneInfo>( twoBones ) };
    const Skeleton target = ScaledRig( twoBones, 1.5F );

    const std::string clipRaw = ReadFile( RepoRoot() + "Editor/Cooked/Meshes/TwoBoneProbe_Wave.anim" );
    ASSERT_FALSE( clipRaw.empty() );
    const auto clipData =
         rfl::json::read<Desert::Assets::Serialization::AnimationAssetData, rfl::DefaultIfMissing>( clipRaw );
    ASSERT_TRUE( clipData.has_value() );
    auto built = Desert::Assets::Serialization::BuildClipFromAssetData( clipData.value() );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();
    const auto clip = built.ExtractValue();
    ASSERT_EQ( clip.SkeletonSignature, source.GetSignature() );

    JPH::Skeleton joltSource;
    JPH::Skeleton joltTarget;
    FillJoltSkeleton( source, joltSource );
    FillJoltSkeleton( target, joltTarget );

    const auto srcBind      = BindPose( source );
    const auto srcBindModel = ModelSpace( source, srcBind );
    const auto tgtBind      = BindPose( target );
    const auto tgtBindModel = ModelSpace( target, tgtBind );

    std::vector<JPH::Mat44> tgtLocalJolt;
    for ( size_t i = 0; i < tgtBind.Size(); ++i )
        tgtLocalJolt.push_back( ToJolt( tgtBind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltSource, ToJoltArray( srcBindModel ).data(), &joltTarget,
                       ToJoltArray( tgtBindModel ).data() );

    const auto rootIdx = source.FindBoneIndex( "Base" );
    ASSERT_TRUE( rootIdx.has_value() );

    const float srcRestY = srcBindModel[*rootIdx][3][1];
    const float tgtRestY = tgtBindModel[*rootIdx][3][1];
    ASSERT_NEAR( tgtRestY / srcRestY, 1.5F, 1e-3F ) << "the taller rig is not 1.5x taller at the root";

    float bestSourceLift = 0.0F;
    float liftAtBest     = 0.0F;
    for ( const double t : SampleTicks( clip ) )
    {
        const auto model = ModelSpace( source, PoseAt( source, clip, t ) );
        const auto src   = ToJoltArray( model );

        std::vector<JPH::Mat44> mapped( target.GetBones().size(), JPH::Mat44::sIdentity() );
        mapper.Map( src.data(), tgtLocalJolt.data(), mapped.data() );

        const float sourceLift = model[*rootIdx][3][1] - srcRestY;
        if ( std::fabs( sourceLift ) > std::fabs( bestSourceLift ) )
        {
            bestSourceLift = sourceLift;
            liftAtBest     = TranslationOf( mapped[*rootIdx] ).y - tgtRestY;
        }
    }

    ASSERT_GT( std::fabs( bestSourceLift ), 10.0F ) << "TwoBoneProbe_Wave no longer lifts its root";

    const float liftRatio = liftAtBest / bestSourceLift;
    std::cout << "[ MEASURED ] source root lift " << bestSourceLift << " cm; the 1.5x rig's mapped root "
              << "lifted " << liftAtBest << " cm; ratio " << liftRatio
              << " (a retargeter that scales pelvis height would give 1.5)\n";

    // 1.0, not 1.5: the translation is carried across as-is. On a character that is half again as tall,
    // a step that clears a 50 cm ledge on the source clears the same 50 cm -- which on the taller rig is
    // a shorter step relative to its own legs. That is what "pelvis height scaling" in T6.2 buys, and
    // nothing in Map() does it.
    EXPECT_NEAR( liftRatio, 1.0F, 0.05F )
         << "the root lift was scaled after all; Map() has acquired a height ratio and T6.2's first item "
            "would have to be revisited";
}

// ---------------------------------------------------------------------------------------------------
// 5. SCALE.
//
// Our BoneTransform carries a Scale and Pose.hpp gives the reason (clips store S keys, blends blend
// them). JPH::SkeletonPose::JointState is a quat and a translation and has nowhere to put one. Map()
// itself is Mat44-typed, so a scale DOES ride through a direct mapping -- and that is the trap worth
// pinning, because it makes the loss invisible on a rig with no chains: Map()'s chain branch calls
// Mat44::SetRotation, which overwrites the 3x3 that was carrying the scale.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, ScaleSurvivesADirectMapping )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones = ProbeBones();
    const Skeleton rig{ std::vector<BoneInfo>( bones ) };

    JPH::Skeleton joltRig;
    FillJoltSkeleton( rig, joltRig );

    const auto bind      = BindPose( rig );
    const auto bindModel = ModelSpace( rig, bind );
    const auto bindJolt  = ToJoltArray( bindModel );

    std::vector<JPH::Mat44> localJolt;
    for ( size_t i = 0; i < bind.Size(); ++i )
        localJolt.push_back( ToJolt( bind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltRig, bindJolt.data(), &joltRig, bindJolt.data() );

    const auto midIdx = rig.FindBoneIndex( kMid );
    ASSERT_TRUE( midIdx.has_value() );

    LocalPose scaled      = bind;
    scaled[*midIdx].Scale = glm::vec3( 2.0F );
    const auto scaledJolt = ToJoltArray( ModelSpace( rig, scaled ) );

    std::vector<JPH::Mat44> out( bindJolt.size(), JPH::Mat44::sIdentity() );
    mapper.Map( scaledJolt.data(), localJolt.data(), out.data() );

    const JPH::Vec3 col = out[*midIdx].GetAxisX();
    EXPECT_NEAR( col.Length(), 2.0F, 1e-3F )
         << "a 2x scale did not survive a direct mapping; the Mat44 path is narrower than it looks";
}

// ---------------------------------------------------------------------------------------------------
// 6. THE CHAIN PATH — the one piece of §3.19's promise that is real, driven for real.
//
// Target = the probe with one extra joint inserted between IK_Elbow and IK_Hand: the purchased-rig case
// (a twist bone the source rig does not have). Jolt's Initialize should produce one Chain, and Map should
// place the extra joint.
//
// THE WHOLE CLIP, NOT ONE SAMPLE. The first draft took the mid-clip pose and measured 5e-5 cm of travel,
// read it as "the chain is inert" and was wrong: IKProbe_Swing is symmetric and tick 24000 carries the
// same rotations as tick 0, so the sample chosen was the rest pose. A single sample is not a measurement
// of a moving thing.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, AnExtraIntermediateJointBecomesAChainAndIsPlaced )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones = ProbeBones();
    const Skeleton source{ std::vector<BoneInfo>( bones ) };

    std::vector<BoneInfo> extended;
    const auto            srcMid = source.FindBoneIndex( kMid );
    const auto            srcTip = source.FindBoneIndex( kTip );
    ASSERT_TRUE( srcMid && srcTip );
    for ( size_t i = 0; i < bones.size(); ++i )
    {
        if ( i == *srcTip )
        {
            BoneInfo twist;
            twist.Name               = "IK_Twist";
            twist.ParentBoneID       = static_cast<uint32_t>( *srcMid );
            twist.LocalBindTransform = bones[*srcTip].LocalBindTransform;
            twist.LocalBindTransform[3][0] *= 0.5F;
            twist.LocalBindTransform[3][1] *= 0.5F;
            twist.LocalBindTransform[3][2] *= 0.5F;
            // THE BEND IS WHAT MAKES THE CHAIN BRANCH OBSERVABLE, and the first version of this test did
            // not have it: with the twist exactly bisecting the source's own offset, the target chain
            // already points where the source points, Quat::sFromTo returns the identity, and replacing
            // that whole line with sIdentity() by hand changed the measured travel by ZERO. A mutation
            // that does not redden may be equivalent IN THE SCENARIO rather than harmless, which is
            // exactly what happened here.
            twist.LocalBindTransform =
                 twist.LocalBindTransform *
                 glm::rotate( glm::mat4( 1.0F ), glm::radians( 30.0F ), glm::vec3( 0, 0, 1 ) );
            extended.push_back( twist );

            BoneInfo hand     = bones[*srcTip];
            hand.ParentBoneID = static_cast<uint32_t>( extended.size() - 1 );
            hand.LocalBindTransform[3][0] *= 0.5F;
            hand.LocalBindTransform[3][1] *= 0.5F;
            hand.LocalBindTransform[3][2] *= 0.5F;
            extended.push_back( hand );
            continue;
        }
        BoneInfo b = bones[i];
        if ( !b.IsRoot() && b.GetParentID() >= *srcTip )
            b.ParentBoneID = b.GetParentID() + 1; // an index shifted by the insertion
        extended.push_back( b );
    }
    Skeleton target( std::move( extended ) );
    target.RecomputeOffsetMatrices();
    ASSERT_TRUE( target.GetStructureError().empty() ) << target.GetStructureError();
    ASSERT_EQ( target.GetBones().size(), bones.size() + 1 );

    JPH::Skeleton joltSource;
    JPH::Skeleton joltTarget;
    FillJoltSkeleton( source, joltSource );
    FillJoltSkeleton( target, joltTarget );

    const auto srcBindJolt  = ToJoltArray( ModelSpace( source, BindPose( source ) ) );
    const auto tgtBind      = BindPose( target );
    const auto tgtBindModel = ModelSpace( target, tgtBind );
    const auto tgtBindJolt  = ToJoltArray( tgtBindModel );

    std::vector<JPH::Mat44> tgtLocalJolt;
    for ( size_t i = 0; i < tgtBind.Size(); ++i )
        tgtLocalJolt.push_back( ToJolt( tgtBind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltSource, srcBindJolt.data(), &joltTarget, tgtBindJolt.data() );

    EXPECT_EQ( mapper.GetMappings().size(), 5u ) << "the five shared names still map 1:1";
    EXPECT_EQ( mapper.GetChains().size(), 1u )
         << "IK_Elbow -> IK_Twist -> IK_Hand is exactly the run Chain exists for";
    EXPECT_EQ( mapper.GetUnmapped().size(), 0u ) << "IK_Twist is inside the chain, not left over";

    const auto clip     = ProbeClip();
    const auto twistIdx = target.FindBoneIndex( "IK_Twist" );
    ASSERT_TRUE( twistIdx.has_value() );

    float moved = 0.0F;
    for ( const double t : SampleTicks( clip ) )
    {
        const auto src = ToJoltArray( ModelSpace( source, PoseAt( source, clip, t ) ) );

        std::vector<JPH::Mat44> out( target.GetBones().size(), JPH::Mat44::sIdentity() );
        mapper.Map( src.data(), tgtLocalJolt.data(), out.data() );
        moved = std::max( moved,
                          Distance( TranslationOf( out[*twistIdx] ), glm::vec3( tgtBindModel[*twistIdx][3] ) ) );
    }

    std::cout << "[ MEASURED ] the unmapped intermediate joint travelled " << moved
              << " cm over the clip -- the chain path is real and it is the one thing here we would "
                 "otherwise have to write\n";
    EXPECT_GT( moved, 1.0F ) << "the unmapped intermediate joint did not move: the chain is inert";

    // AND THE RE-AIM ITSELF, measured against what the direct mapping alone would have produced. Map()
    // rewrites the chain START's rotation with Quat::sFromTo(actual, desired); without that line the
    // elbow keeps D[elbow] * neutral2[elbow] exactly. The angle between the two X axes is therefore a
    // direct observation of the one piece of machinery §3.19 credits this class with.
    const auto elbowIdx = target.FindBoneIndex( kMid );
    ASSERT_TRUE( elbowIdx.has_value() );

    const auto srcModel = ModelSpace( source, PoseAt( source, clip, clip.DurationTicks.Value * 0.3 ) );
    std::vector<JPH::Mat44> out( target.GetBones().size(), JPH::Mat44::sIdentity() );
    mapper.Map( ToJoltArray( srcModel ).data(), tgtLocalJolt.data(), out.data() );

    const auto      srcBindModel = ModelSpace( source, BindPose( source ) );
    const glm::mat4 directOnly =
         srcModel[*srcMid] * glm::inverse( srcBindModel[*srcMid] ) * tgtBindModel[*elbowIdx];

    const JPH::Vec3 gotAxis = out[*elbowIdx].GetAxisX();
    const glm::vec3 got     = glm::normalize( glm::vec3( gotAxis.GetX(), gotAxis.GetY(), gotAxis.GetZ() ) );
    const glm::vec3 direct  = glm::normalize( glm::vec3( directOnly[0] ) );
    const float     degrees = glm::degrees( std::acos( std::clamp( glm::dot( got, direct ), -1.0F, 1.0F ) ) );

    std::cout << "[ MEASURED ] the chain re-aim turned the chain start by " << degrees
              << " deg away from what the direct mapping alone gives\n";
    EXPECT_GT( degrees, 1.0F )
         << "the chain start was not re-aimed, so Quat::sFromTo produced the identity and this scenario "
            "cannot tell the chain branch from its absence";
}

// ---------------------------------------------------------------------------------------------------
// 7. THE OUTPUT IS IN THE WRONG CURRENCY FOR OUR PIPELINE, and this measures the conversion, not the idea.
//
// T5.4 made the rig a pose->pose operator: LocalPose in, LocalPose out (parent-relative TRS). Map() hands
// back MODEL-SPACE Mat44 and takes model-space Mat44. Putting it in the pipeline therefore costs, per
// character per frame, one ComponentPose resolve on the way in and, on the way out, one 4x4 INVERSE and
// one BoneTransform::FromMatrix per bone -- and FromMatrix is a refusal path (it rejects a non-positive
// determinant rather than straightening a mirror), so every frame of a retarget acquires a way to fail.
//
// The round trip is measured rather than argued: does Map()'s output survive being turned back into a
// LocalPose and re-resolved?
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, ConvertingTheOutputBackToALocalPoseRoundTrips )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones = ProbeBones();
    const Skeleton rig{ std::vector<BoneInfo>( bones ) };
    const auto     clip = ProbeClip();

    JPH::Skeleton joltRig;
    FillJoltSkeleton( rig, joltRig );

    const auto bind     = BindPose( rig );
    const auto bindJolt = ToJoltArray( ModelSpace( rig, bind ) );

    std::vector<JPH::Mat44> localJolt;
    for ( size_t i = 0; i < bind.Size(); ++i )
        localJolt.push_back( ToJolt( bind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltRig, bindJolt.data(), &joltRig, bindJolt.data() );

    const auto model = ModelSpace( rig, PoseAt( rig, clip, clip.DurationTicks.Value * 0.25 ) );
    const auto src   = ToJoltArray( model );

    std::vector<JPH::Mat44> out( src.size(), JPH::Mat44::sIdentity() );
    mapper.Map( src.data(), localJolt.data(), out.data() );

    // Model space -> parent-relative -> TRS, which is what a pipeline stage would have to do.
    LocalPose back( rig.GetBones().size() );
    for ( uint32_t i = 0; i < rig.GetBones().size(); ++i )
    {
        glm::mat4 m( 1.0F );
        for ( int c = 0; c < 4; ++c )
        {
            const JPH::Vec4 col = out[i].GetColumn4( c );
            m[c]                = glm::vec4( col.GetX(), col.GetY(), col.GetZ(), col.GetW() );
        }
        const uint32_t parent = rig.ResolveParent( i );
        if ( parent != Skeleton::NO_PARENT )
        {
            glm::mat4 pm( 1.0F );
            for ( int c = 0; c < 4; ++c )
            {
                const JPH::Vec4 col = out[parent].GetColumn4( c );
                pm[c]               = glm::vec4( col.GetX(), col.GetY(), col.GetZ(), col.GetW() );
            }
            m = glm::inverse( pm ) * m;
        }
        auto trs = Desert::Animation::BoneTransform::FromMatrix( m );
        ASSERT_TRUE( trs.IsSuccess() ) << "bone " << bones[i].Name << ": " << trs.GetError();
        back[i] = trs.ExtractValue();
    }

    const auto reresolved = ModelSpace( rig, back );
    for ( size_t i = 0; i < reresolved.size(); ++i )
        EXPECT_LT( Distance( glm::vec3( reresolved[i][3] ), glm::vec3( model[i][3] ) ), 1e-2F )
             << "bone " << bones[i].Name << " did not survive the model -> local -> model round trip";
}

// ---------------------------------------------------------------------------------------------------
// 8a. A PRECONDITION OUR SKELETON DELIBERATELY DOES NOT MEET — and it is an assert, not a refusal.
//
// Map()'s unmapped branch asserts `parent < joint` by ARRAY INDEX, and LockTranslations /
// LockAllTranslations assert AreJointsCorrectlyOrdered(). Our Skeleton says the opposite in as many
// words: "the bone ARRAY order is deliberately left alone -- bone indices are on disk in every .skmesh"
// (Skeleton.hpp), which is why GetResolveOrder() exists at all. A rig exported child-first is legal for
// us and silently wrong for Jolt in Release, where JPH_ASSERT compiles to ((void)0).
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, AChildFirstRigIsLegalForUsAndRejectedByJolt )
{
    std::vector<BoneInfo> bones( 2 );
    bones[0].Name               = "Child";
    bones[0].ParentBoneID       = 1;
    bones[0].LocalBindTransform = glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 50.0F, 0.0F ) );
    bones[1].Name               = "Parent";
    bones[1].LocalBindTransform = glm::translate( glm::mat4( 1.0F ), glm::vec3( 0.0F, 100.0F, 0.0F ) );

    const Skeleton rig( std::move( bones ) );
    EXPECT_TRUE( rig.GetStructureError().empty() ) << "child-first is a legal bone array for this engine";
    EXPECT_EQ( rig.GetResolveOrder().front(), 1u ) << "and the resolve order is what makes it legal";

    JPH::Skeleton joltRig;
    FillJoltSkeleton( rig, joltRig );
    EXPECT_FALSE( joltRig.AreJointsCorrectlyOrdered() )
         << "Jolt would accept this rig; then Map()'s `parents first` assert is not load-bearing and this "
            "test should be deleted rather than relaxed";

    // AND THE CONVERSION ITSELF HAS A PRECONDITION NOBODY STATES. Jolt's index-taking AddJoint overload
    // reads `mJoints[parentIndex].mName` while the array is still being filled, so it requires
    // parentIndex < the joint's own index -- the same parents-first rule, one layer earlier and with no
    // assert on it at all. Checked here rather than triggered: calling it on this rig is an out-of-bounds
    // read, which is how it was found.
    const auto& b = rig.GetBones();
    ASSERT_EQ( b.size(), 2u );
    EXPECT_GT( b[0].GetParentID(), 0u )
         << "this rig no longer has a bone whose parent sits after it; the precondition above is "
            "untestable and the note should go";
}

// ---------------------------------------------------------------------------------------------------
// 8b. THE RETARGET DIRECTION ITSELF IS A PRECONDITION VIOLATION.
//
// Initialize() asserts n1 <= n2 -- "Skeleton 1 should be the low detail skeleton". The use T6 exists for
// is a purchased clip on a bought rig driving OUR character, and there is no rule that the seller's rig
// has fewer bones than ours. The probes alone already give the violating pair.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, TheSourceRigMayBeLargerThanTheTargetAndJoltForbidsThat )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const Skeleton fiveBone( ProbeBones() );

    const std::string raw = ReadFile( RepoRoot() + "Editor/Cooked/Meshes/TwoBoneProbe.skeleton" );
    ASSERT_FALSE( raw.empty() );
    auto data = rfl::json::read<Desert::Assets::Serialization::SkeletonAssetData, rfl::DefaultIfMissing>( raw );
    ASSERT_TRUE( data.has_value() );
    const Skeleton twoBone( std::move( data.value().Bones ) );

    JPH::Skeleton joltBig;
    JPH::Skeleton joltSmall;
    FillJoltSkeleton( fiveBone, joltBig );
    FillJoltSkeleton( twoBone, joltSmall );

    // Not called: Initialize(big, ..., small, ...) trips a JPH_ASSERT in Debug and produces an undefined
    // mapping in Release. The pair is asserted instead -- it is the fact a caller would have to design
    // around, and checking it rather than stating it in prose means a future Jolt that lifts the
    // restriction makes this test's premise visibly false.
    EXPECT_GT( joltBig.GetJointCount(), joltSmall.GetJointCount() )
         << "the corpus no longer contains a source rig larger than a target rig";
}

int main( int argc, char** argv )
{
    // See the file header: JPH::Allocate is a null function pointer until this runs, and PhysicsWorld::Init
    // is the only thing in the tree that runs it today.
    JPH::RegisterDefaultAllocator();

    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
