// THE RIG THAT CAN SEE — a suite about what an INSTRUMENT is able to distinguish, not about what a
// function returns.
//
// WHY IT EXISTS. Every rig this repository has ever contained is ONE bone called "Root", with an IDENTITY
// OffsetMatrix and no parent. That is not a small rig; it is a BLIND one. A bone's skinning matrix is
// `global * OffsetMatrix`, so when OffsetMatrix is the identity the bone's own space and the space the
// skinning matrix lives in are the SAME SPACE — and every defect that confuses the two produces, on this
// rig, exactly the same numbers as the correct code. Not a smaller error: the same bytes. No test and no
// frame taken on the one-bone probe can refuse such a defect, in either direction.
//
// One of them is in the tree as this suite lands. Animator::CalculateBlendedPose cross-fades by
// decomposing `m_CurrentPose.BoneMatrices[j]` — which is `global * OffsetMatrix` — and slerping the
// ROTATION OF THAT PRODUCT, i.e. of a quantity that carries the inverse bind pose inside it. The
// measurements below are what that costs: 0.000 cm on the one-bone probe, up to 254.96 cm here.
//
// WHAT THIS SUITE ASSERTS, AND WHAT IT DELIBERATELY DOES NOT. It does not assert that the engine blends
// correctly — fixing that belongs to the pose substrate work, and a suite that pinned today's answer
// would have to be rewritten by the fix. It asserts the property that makes the fix CHECKABLE: that on
// this rig the two candidate orders (blend the bone transforms and then skin, versus skin and then blend)
// are measurably different, and that on the one-bone probe they are bit-for-bit the same. That statement
// is true before the fix and after it, and it is the reason the rig is worth shipping.
//
// The rig is also rebuilt here from its construction rather than read and trusted. TwoBoneProbe.* were
// hand-authored (no importer produces them; see .gitignore's fifth exception), so the recipe has to live
// somewhere a compiler reads, or the files are 15 KB of numbers nobody can check.

#include <gtest/gtest.h>

#include <Common/Core/AssetHandle.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/ClipSkeletonMatch.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TimeModel.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>

#include <rflcpp/rfl.hpp>
#include <rflcpp/rfl/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <optional>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kCookedDir = "Editor/Cooked/Meshes/";
    constexpr const char* kSceneFile = "Editor/Resources/Assets/Scenes/ANIM_TwoBoneWitness.desce";

    constexpr const char* kBaseBone = "Base";
    constexpr const char* kArmBone  = "Arm";

    // The handle the shipped scene stores for the shipped witness mesh, derived from the project-relative
    // cooked path exactly as AssetHandle::StableKeyForPath derives it. Pinned for the same reason the
    // one-bone probe's is pinned next door: it is a number in a file no compiler reads.
    constexpr const char*   kWitnessMeshPath   = "Cooked/Meshes/TwoBoneProbe.skmesh";
    constexpr std::uint64_t kWitnessMeshHandle = 5756835276253557057ull;

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + std::string( kCookedDir ) + "TwoBoneProbe.skeleton" );
            if ( probe )
            {
                return prefix;
            }
            prefix += "../";
        }
        return {};
    }

    std::string ReadFile( const std::string& path )
    {
        const std::ifstream in( path, std::ios::binary );
        if ( !in )
        {
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // ---------------------------------------------------------------- the construction, in code

    glm::mat4 BaseLocalBind()
    {
        // 1 world unit = 1 cm. A hip-height root that is ALSO rotated: a root whose bind is a pure
        // translation still leaves the rotational half of the bind pose untested.
        return glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 100.0f, 0.0f ) ) *
               glm::rotate( glm::mat4( 1.0f ), glm::radians( 30.0f ), glm::vec3( 0.0f, 0.0f, 1.0f ) );
    }

    glm::mat4 ArmLocalBind()
    {
        return glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 90.0f, 0.0f ) ) *
               glm::rotate( glm::mat4( 1.0f ), glm::radians( -25.0f ), glm::vec3( 1.0f, 0.0f, 0.0f ) );
    }

    std::vector<Desert::Animation::BoneInfo> WitnessBones()
    {
        const glm::mat4 baseGlobal = BaseLocalBind();
        const glm::mat4 armGlobal  = baseGlobal * ArmLocalBind();

        Desert::Animation::BoneInfo base;
        base.Name               = kBaseBone;
        base.LocalBindTransform = BaseLocalBind();
        base.OffsetMatrix       = glm::inverse( baseGlobal );
        base.ParentBoneID       = std::nullopt;

        Desert::Animation::BoneInfo arm;
        arm.Name               = kArmBone;
        arm.LocalBindTransform = ArmLocalBind();
        arm.OffsetMatrix       = glm::inverse( armGlobal );
        arm.ParentBoneID       = 0u;

        return { base, arm };
    }

    // ---------------------------------------------------------------- reading what was shipped

    Desert::Assets::Serialization::SkeletonAssetData LoadSkeletonData( const std::string& stem )
    {
        const std::string raw = ReadFile( RepoRoot() + kCookedDir + stem + ".skeleton" );
        EXPECT_FALSE( raw.empty() ) << "could not read " << stem << ".skeleton";
        auto data =
             rfl::json::read<Desert::Assets::Serialization::SkeletonAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() );
        return data.has_value() ? data.value() : Desert::Assets::Serialization::SkeletonAssetData{};
    }

    Desert::Assets::Serialization::MeshAssetData LoadMeshData( const std::string& stem )
    {
        const std::string raw = ReadFile( RepoRoot() + kCookedDir + stem + ".skmesh" );
        EXPECT_FALSE( raw.empty() ) << "could not read " << stem << ".skmesh";
        // Through the engine's own reader (B11): a cooked mesh is a binary container, and a suite that
        // parsed the fixture as JSON would be reading it by a route the engine does not take.
        auto data = Desert::Assets::Serialization::ReadMeshAssetData( raw, stem + ".skmesh" );
        EXPECT_TRUE( data.IsSuccess() ) << ( data.IsSuccess() ? std::string{} : data.GetError() );
        return data.IsSuccess() ? data.GetValue() : Desert::Assets::Serialization::MeshAssetData{};
    }

    // Built exactly as AnimationAsset::Load builds it: the same reader with the same policy, then the same
    // pure build step, so anything this suite accepts the engine accepts.
    Desert::Animation::AnimationClip LoadClip( const std::string& stem )
    {
        const std::string raw = ReadFile( RepoRoot() + kCookedDir + stem + ".anim" );
        EXPECT_FALSE( raw.empty() ) << "could not read " << stem << ".anim";
        auto data =
             rfl::json::read<Desert::Assets::Serialization::AnimationAssetData, rfl::DefaultIfMissing>( raw );
        EXPECT_TRUE( data.has_value() );
        if ( !data.has_value() )
        {
            return {};
        }
        auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data.value() );
        EXPECT_TRUE( built.IsSuccess() ) << stem << ": " << ( built.IsSuccess() ? "" : built.GetError() );
        return built.IsSuccess() ? built.ExtractValue() : Desert::Animation::AnimationClip{};
    }

    Desert::Animation::Skeleton RigFromFile( const std::string& stem )
    {
        auto bones = LoadSkeletonData( stem ).Bones;
        return Desert::Animation::Skeleton( std::move( bones ) );
    }

    Desert::Animation::ClipRigIdentity IdentityOf( const Desert::Animation::AnimationClip& clip )
    {
        Desert::Animation::ClipRigIdentity id;
        id.ClipName          = clip.AnimationName;
        id.SkeletonSignature = clip.SkeletonSignature;
        for ( const auto& track : clip.Tracks )
        {
            if ( !track.BoneName.empty() )
            {
                id.AnimatedBones.push_back( track.BoneName );
            }
        }
        return id;
    }

    // ---------------------------------------------------------------- the two candidate blend orders
    //
    // A SECOND IMPLEMENTATION ON PURPOSE. These mirror the TRS decompose/blend/compose the Animator uses,
    // and they are written here rather than called from there because the question this suite asks is
    // about the RIG, not about the engine: it must keep its meaning while Animator is rewritten under it.

    // Initialised for the reason Assets/Serialization/Animation.hpp gives for its own mirrors: glm leaves
    // its components indeterminate, and every field here is read back by the comparison below.
    struct TRS
    {
        glm::vec3 Translation = glm::vec3( 0.0f );
        glm::quat Rotation    = glm::quat( 1.0f, 0.0f, 0.0f, 0.0f );
        glm::vec3 Scale       = glm::vec3( 1.0f );
    };

    TRS Decompose( const glm::mat4& m )
    {
        TRS out;
        out.Translation = glm::vec3( m[3] );
        out.Scale       = glm::vec3( glm::length( glm::vec3( m[0] ) ), glm::length( glm::vec3( m[1] ) ),
                                     glm::length( glm::vec3( m[2] ) ) );

        glm::mat3 rotation;
        rotation[0]  = glm::vec3( m[0] ) / out.Scale.x;
        rotation[1]  = glm::vec3( m[1] ) / out.Scale.y;
        rotation[2]  = glm::vec3( m[2] ) / out.Scale.z;
        out.Rotation = glm::quat_cast( rotation );
        return out;
    }

    glm::mat4 BlendMatrices( const glm::mat4& a, const glm::mat4& b, float alpha )
    {
        const TRS ta = Decompose( a );
        const TRS tb = Decompose( b );

        const glm::mat4 t = glm::translate( glm::mat4( 1.0f ), glm::mix( ta.Translation, tb.Translation, alpha ) );
        const glm::mat4 r = glm::toMat4( glm::slerp( ta.Rotation, tb.Rotation, alpha ) );
        const glm::mat4 s = glm::scale( glm::mat4( 1.0f ), glm::mix( ta.Scale, tb.Scale, alpha ) );
        return t * r * s;
    }

    // Blend the BONE's own transforms, then take the result into skinning space. The bind pose is applied
    // once, to a finished pose.
    glm::mat4 BlendThenSkin( const glm::mat4& a, const glm::mat4& b, const glm::mat4& offset, float alpha )
    {
        return BlendMatrices( a, b, alpha ) * offset;
    }

    // Take each pose into skinning space first, then blend those. The inverse bind pose is inside the
    // interpolation, so the slerp turns a quantity that is not a bone orientation.
    glm::mat4 SkinThenBlend( const glm::mat4& a, const glm::mat4& b, const glm::mat4& offset, float alpha )
    {
        return BlendMatrices( a * offset, b * offset, alpha );
    }

    // The animated global transform of every bone at one time, i.e. what the parent chain produces before
    // the offset is applied. A clip track REPLACES the bind local, which is why the corpus authors its
    // keys around the bind values.
    // `seconds` rather than a tick, because the callers sweep eighths of a second and that is the unit
    // the witness's own protocol is written in; the conversion onto the clip's grid happens once, here.
    std::vector<glm::mat4> GlobalsAt( const Desert::Animation::Skeleton&      rig,
                                      const Desert::Animation::AnimationClip& clip, float seconds )
    {
        const auto&            bones = rig.GetBones();
        std::vector<glm::mat4> local( bones.size(), glm::mat4( 1.0f ) );
        for ( std::size_t i = 0; i < bones.size(); ++i )
        {
            local[i] = bones[i].LocalBindTransform;
            for ( const auto& track : clip.Tracks )
            {
                if ( track.BoneName == bones[i].Name )
                {
                    // `BoneTrack::GetTransform` composed P/R/S into a mat4 and was removed by А1: every
                    // caller decomposed it again immediately, so the matrix was a round trip with no
                    // consumer on the animation system's hottest path. `Sample` returns the three stored
                    // quantities and this test composes them itself, which is what it wanted anyway.
                    const auto at =
                         Desert::Animation::SecondsToFrameTime( static_cast<double>( seconds ), clip.TickRate );
                    local[i] = track.Sample( at, clip.TickRate ).ToMatrix();
                }
            }
        }

        std::vector<glm::mat4> global( bones.size(), glm::mat4( 1.0f ) );
        for ( std::size_t i = 0; i < bones.size(); ++i )
        {
            const std::optional<std::uint32_t> parent = bones[i].ParentBoneID;

            global[i] = local[i];
            if ( parent.has_value() && *parent < i )
            {
                global[i] = global[*parent] * local[i];
            }
        }
        return global;
    }

    // The largest distance, in centimetres, that any skinned vertex of `mesh` ends up apart under the two
    // blend orders — swept over both clips' key times and three alphas. Distance on VERTICES rather than on
    // matrix entries because centimetres are what a frame shows and what a bug report can be written in.
    float WidestDisagreement( const Desert::Animation::Skeleton&                  rig,
                              const Desert::Assets::Serialization::MeshAssetData& mesh,
                              const Desert::Animation::AnimationClip&             clipA,
                              const Desert::Animation::AnimationClip&             clipB )
    {
        const auto& bones  = rig.GetBones();
        float       widest = 0.0f;

        for ( int ia = 0; ia <= 16; ++ia )
        {
            const float timeA = static_cast<float>( ia ) / 8.0f;
            for ( int ib = 0; ib <= 16; ++ib )
            {
                const float timeB   = static_cast<float>( ib ) / 8.0f;
                const auto  globalA = GlobalsAt( rig, clipA, timeA );
                const auto  globalB = GlobalsAt( rig, clipB, timeB );

                for ( const float alpha : { 0.25f, 0.5f, 0.75f } )
                {
                    for ( std::size_t j = 0; j < bones.size(); ++j )
                    {
                        const glm::mat4 lhs =
                             BlendThenSkin( globalA[j], globalB[j], bones[j].OffsetMatrix, alpha );
                        const glm::mat4 rhs =
                             SkinThenBlend( globalA[j], globalB[j], bones[j].OffsetMatrix, alpha );

                        for ( const auto& vertex : mesh.SkinnedVertices )
                        {
                            if ( vertex.BoneIDs[0] != static_cast<std::uint32_t>( j ) )
                            {
                                continue;
                            }
                            const glm::vec4 p( vertex.Position, 1.0f );
                            widest = std::max( widest, glm::length( glm::vec3( lhs * p - rhs * p ) ) );
                        }
                    }
                }
            }
        }
        return widest;
    }
} // namespace

// THE CONSTRUCTION IS THE SOURCE, THE FILE IS THE COPY. TwoBoneProbe.skeleton was authored by hand, so the
// recipe has to be readable somewhere a compiler checks it — otherwise the rig is 500 bytes of numbers that
// can rot without anything noticing.
TEST( TwoBoneWitness, TheShippedRigIsTheChainThisSuiteDescribes )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";

    const auto data     = LoadSkeletonData( "TwoBoneProbe" );
    const auto expected = WitnessBones();

    ASSERT_EQ( data.Bones.size(), expected.size() );
    for ( std::size_t i = 0; i < expected.size(); ++i )
    {
        EXPECT_EQ( data.Bones[i].Name, expected[i].Name );
        EXPECT_EQ( data.Bones[i].ParentBoneID, expected[i].ParentBoneID );
        for ( int c = 0; c < 4; ++c )
        {
            for ( int r = 0; r < 4; ++r )
            {
                EXPECT_NEAR( data.Bones[i].LocalBindTransform[c][r], expected[i].LocalBindTransform[c][r], 1e-3f )
                     << "bone " << expected[i].Name << " bind [" << c << "][" << r << "]";
                EXPECT_NEAR( data.Bones[i].OffsetMatrix[c][r], expected[i].OffsetMatrix[c][r], 1e-3f )
                     << "bone " << expected[i].Name << " offset [" << c << "][" << r << "]";
            }
        }
    }

    // One identity, derived twice and written into four files no compiler reads: the rig's own signature
    // field, the mesh's SkeletonSignature, and both clips'.
    const std::uint64_t signature = Desert::Animation::Skeleton::ComputeSignature( data.Bones );
    EXPECT_EQ( data.Signature, signature ) << "TwoBoneProbe.skeleton's stored signature is not the one the "
                                              "engine derives for the rig inside it.";
    EXPECT_EQ( LoadMeshData( "TwoBoneProbe" ).SkeletonSignature.value_or( 0ull ), signature );
    for ( const char* stem : { "TwoBoneProbe_Wave", "TwoBoneProbe_Twist" } )
    {
        EXPECT_EQ( LoadClip( stem ).SkeletonSignature, signature )
             << stem
             << " claims a different rig from TwoBoneProbe.skeleton. Every frame taken against it "
                "would show a bind pose and still render, which is broken evidence, not no evidence.";
    }

    EXPECT_EQ( static_cast<std::uint64_t>( Common::AssetHandle::FromCookedPath( kWitnessMeshPath ) ),
               kWitnessMeshHandle );
    const std::string scene = ReadFile( RepoRoot() + kSceneFile );
    ASSERT_FALSE( scene.empty() ) << "could not read " << kSceneFile;
    EXPECT_NE( scene.find( "\"MeshGuid\": " + std::to_string( kWitnessMeshHandle ) ), std::string::npos )
         << kSceneFile
         << " no longer stores the witness mesh's path-derived handle, so the scene that "
            "places the rig would resolve to no mesh at all.";
}

// THE INSTRUMENT MUST NOT BE DEGENERATE. Each of these is a way the rig could quietly become the one-bone
// probe again while still parsing, still rendering, and still passing every other test in this suite.
TEST( TwoBoneWitness, NeitherBoneCollapsesIntoTheSpaceItIsSupposedToSeparate )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto  rig   = RigFromFile( "TwoBoneProbe" );
    const auto& bones = rig.GetBones();
    ASSERT_EQ( bones.size(), 2u );

    ASSERT_TRUE( bones[0].IsRoot() );
    ASSERT_TRUE( bones[1].ParentBoneID.has_value() );
    EXPECT_EQ( bones[1].GetParentID(), 0u ) << "the second bone is no longer a CHILD of the first, so "
                                               "there is no parent chain left to get wrong.";

    // A rig whose bones are named like the one-bone corpus's or like the foreign negative control would be
    // handed their clips by ClipSkeletonMatch's name-overlap fallback, which is the opposite of an
    // instrument.
    EXPECT_NE( bones[0].Name, "Root" );
    EXPECT_NE( bones[1].Name, "Root" );
    EXPECT_NE( bones[0].Name, "Hips" );
    EXPECT_NE( bones[1].Name, "Hips" );

    for ( const auto& bone : bones )
    {
        float widest = 0.0f;
        for ( int c = 0; c < 4; ++c )
        {
            for ( int r = 0; r < 4; ++r )
            {
                widest = std::max( widest, std::fabs( bone.OffsetMatrix[c][r] - glm::mat4( 1.0f )[c][r] ) );
            }
        }
        EXPECT_GT( widest, 0.1f ) << "bone '" << bone.Name
                                  << "' has an (almost) identity OffsetMatrix, which makes its bone space "
                                     "and its skinning space the same space — the exact blindness this rig "
                                     "was added to remove.";
    }

    // And the parent transform the child is composed through must itself be non-identity, or the chain is
    // a chain in name only.
    float parentTravel = 0.0f;
    for ( int c = 0; c < 4; ++c )
    {
        for ( int r = 0; r < 4; ++r )
        {
            parentTravel = std::max( parentTravel,
                                     std::fabs( bones[0].LocalBindTransform[c][r] - glm::mat4( 1.0f )[c][r] ) );
        }
    }
    EXPECT_GT( parentTravel, 0.1f );
}

// THE WITNESS ITSELF, and it is a statement about the RIG rather than about any implementation: on this rig
// the two candidate orders disagree by more than two metres; on the one-bone probe they agree exactly.
// Both halves are needed. The second is what proves the first is a property of the instrument and not of
// the arithmetic — and it is the measured reason a whole class of defect has been invisible here.
TEST( TwoBoneWitness, TheRigSeparatesBlendingBeforeSkinningFromBlendingAfterIt )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const float blind = WidestDisagreement( RigFromFile( "SkinProbe" ), LoadMeshData( "SkinProbe" ),
                                            LoadClip( "SkinProbe_Hover" ), LoadClip( "SkinProbe_Tilt" ) );
    EXPECT_LT( blind, 1e-3f )
         << "the one-bone probe now separates the two blend orders by " << blind
         << " cm. That is not an improvement to celebrate here: this suite's claim is that it does NOT, and "
            "the number above means the probe rig changed under a test that reads it as a control.";

    const float seen = WidestDisagreement( RigFromFile( "TwoBoneProbe" ), LoadMeshData( "TwoBoneProbe" ),
                                           LoadClip( "TwoBoneProbe_Wave" ), LoadClip( "TwoBoneProbe_Twist" ) );
    EXPECT_GT( seen, 100.0f )
         << "the two-bone witness separates the two blend orders by only " << seen
         << " cm. Below roughly a body's width the difference stops being something a frame can settle, and "
            "the rig has stopped being an instrument even though every other test here still passes.";
}

// THE TWO CORPORA MUST NOT DRIVE EACH OTHER'S RIGS. ClipDrivesRig accepts a clip whose animated bones are
// mostly present BY NAME even when the signature disagrees, so one careless bone name would hand the
// witness rig the one-bone corpus (and the Foreign_Hips negative control with it) and quietly turn two
// instruments into one.
TEST( TwoBoneWitness, TheWitnessCorpusAndTheOneBoneCorpusStayApart )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto witnessRig = Desert::Animation::IdentifyRig( RigFromFile( "TwoBoneProbe" ) );
    const auto probeRig   = Desert::Animation::IdentifyRig( RigFromFile( "SkinProbe" ) );

    EXPECT_NE( witnessRig.Signature, probeRig.Signature );

    for ( const char* stem : { "TwoBoneProbe_Wave", "TwoBoneProbe_Twist" } )
    {
        const auto identity = IdentityOf( LoadClip( stem ) );
        EXPECT_TRUE( Desert::Animation::ClipDrivesRig( identity, witnessRig ) )
             << stem << " is not offered to the rig it names.";
        EXPECT_FALSE( Desert::Animation::ClipDrivesRig( identity, probeRig ) )
             << stem << " is offered to the one-bone probe rig, which has neither of its bones.";
    }

    for ( const char* stem : { "SkinProbe_Hover", "SkinProbe_Tilt", "Foreign_Hips" } )
    {
        EXPECT_FALSE( Desert::Animation::ClipDrivesRig( IdentityOf( LoadClip( stem ) ), witnessRig ) )
             << stem
             << " is offered to the two-bone witness rig. Rename the bone in the new rig, not the "
                "match rule: the corpus next door depends on it saying no.";
    }
}

// THE WITNESS CLIPS HAVE TO MOVE THE RIG, and move it in two different ways. A corpus whose keys sit at
// the bind value certifies a dead animation system while parsing perfectly; a pair of clips that move the
// rig the SAME way cannot tell a frame which of them played.
TEST( TwoBoneWitness, TheWitnessClipsMoveTheChainAndMoveItDifferently )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto rig   = RigFromFile( "TwoBoneProbe" );
    const auto wave  = LoadClip( "TwoBoneProbe_Wave" );
    const auto twist = LoadClip( "TwoBoneProbe_Twist" );

    for ( const auto* clip : { &wave, &twist } )
    {
        EXPECT_EQ( clip->DurationTicks.Value, 2 * Desert::Animation::PROJECT_TICK_RATE.Numerator )
             << clip->AnimationName
             << " is no longer the 2 s cycle the witness scene's exit time and shot frame counts are "
                "chosen against.";
        EXPECT_EQ( clip->TickRate, Desert::Animation::PROJECT_TICK_RATE )
             << clip->AnimationName << " is not on the project tick grid.";
        ASSERT_EQ( clip->Tracks.size(), 2u ) << clip->AnimationName << " does not drive both bones.";
    }

    // Travel of the ARM's tip, which is the end of the chain and therefore the place where a lost parent
    // transform shows up as centimetres rather than as a rounding difference.
    const glm::vec4 tip( 0.0f, 80.0f, 0.0f, 1.0f );
    auto            travel = [&]( const Desert::Animation::AnimationClip& clip )
    {
        glm::vec3 lo( 1e9f );
        glm::vec3 hi( -1e9f );
        for ( int i = 0; i <= 16; ++i )
        {
            const auto      global = GlobalsAt( rig, clip, static_cast<float>( i ) / 8.0f );
            const glm::vec3 p      = glm::vec3( global[1] * tip );
            lo                     = glm::min( lo, p );
            hi                     = glm::max( hi, p );
        }
        return hi - lo;
    };

    const glm::vec3 waveTravel  = travel( wave );
    const glm::vec3 twistTravel = travel( twist );

    EXPECT_GE( waveTravel.y, 100.0f ) << "TwoBoneProbe_Wave lifts the chain's tip only " << waveTravel.y
                                      << " cm; it is the witness corpus's positive control for translation "
                                         "and has to be unmistakable in a frame.";
    // 60 cm is not a round number chosen for comfort: the arm box is 36 cm across, so this is a sideways
    // sweep of nearly two of its own widths. Measured travel today is 90.5 cm; the floor is set below that
    // rather than at it so a key-spacing change is not a failure, and far enough above zero that a clip
    // which stopped turning could not slip through.
    EXPECT_GE( twistTravel.x, 60.0f ) << "TwoBoneProbe_Twist swings the chain's tip only " << twistTravel.x
                                      << " cm sideways; it is the control for rotation, and a rig barely "
                                         "turned looks unchanged.";

    // Different KINDS of motion: Wave is mostly vertical, Twist mostly horizontal. A frame can then say
    // which clip played rather than only that something played.
    EXPECT_GT( waveTravel.y, twistTravel.y );
    EXPECT_GT( twistTravel.x, waveTravel.x );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
