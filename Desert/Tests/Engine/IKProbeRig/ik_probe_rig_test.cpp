// THE RIG THE FRAME IS TAKEN ON, AND THE FOUR AGREEMENTS THAT MAKE THAT FRAME READABLE.
//
// А2 shipped TwoBoneProbe and the note beside it explains why a one-bone rig is a BLIND instrument. IKProbe
// exists because TwoBoneProbe is blind to a different thing: a two-bone IK chain is THREE joints — the end
// bone, its parent and its grandparent — so two bones give ONE segment, which is a look-at, not IK. There
// was no rig in this repository a two-bone solver could run on at all.
//
// Five bones: IK_Shoulder -> IK_Elbow -> IK_Hand is the chain, and IK_Post and IK_Kerb are two further
// ROOTS the solver never names. The shots in Docs/Animation/Shots/A3 make four claims about the picture,
// and each one is only meaningful if a number in the DATA agrees with it. That agreement is what this suite
// is — the relation, not the two sides (verify skill §4):
//
//   "the hand arrived at the post"        needs the scene's authored Goal to BE the post bone's position;
//   "the goal was reached, not clamped"   needs the goal to lie inside the chain's reach shell;
//   "the kerb rectangle is zero pixels"   needs the kerb to be OUT of the chain's reach, so that no pose
//                                         of the arm could have moved it even in principle;
//   "the solve happened at all"           needs the construction in this file to be the shipped bytes.
//
// The construction is rebuilt here in C++ for А2's reason: IKProbe.* are hand-authored, no importer produces
// them, and without a recipe a compiler reads they are 30 KB of numbers that can rot silently.

#include <gtest/gtest.h>

#include <Common/Content/MeshBinaryHeader.hpp>
#include <Common/Content/TextAssetHeader.hpp>
#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Core/Timestep.hpp>

#include <Engine/Animation/AnimationClip.hpp>
#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Animation/TwoBoneIKControl.hpp>
#include <Engine/Assets/Serialization/Animation.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <rflcpp/rfl.hpp>

#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kCookedDir = "Editor/Cooked/Meshes/";
    constexpr const char* kWitness   = "Editor/Resources/Assets/Scenes/ANIM_IKWitness.desce";
    constexpr const char* kNoControl = "Editor/Resources/Assets/Scenes/ANIM_IKWitness_NoIK.desce";

    constexpr const char* kShoulder = "IK_Shoulder";
    constexpr const char* kElbow    = "IK_Elbow";
    constexpr const char* kHand     = "IK_Hand";
    constexpr const char* kPost     = "IK_Post";
    constexpr const char* kKerb     = "IK_Kerb";

    // 1 world unit = 1 cm, and these are the numbers the whole protocol turns on.
    constexpr float kUpperLimbCm = 80.0F;
    constexpr float kLowerLimbCm = 60.0F;
    constexpr float kGoalCm      = 90.0F; // |Goal - IK_Shoulder|, i.e. where in the shell the goal sits

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + std::string( kCookedDir ) + "IKProbe.skeleton" );
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

    glm::mat4 Place( const glm::vec3& translation, float degrees, const glm::vec3& axis )
    {
        return glm::translate( glm::mat4( 1.0F ), translation ) *
               glm::rotate( glm::mat4( 1.0F ), glm::radians( degrees ), axis );
    }

    glm::mat4 ShoulderLocalBind()
    {
        // A ROTATION AS WELL AS A TRANSLATION on every bone, for the reason А2's rig exists: a bind that is
        // pure translation leaves the rotational half of the bind pose untested, and an offset matrix that
        // is nearly the identity hides every confusion between a bone's own space and skinning space.
        return Place( { 0.0F, 150.0F, 0.0F }, 20.0F, { 0.0F, 0.0F, 1.0F } );
    }
    glm::mat4 ElbowLocalBind()
    {
        return Place( { 0.0F, kUpperLimbCm, 0.0F }, -25.0F, { 1.0F, 0.0F, 0.0F } );
    }
    glm::mat4 HandLocalBind()
    {
        return Place( { 0.0F, kLowerLimbCm, 0.0F }, 15.0F, { 0.0F, 1.0F, 0.0F } );
    }
    glm::mat4 PostLocalBind()
    {
        return glm::translate( glm::mat4( 1.0F ), glm::vec3( kGoalCm, 150.0F, 0.0F ) );
    }
    glm::mat4 KerbLocalBind()
    {
        return glm::translate( glm::mat4( 1.0F ), glm::vec3( 190.0F, 150.0F, 0.0F ) );
    }

    std::vector<Desert::Animation::BoneInfo> ProbeBones()
    {
        const glm::mat4 shoulderGlobal = ShoulderLocalBind();
        const glm::mat4 elbowGlobal    = shoulderGlobal * ElbowLocalBind();
        const glm::mat4 handGlobal     = elbowGlobal * HandLocalBind();

        std::vector<Desert::Animation::BoneInfo> bones( 5 );

        bones[0].Name               = kShoulder;
        bones[0].LocalBindTransform = ShoulderLocalBind();
        bones[0].OffsetMatrix       = glm::inverse( shoulderGlobal );

        bones[1].Name               = kElbow;
        bones[1].ParentBoneID       = 0U;
        bones[1].LocalBindTransform = ElbowLocalBind();
        bones[1].OffsetMatrix       = glm::inverse( elbowGlobal );

        bones[2].Name               = kHand;
        bones[2].ParentBoneID       = 1U;
        bones[2].LocalBindTransform = HandLocalBind();
        bones[2].OffsetMatrix       = glm::inverse( handGlobal );

        bones[3].Name               = kPost;
        bones[3].LocalBindTransform = PostLocalBind();
        bones[3].OffsetMatrix       = glm::inverse( PostLocalBind() );

        bones[4].Name               = kKerb;
        bones[4].LocalBindTransform = KerbLocalBind();
        bones[4].OffsetMatrix       = glm::inverse( KerbLocalBind() );

        return bones;
    }

    // ---------------------------------------------------------------- reading what was shipped

    Desert::Assets::Serialization::SkeletonAssetData LoadSkeletonData()
    {
        const std::string raw = ReadFile( RepoRoot() + kCookedDir + "IKProbe.skeleton" );
        EXPECT_FALSE( raw.empty() ) << "could not read IKProbe.skeleton";
        auto data = Common::Json::Read<Desert::Assets::Serialization::SkeletonAssetData>( raw );
        EXPECT_TRUE( data.IsSuccess() );
        return data.IsSuccess() ? data.GetValue() : Desert::Assets::Serialization::SkeletonAssetData{};
    }

    Desert::Assets::Serialization::MeshAssetData LoadMeshData()
    {
        const std::string raw = ReadFile( RepoRoot() + kCookedDir + "IKProbe.skmesh" );
        EXPECT_FALSE( raw.empty() ) << "could not read IKProbe.skmesh";
        // Through the engine's own reader (B11): a cooked mesh is a binary container, and a suite that
        // parsed the fixture as JSON would be reading it by a route the engine does not take.
        auto data = Desert::Assets::Serialization::ReadMeshAssetData( raw, "IKProbe.skmesh" );
        EXPECT_TRUE( data.IsSuccess() ) << ( data.IsSuccess() ? std::string{} : data.GetError() );
        return data.IsSuccess() ? data.GetValue() : Desert::Assets::Serialization::MeshAssetData{};
    }

    Desert::Animation::AnimationClip LoadClip()
    {
        const std::string raw = ReadFile( RepoRoot() + kCookedDir + "IKProbe_Swing.anim" );
        EXPECT_FALSE( raw.empty() ) << "could not read IKProbe_Swing.anim";
        auto data = Common::Json::Read<Desert::Assets::Serialization::AnimationAssetData>( raw );
        EXPECT_TRUE( data.IsSuccess() );
        if ( !data.IsSuccess() )
        {
            return {};
        }
        auto built = Desert::Assets::Serialization::BuildClipFromAssetData( data.GetValue() );
        EXPECT_TRUE( built.IsSuccess() ) << ( built.IsSuccess() ? "" : built.GetError() );
        return built.IsSuccess() ? built.ExtractValue() : Desert::Animation::AnimationClip{};
    }

    Desert::Animation::Skeleton RigFromFile()
    {
        auto bones = LoadSkeletonData().Bones;
        return Desert::Animation::Skeleton( std::move( bones ) );
    }

    /// The bone's position in the rig AT REST, or the origin when the rig has no such bone (which the
    /// caller's EXPECT has already reported). `value_or` rather than `*`: the analyser cannot see that
    /// gtest's EXPECT_TRUE guards the dereference, and it is right not to — EXPECT does not return.
    glm::vec3 BindPositionOf( const Desert::Animation::Skeleton& rig, const char* name )
    {
        const auto index = rig.FindBoneIndex( name );
        EXPECT_TRUE( index.has_value() ) << "the rig has no bone '" << name << "'";

        std::vector<glm::mat4> global;
        rig.ResolveComponentSpace( [&rig]( std::uint32_t bone )
                                   { return rig.GetBones()[bone].LocalBindTransform; }, global );
        const std::uint32_t bone = index.value_or( 0U );
        return index.has_value() ? glm::vec3( global[bone][3] ) : glm::vec3( 0.0F );
    }
} // namespace

// ---------------------------------------------------------------- the construction IS the bytes

TEST( IKProbeRig, TheShippedRigIsTheChainThisSuiteDescribes )
{
    ASSERT_FALSE( RepoRoot().empty() ) << "could not locate the repository root from the working directory";

    const auto data     = LoadSkeletonData();
    const auto expected = ProbeBones();

    ASSERT_EQ( data.Bones.size(), expected.size() );
    for ( std::size_t i = 0; i < expected.size(); ++i )
    {
        EXPECT_EQ( data.Bones[i].Name, expected[i].Name );
        EXPECT_EQ( data.Bones[i].ParentBoneID, expected[i].ParentBoneID );
        for ( int c = 0; c < 4; ++c )
        {
            for ( int r = 0; r < 4; ++r )
            {
                EXPECT_NEAR( data.Bones[i].LocalBindTransform[c][r], expected[i].LocalBindTransform[c][r], 1e-3F )
                     << "bone " << expected[i].Name << " bind [" << c << "][" << r << "]";
                EXPECT_NEAR( data.Bones[i].OffsetMatrix[c][r], expected[i].OffsetMatrix[c][r], 1e-3F )
                     << "bone " << expected[i].Name << " offset [" << c << "][" << r << "]";
            }
        }
    }

    // ONE identity, derived twice and written into four files no compiler reads.
    const std::uint64_t signature = Desert::Animation::Skeleton::ComputeSignature( data.Bones );
    EXPECT_EQ( data.Signature, signature );
    EXPECT_EQ( LoadMeshData().SkeletonSignature.value_or( 0ULL ), signature );
    EXPECT_EQ( LoadClip().SkeletonSignature, signature )
         << "IKProbe_Swing claims a different rig from IKProbe.skeleton. Every frame taken against it would "
            "show a bind pose and still render, which is broken evidence rather than no evidence.";

    // SCNE 28: the two shipped scenes name the mesh by the GUID its own header states -- read from the
    // file, not pinned, so re-cooking the mesh with a new GUID fails here until the scenes follow.
    const auto meshGuid =
         Common::Content::ReadMeshHeaderGuid( ReadFile( RepoRoot() + kCookedDir + "IKProbe.skmesh" ) );
    ASSERT_TRUE( meshGuid.has_value() ) << "IKProbe.skmesh states no header GUID (not a v3 mesh)";
    const std::string meshGuidText = Common::Content::AssetGuidToText( *meshGuid );
    for ( const char* scene : { kWitness, kNoControl } )
    {
        const std::string text = ReadFile( RepoRoot() + scene );
        ASSERT_FALSE( text.empty() ) << "could not read " << scene;
        EXPECT_NE( text.find( R"("MeshGuid": ")" + meshGuidText + "\"" ), std::string::npos )
             << scene << " does not name the probe mesh by its header GUID " << meshGuidText
             << ", so the scene that places the rig would resolve to no mesh at all.";
    }
}

// ---------------------------------------------------------------- the instrument must not be degenerate

TEST( IKProbeRig, TheChainIsThreeDeepAndTheTwoControlBonesAreOutsideIt )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto  rig   = RigFromFile();
    const auto& bones = rig.GetBones();
    ASSERT_EQ( bones.size(), 5U );
    EXPECT_TRUE( rig.GetStructureError().empty() ) << rig.GetStructureError();

    // THE WHOLE REASON THIS RIG EXISTS. A two-bone chain is the named bone, its parent AND its grandparent;
    // TwoBoneProbe stops one short, and this is the assertion that keeps IKProbe from drifting back to it.
    const auto handIndex = rig.FindBoneIndex( kHand );
    ASSERT_TRUE( handIndex.has_value() );
    const std::uint32_t elbow = rig.ResolveParent( handIndex.value_or( 0U ) );
    ASSERT_NE( elbow, Desert::Animation::Skeleton::NO_PARENT );
    const std::uint32_t shoulder = rig.ResolveParent( elbow );
    ASSERT_NE( shoulder, Desert::Animation::Skeleton::NO_PARENT );
    EXPECT_EQ( rig.ResolveParent( shoulder ), Desert::Animation::Skeleton::NO_PARENT );
    EXPECT_EQ( bones[elbow].Name, kElbow );
    EXPECT_EQ( bones[shoulder].Name, kShoulder );

    // Both control bones are ROOTS, so nothing the solver writes can reach them through a parent chain.
    for ( const char* name : { kPost, kKerb } )
    {
        const auto index = rig.FindBoneIndex( name );
        ASSERT_TRUE( index.has_value() );
        EXPECT_EQ( rig.ResolveParent( index.value_or( 0U ) ), Desert::Animation::Skeleton::NO_PARENT )
             << name
             << " is no longer a root, so it would follow whatever the solver does to its ancestor "
                "and would stop being a control.";
    }

    // No bone may be named like the one-bone corpus or the foreign negative control: ClipSkeletonMatch
    // falls back to >= 50 % bone-NAME overlap when signatures disagree, so a rig named "Root" or "Hips"
    // gets offered their clips and two instruments collapse into one. (А2's note, and it is still true.)
    for ( const auto& bone : bones )
    {
        EXPECT_NE( bone.Name, "Root" );
        EXPECT_NE( bone.Name, "Hips" );
        EXPECT_NE( bone.Name, "Base" );
        EXPECT_NE( bone.Name, "Arm" );
        EXPECT_NE( bone.OffsetMatrix, glm::mat4( 1.0F ) )
             << bone.Name
             << " has an IDENTITY OffsetMatrix, which makes its own space and skinning space "
                "the same space and the rig blind to every defect that confuses them.";
    }

    // Every bone carries geometry, or a bone that stopped being solved would leave nothing to see.
    const auto mesh = LoadMeshData();
    ASSERT_FALSE( mesh.SkinnedVertices.empty() );
    std::vector<int> influences( bones.size(), 0 );
    for ( const auto& vertex : mesh.SkinnedVertices )
    {
        ASSERT_LT( vertex.BoneIDs[0], bones.size() );
        ++influences[vertex.BoneIDs[0]];
    }
    for ( std::size_t i = 0; i < bones.size(); ++i )
    {
        EXPECT_GT( influences[i], 0 ) << "bone " << bones[i].Name
                                      << " skins no vertex, so a frame cannot "
                                         "show whether it moved";
    }
}

// ---------------------------------------------------------------- the four agreements the shots rest on

TEST( IKProbeRig, TheScenesGoalIsThePostBonesPositionAndItIsInsideTheReach )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto      rig      = RigFromFile();
    const glm::vec3 shoulder = BindPositionOf( rig, kShoulder );
    const glm::vec3 elbow    = BindPositionOf( rig, kElbow );
    const glm::vec3 hand     = BindPositionOf( rig, kHand );
    const glm::vec3 post     = BindPositionOf( rig, kPost );
    const glm::vec3 kerb     = BindPositionOf( rig, kKerb );

    const float upper = glm::length( elbow - shoulder );
    const float lower = glm::length( hand - elbow );
    EXPECT_NEAR( upper, kUpperLimbCm, 1e-3F );
    EXPECT_NEAR( lower, kLowerLimbCm, 1e-3F );

    // "THE HAND ARRIVED AT THE POST" is only a statement about IK if the post IS the goal. The scene stores
    // the goal as three numbers and the rig stores the post as a bind transform; nothing but this line makes
    // the two agree, and a frame showing the hand beside the post would look like a solver bug either way.
    const std::string scene = ReadFile( RepoRoot() + kWitness );
    ASSERT_FALSE( scene.empty() );
    EXPECT_NE( scene.find( R"("Goal": [90.0, 150.0, 0.0])" ), std::string::npos )
         << "the witness scene's authored goal is not the post bone's position any more.";
    EXPECT_NE( scene.find( std::string( R"("EndBone": ")" ) + kHand + "\"" ), std::string::npos );
    EXPECT_NEAR( glm::length( post - glm::vec3( 90.0F, 150.0F, 0.0F ) ), 0.0F, 1e-3F );

    // "REACHED, NOT CLAMPED": strictly inside the shell, with room either side, so the shots show the
    // reachable branch of the solver and not one of its two clamps.
    const float goalDistance = glm::length( post - shoulder );
    EXPECT_NEAR( goalDistance, kGoalCm, 1e-3F );
    EXPECT_GT( goalDistance, std::fabs( upper - lower ) + 10.0F );
    EXPECT_LT( goalDistance, upper + lower - 10.0F );

    // "THE KERB RECTANGLE IS ZERO PIXELS" needs the kerb to be UNREACHABLE. Without this the negative
    // control would only be saying "the arm happened not to go there this frame".
    const float kerbDistance = glm::length( kerb - shoulder );
    EXPECT_GT( kerbDistance, upper + lower )
         << "the kerb is " << kerbDistance << " cm from the chain's root and the chain reaches "
         << ( upper + lower )
         << " cm, so some pose of the arm could touch it and the rectangle around it "
            "would stop being a negative control.";
}

// ---------------------------------------------------------------- end to end, from the shipped bytes

TEST( IKProbeRig, TheShippedSolverReachesTheShippedGoalFromTheShippedClip )
{
    ASSERT_FALSE( RepoRoot().empty() );

    const auto rig  = RigFromFile();
    const auto clip = LoadClip();
    ASSERT_FALSE( clip.Tracks.empty() );

    const auto handIndex = rig.FindBoneIndex( kHand );
    const auto kerbIndex = rig.FindBoneIndex( kKerb );
    ASSERT_TRUE( handIndex.has_value() );
    ASSERT_TRUE( kerbIndex.has_value() );
    const std::uint32_t hand = handIndex.value_or( 0U );
    const std::uint32_t kerb = kerbIndex.value_or( 0U );

    const glm::vec3 goal( 90.0F, 150.0F, 0.0F );

    // The same 1/60 s the headless `--play` advances by, and the same number of steps as `--shot-frames 90`,
    // so the pose this test measures is the pose the committed shots were taken of.
    const auto advance = []( Desert::Animation::Animator& animator )
    {
        for ( int frame = 0; frame < 90; ++frame )
        {
            animator.Update( Common::Timestep( 1.0F / 60.0F ) );
        }
    };

    Desert::Animation::Animator plain( rig );
    plain.Play( clip, true );
    advance( plain );

    Desert::Animation::Animator solved( rig );
    solved.Play( clip, true );
    {
        auto control = std::make_unique<Desert::Animation::TwoBoneIKControl>();
        control->SetEndBone( kHand );
        control->SetGoal( goal );
        control->SetPoleTarget( glm::vec3( 45.0F, 250.0F, 0.0F ) );
        solved.AddControl( std::move( control ) );
    }
    advance( solved );

    const auto* control = dynamic_cast<const Desert::Animation::TwoBoneIKControl*>( solved.GetControl( 0 ) );
    ASSERT_NE( control, nullptr );
    EXPECT_TRUE( control->GetLastError().empty() ) << control->GetLastError();
    EXPECT_EQ( control->GetLastReach(), Desert::Animation::Solvers::TwoBoneIKReach::Reached );
    EXPECT_EQ( control->GetLastPlane(), Desert::Animation::Solvers::TwoBoneIKPlane::FromPoleTarget );

    const glm::vec3 solvedHand = glm::vec3( solved.GetBoneModelMatrix( hand )[3] );
    const glm::vec3 plainHand  = glm::vec3( plain.GetBoneModelMatrix( hand )[3] );

    EXPECT_NEAR( glm::length( solvedHand - goal ), 0.0F, 0.05F )
         << "the hand ended " << glm::length( solvedHand - goal ) << " cm from the post";

    // AND THE ANIMATION WAS ACTUALLY FIGHTING IT. If the clip happened to put the hand on the goal by
    // itself, the shots would be a picture of nothing, and every number in them would still look right.
    EXPECT_GT( glm::length( plainHand - goal ), 50.0F )
         << "the clip's own pose is only " << glm::length( plainHand - goal )
         << " cm from the goal, so the shots cannot show the solver doing anything";

    // The control bone is untouched in SKINNING SPACE, which is the space the frame is made from.
    EXPECT_EQ( plain.GetPose().Matrices[kerb], solved.GetPose().Matrices[kerb] );
}

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
