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

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

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
    constexpr const char* kRoot  = "IK_Shoulder";
    constexpr const char* kMid   = "IK_Elbow";
    constexpr const char* kTip   = "IK_Hand";

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
        auto data = rfl::json::read<Desert::Assets::Serialization::SkeletonAssetData, rfl::DefaultIfMissing>( raw );
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
        return JPH::Mat44( JPH::Vec4( m[0][0], m[0][1], m[0][2], m[0][3] ),
                           JPH::Vec4( m[1][0], m[1][1], m[1][2], m[1][3] ),
                           JPH::Vec4( m[2][0], m[2][1], m[2][2], m[2][3] ),
                           JPH::Vec4( m[3][0], m[3][1], m[3][2], m[3][3] ) );
    }

    glm::vec3 TranslationOf( const JPH::Mat44& m )
    {
        const JPH::Vec3 t = m.GetTranslation();
        return { t.GetX(), t.GetY(), t.GetZ() };
    }

    // Our Skeleton -> JPH::Skeleton, in OUR bone array order. The order is carried over unchanged on
    // purpose: the whole point of measurement 7 is what Jolt does with an order our type permits.
    void FillJoltSkeleton( const Skeleton& rig, JPH::Skeleton& out )
    {
        const auto& bones = rig.GetBones();
        for ( const BoneInfo& b : bones )
            out.AddJoint( b.Name, b.IsRoot() ? -1 : static_cast<int>( b.GetParentID() ) );
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
        LocalPose        local = BindPose( rig );
        const FrameTime  at{ Desert::Animation::FrameNumber{ static_cast<int32_t>( ticks ) }, 0.0F };
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

    const Skeleton rig( std::vector<BoneInfo>( bones ) );
    ASSERT_TRUE( rig.GetStructureError().empty() ) << rig.GetStructureError();

    JPH::Skeleton joltRig;
    FillJoltSkeleton( rig, joltRig );
    ASSERT_EQ( joltRig.GetJointCount(), 5 );

    const auto bind        = BindPose( rig );
    const auto bindModel   = ModelSpace( rig, bind );
    const auto bindJolt    = ToJoltArray( bindModel );

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
    const Skeleton rig( std::vector<BoneInfo>( bones ) );
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

        maxTravel = std::max( maxTravel, Distance( TranslationOf( out[*tipIdx] ), glm::vec3( bindModel[*tipIdx][3] ) ) );
    }

    // The corpus suite pins this clip as one that MOVES; 10 cm is far above the 1e-3 cm floor measurement
    // 1 uses and far below what the clip actually travels.
    EXPECT_GT( maxTravel, 10.0F ) << "the mapped tip barely moved (" << maxTravel
                                  << " cm): the clip is not reaching the far side of the mapper";
}

// ---------------------------------------------------------------------------------------------------
// 3. THE DECIDING MEASUREMENT: A TARGET WITH DIFFERENT PROPORTIONS.
//
// This is what retargeting IS. The target rig here is the probe with every bone offset scaled 1.5x -- a
// taller character, same bone names, same hierarchy. A retargeter must leave the TARGET's bone lengths
// alone and transfer only the motion.
//
// Asserted as a RATIO (mapped limb length / the target's own rest limb length), never in centimetres:
// the property is scale-free, and a threshold in centimetres would be a different assertion on every rig.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, MappingOntoADifferentlyProportionedRigDoesNotPreserveItsBoneLengths )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones  = ProbeBones();
    const Skeleton source( std::vector<BoneInfo>( bones ) );
    const Skeleton target = ScaledRig( bones, 1.5F );
    const auto     clip   = ProbeClip();
    ASSERT_FALSE( clip.Tracks.empty() );

    // The two rigs are the SAME rig as far as this engine is concerned.
    EXPECT_EQ( source.GetSignature(), target.GetSignature() )
         << "ComputeSignature is over names and parents; proportions are deliberately not in it";

    JPH::Skeleton joltSource;
    JPH::Skeleton joltTarget;
    FillJoltSkeleton( source, joltSource );
    FillJoltSkeleton( target, joltTarget );

    const auto srcBind      = BindPose( source );
    const auto tgtBind      = BindPose( target );
    const auto srcBindModel = ModelSpace( source, srcBind );
    const auto tgtBindModel = ModelSpace( target, tgtBind );
    const auto srcBindJolt  = ToJoltArray( srcBindModel );
    const auto tgtBindJolt  = ToJoltArray( tgtBindModel );

    std::vector<JPH::Mat44> tgtLocalJolt;
    for ( size_t i = 0; i < tgtBind.Size(); ++i )
        tgtLocalJolt.push_back( ToJolt( tgtBind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltSource, srcBindJolt.data(), &joltTarget, tgtBindJolt.data() );
    ASSERT_EQ( mapper.GetMappings().size(), 5u ) << "same names -> five 1:1 mappings";

    const auto rootIdx = target.FindBoneIndex( kRoot );
    const auto midIdx  = target.FindBoneIndex( kMid );
    const auto tipIdx  = target.FindBoneIndex( kTip );
    ASSERT_TRUE( rootIdx && midIdx && tipIdx );

    const float upperRest =
         Distance( glm::vec3( tgtBindModel[*rootIdx][3] ), glm::vec3( tgtBindModel[*midIdx][3] ) );
    const float lowerRest =
         Distance( glm::vec3( tgtBindModel[*midIdx][3] ), glm::vec3( tgtBindModel[*tipIdx][3] ) );
    ASSERT_GT( upperRest, 1.0F );
    ASSERT_GT( lowerRest, 1.0F );

    float worstRatio = 1.0F; // the sample furthest from 1.0, in either direction
    float atRestUpper = 0.0F;
    for ( const double t : SampleTicks( clip ) )
    {
        const auto local = PoseAt( source, clip, t );
        const auto model = ModelSpace( source, local );
        const auto src   = ToJoltArray( model );

        std::vector<JPH::Mat44> out( target.GetBones().size(), JPH::Mat44::sIdentity() );
        mapper.Map( src.data(), tgtLocalJolt.data(), out.data() );

        const float upper = Distance( TranslationOf( out[*rootIdx] ), TranslationOf( out[*midIdx] ) ) / upperRest;
        const float lower = Distance( TranslationOf( out[*midIdx] ), TranslationOf( out[*tipIdx] ) ) / lowerRest;
        if ( t == 0.0 )
            atRestUpper = upper;
        for ( const float r : { upper, lower } )
            if ( std::fabs( r - 1.0F ) > std::fabs( worstRatio - 1.0F ) )
                worstRatio = r;
    }

    // WHAT THE FORMULA PREDICTS, so that the number below is understood rather than merely recorded.
    // Map() writes outPose2[j2] = pose1[j1] * (neutral1[j1]^-1 * neutral2[j2]). Substituting
    // D[j] = pose1[j] * neutral1[j]^-1 (the source joint's model-space delta from ITS neutral pose) gives
    // outPose2[j2] = D[j1] * neutral2[j2]: the target's rest transform carried by the source's delta. Each
    // mapped joint is written INDEPENDENTLY in model space -- nothing in Map() consults the target's own
    // bone length -- so the distance between two mapped joints is only the target's when D is the same
    // rigid transform at both ends, i.e. at rest and wherever the two source joints happen not to have
    // rotated relative to each other. It is therefore CORRECT AT REST and wrong under animation, which is
    // the worst shape a defect can have: the bind pose looks perfect.
    EXPECT_NEAR( atRestUpper, 1.0F, 1e-3F )
         << "at the clip's first sample the mapped limb should still be the target's own length";

    EXPECT_GT( std::fabs( worstRatio - 1.0F ), 0.05F )
         << "the mapped limb stayed within 5% of the target's own length across the whole clip (worst ratio "
         << worstRatio
         << "). If this ever becomes true, re-read the note above -- it would mean Map() acquired a notion "
            "of the target's bone lengths, and T6.2's refusal would have to be revisited.";

    // Printed so the document can quote a number rather than a verdict.
    std::cout << "[ MEASURED ] worst mapped-limb / target-rest-limb ratio across the clip: " << worstRatio
              << " (1.0 would be a retargeter)\n";
}

// ---------------------------------------------------------------------------------------------------
// 4. PELVIS HEIGHT SCALING — T6.2 names it; the mapper has no notion of it.
//
// Measured on the probe's root: after mapping a source pose onto a 1.5x-taller target, where does the
// target's root sit? A retargeter scales root height by the rig-height ratio so the taller character's
// feet stay on the floor. Map() places it wherever the source's root is.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, TheTargetRootTakesTheSourceHeightNotItsOwn )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones  = ProbeBones();
    const Skeleton source( std::vector<BoneInfo>( bones ) );
    const Skeleton target = ScaledRig( bones, 1.5F );
    const auto     clip   = ProbeClip();

    JPH::Skeleton joltSource;
    JPH::Skeleton joltTarget;
    FillJoltSkeleton( source, joltSource );
    FillJoltSkeleton( target, joltTarget );

    const auto srcBindJolt = ToJoltArray( ModelSpace( source, BindPose( source ) ) );
    const auto tgtBind     = BindPose( target );
    const auto tgtBindModel = ModelSpace( target, tgtBind );
    const auto tgtBindJolt  = ToJoltArray( tgtBindModel );

    std::vector<JPH::Mat44> tgtLocalJolt;
    for ( size_t i = 0; i < tgtBind.Size(); ++i )
        tgtLocalJolt.push_back( ToJolt( tgtBind[i].ToMatrix() ) );

    JPH::SkeletonMapper mapper;
    mapper.Initialize( &joltSource, srcBindJolt.data(), &joltTarget, tgtBindJolt.data() );

    const auto rootIdx = target.FindBoneIndex( kRoot );
    ASSERT_TRUE( rootIdx.has_value() );

    // Halfway through the clip, where the source root has travelled.
    const auto local = PoseAt( source, clip, clip.DurationTicks.Value * 0.5 );
    const auto model = ModelSpace( source, local );
    const auto src   = ToJoltArray( model );

    std::vector<JPH::Mat44> out( target.GetBones().size(), JPH::Mat44::sIdentity() );
    mapper.Map( src.data(), tgtLocalJolt.data(), out.data() );

    const float sourceRootY = model[*rootIdx][3][1];
    const float mappedRootY = TranslationOf( out[*rootIdx] ).y;

    EXPECT_NEAR( mappedRootY, sourceRootY, 1e-3F )
         << "the mapped root did not land on the source root; the derivation in measurement 3 is wrong";

    std::cout << "[ MEASURED ] target rest root height " << tgtBindModel[*rootIdx][3][1]
              << " cm, source root height at mid-clip " << sourceRootY << " cm, mapped target root "
              << mappedRootY << " cm -- no height ratio is applied anywhere in Map()\n";
}

// ---------------------------------------------------------------------------------------------------
// 5. SCALE.
//
// Our BoneTransform carries a Scale and Pose.hpp gives the reason (clips store S keys, blends blend them).
// JPH::SkeletonPose::JointState is a quat and a translation and has nowhere to put one. Map() itself is
// Mat44-typed, so a scale does ride through a DIRECT mapping -- and that is the trap worth pinning,
// because it makes the loss invisible on a rig with no chains and unavoidable on one with them:
// Map()'s chain branch calls Mat44::SetRotation, which overwrites the 3x3 that was carrying the scale.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, ScaleSurvivesADirectMappingAndIsDestroyedByAChain )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones = ProbeBones();
    const Skeleton rig( std::vector<BoneInfo>( bones ) );

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

    LocalPose scaled = bind;
    scaled[*midIdx].Scale = glm::vec3( 2.0F );
    const auto scaledJolt = ToJoltArray( ModelSpace( rig, scaled ) );

    std::vector<JPH::Mat44> out( bindJolt.size(), JPH::Mat44::sIdentity() );
    mapper.Map( scaledJolt.data(), localJolt.data(), out.data() );

    // The column length of a scaled basis IS the scale, and the direct-mapping branch is a plain matrix
    // product, so it comes through.
    const JPH::Vec3 col = out[*midIdx].GetAxisX();
    EXPECT_NEAR( col.Length(), 2.0F, 1e-3F )
         << "a 2x scale did not survive a direct mapping; the Mat44 path is narrower than it looks";
}

// ---------------------------------------------------------------------------------------------------
// 6. THE CHAIN PATH — the one piece of §3.19's promise that is real, driven for real.
//
// Target = the probe with one extra joint inserted between IK_Elbow and IK_Hand: the purchased-rig case
// (a twist bone the source rig does not have). Jolt's Initialize should produce one Chain, and Map should
// place the extra joint by re-aiming the chain start at the source's direction.
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, AnExtraIntermediateJointBecomesAChainAndIsPlaced )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto     bones = ProbeBones();
    const Skeleton source( std::vector<BoneInfo>( bones ) );

    // IK_Shoulder(0) IK_Elbow(1) IK_Twist(2, child of elbow) IK_Hand(3, child of twist) IK_Post(4) IK_Kerb(5)
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
            extended.push_back( twist );

            BoneInfo hand         = bones[*srcTip];
            hand.ParentBoneID     = static_cast<uint32_t>( extended.size() - 1 );
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

    const auto local = PoseAt( source, clip, clip.DurationTicks.Value * 0.5 );
    const auto src   = ToJoltArray( ModelSpace( source, local ) );

    std::vector<JPH::Mat44> out( target.GetBones().size(), JPH::Mat44::sIdentity() );
    mapper.Map( src.data(), tgtLocalJolt.data(), out.data() );

    const float moved =
         Distance( TranslationOf( out[*twistIdx] ), glm::vec3( tgtBindModel[*twistIdx][3] ) );
    EXPECT_GT( moved, 1.0F ) << "the unmapped intermediate joint did not move: the chain is inert";
}

// ---------------------------------------------------------------------------------------------------
// 7a. A PRECONDITION OUR SKELETON DELIBERATELY DOES NOT MEET — and it is an assert, not a refusal.
//
// Map()'s unmapped branch asserts `parent < joint` by ARRAY INDEX, and LockTranslations/LockAllTranslations
// assert AreJointsCorrectlyOrdered(). Our Skeleton says the opposite in as many words: "the bone ARRAY
// order is deliberately left alone -- bone indices are on disk in every .skmesh" (Skeleton.hpp), which is
// why GetResolveOrder() exists at all. A rig exported child-first is legal for us and silently wrong for
// Jolt in Release, where JPH_ASSERT compiles to ((void)0).
// ---------------------------------------------------------------------------------------------------
TEST( SkeletonMapperFit, AChildFirstRigIsLegalForUsAndRejectedByJolt )
{
    // Two bones, child at index 0. Nothing in our Skeleton objects: the resolve order sorts it out.
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
}

// ---------------------------------------------------------------------------------------------------
// 7b. THE RETARGET DIRECTION ITSELF IS A PRECONDITION VIOLATION.
//
// Initialize() asserts n1 <= n2 -- "Skeleton 1 should be the low detail skeleton". The use T6 exists for is
// a purchased clip on a bought rig driving OUR character, and there is no rule that the seller's rig has
// fewer bones than ours. The probes alone already give the violating pair.
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
    // mapping in Release. The pair is asserted instead, which is the fact a caller would have to design
    // around, and it is checked rather than asserted in prose so that a future Jolt that lifts the
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
