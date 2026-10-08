// The Physics Asset editor's renderer-free half (RAG1c): one undo record per committed edit, the save through the
// asset serializer, and Simulate in a private PhysicsWorld that is never the scene's.

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Panels/PhysicsAssetEditor/PhysicsAssetEdit.hpp>

#include <Engine/Animation/Animator.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Physics/PhysicsAssetFormat.hpp>
#include <Engine/Physics/PhysicsWorld.hpp>

#include <gtest/gtest.h>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

using namespace Desert;
using Desert::Animation::BoneInfo;
using Desert::Animation::Skeleton;
using Desert::Editor::CommandHistory;

namespace
{
    BoneInfo MakeBone( const char* name, std::optional<uint32_t> parent, const glm::vec3& offset )
    {
        BoneInfo b;
        b.Name               = name;
        b.ParentBoneID       = parent;
        b.LocalBindTransform = glm::translate( glm::mat4( 1.0f ), offset );
        return b;
    }

    // pelvis at y 100, spine 30 cm above it; Y up, centimetres.
    Skeleton MakeRig()
    {
        std::vector<BoneInfo> bones;
        bones.push_back( MakeBone( "pelvis", std::nullopt, { 0.0f, 100.0f, 0.0f } ) );
        bones.push_back( MakeBone( "spine", 0u, { 0.0f, 30.0f, 0.0f } ) );
        return Skeleton( std::move( bones ) );
    }

    Physics::PhysicsAssetData MakeAsset()
    {
        const glm::quat zToY = glm::angleAxis( -glm::half_pi<float>(), glm::vec3( 1.0f, 0.0f, 0.0f ) );
        const glm::quat xToY = glm::angleAxis( glm::half_pi<float>(), glm::vec3( 0.0f, 0.0f, 1.0f ) );

        Physics::PhysicsAssetData asset;
        asset.Skeleton = { 0xA11Cull, 0x0004ull };
        for ( const char* bone : { "pelvis", "spine" } )
        {
            Physics::PhysicsAssetBody body;
            body.Bone     = bone;
            body.Shape    = Physics::PhysicsBodyShape::Capsule;
            body.Center   = { 0.0f, 15.0f, 0.0f };
            body.Rotation = zToY;
            body.Radius   = 6.0f;
            body.Length   = 14.0f;
            asset.Bodies.push_back( body );
        }
        Physics::PhysicsAssetConstraint joint;
        joint.ParentBone = "pelvis";
        joint.ChildBone  = "spine";
        joint.Rotation   = xToY;
        asset.Constraints.push_back( joint );
        return asset;
    }

    std::vector<unsigned char> ReadBytes( const std::filesystem::path& path )
    {
        std::ifstream in( path, std::ios::binary );
        return { std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
    }
} // namespace

TEST( PhysicsAssetEditor, LimitEditIsOneUndoStepAndUndoRestoresIt )
{
    CommandHistory::Get().Clear();
    auto       data     = std::make_shared<Physics::PhysicsAssetData>( MakeAsset() );
    const auto original = *data;

    ASSERT_TRUE( Editor::CommitJointLimits( data, "spine", 30.0f, 20.0f, 10.0f ).IsSuccess() );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), 1u ) << "one committed edit must be one record";
    EXPECT_FLOAT_EQ( data->Constraints[0].Swing1LimitDegrees, 30.0f );
    EXPECT_FLOAT_EQ( data->Constraints[0].Swing2LimitDegrees, 20.0f );
    EXPECT_FLOAT_EQ( data->Constraints[0].TwistLimitDegrees, 10.0f );

    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_EQ( *data, original ) << "undo did not restore the authored limits";
    ASSERT_TRUE( CommandHistory::Get().Redo() );
    EXPECT_FLOAT_EQ( data->Constraints[0].TwistLimitDegrees, 10.0f );

    // Refusals leave no record: an unknown joint, a limit out of range, a body that does not exist.
    const std::size_t records = CommandHistory::Get().UndoStack().size();
    EXPECT_FALSE( Editor::CommitJointLimits( data, "head", 10.0f, 10.0f, 10.0f ).IsSuccess() );
    EXPECT_FALSE( Editor::CommitJointLimits( data, "spine", 10.0f, 190.0f, 10.0f ).IsSuccess() );
    Physics::PhysicsAssetBody ghost;
    ghost.Bone = "head";
    EXPECT_FALSE( Editor::CommitBody( data, ghost ).IsSuccess() );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), records );

    // A body edit is one record too, and undo puts the old radius back.
    Physics::PhysicsAssetBody fatter = data->Bodies[1];
    fatter.Radius                    = 9.0f;
    ASSERT_TRUE( Editor::CommitBody( data, fatter ).IsSuccess() );
    EXPECT_EQ( CommandHistory::Get().UndoStack().size(), records + 1 );
    ASSERT_TRUE( CommandHistory::Get().Undo() );
    EXPECT_FLOAT_EQ( data->Bodies[1].Radius, 6.0f );
    CommandHistory::Get().Clear();
}

TEST( PhysicsAssetEditor, SaveRoundTripsThroughTheAssetSerializerAndKeepsTheFileGuid )
{
    const auto dir = std::filesystem::temp_directory_path() / "DesertPhysicsAssetEditorTest";
    std::filesystem::remove_all( dir );
    const auto path = dir / "Ragdoll.dephysasset";

    Physics::PhysicsAssetData edited        = MakeAsset();
    edited.Constraints[0].TwistLimitDegrees = 12.5f;
    ASSERT_TRUE( Editor::SavePhysicsAssetDocument( path, edited ).IsSuccess() );

    const auto first = Physics::DecodePhysicsAsset( ReadBytes( path ) );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    EXPECT_EQ( first.GetValue().Skeleton, edited.Skeleton );
    EXPECT_EQ( first.GetValue().Bodies, edited.Bodies );
    EXPECT_EQ( first.GetValue().Constraints, edited.Constraints );

    // The second save of the loaded data writes the same bytes and keeps the file's identity.
    const auto bytes = ReadBytes( path );
    ASSERT_TRUE( Editor::SavePhysicsAssetDocument( path, first.GetValue() ).IsSuccess() );
    EXPECT_EQ( ReadBytes( path ), bytes );
    std::filesystem::remove_all( dir );
}

TEST( PhysicsAssetEditor, SimulateRunsInAPrivateWorldAndStopResets )
{
    const Skeleton            rig   = MakeRig();
    const auto                asset = MakeAsset();
    const Animation::Animator animator( rig );

    Physics::PhysicsWorld scene; // the level's world: Simulate must never touch it
    ASSERT_TRUE( scene.Init( 981.0f ) );

    Editor::PhysicsAssetPreviewSimulation sim;
    const auto                            started = sim.Start( asset, rig, animator.GetLocalPose() );
    ASSERT_TRUE( started.IsSuccess() ) << started.GetError();
    ASSERT_NE( sim.World(), nullptr );
    EXPECT_NE( sim.World(), &scene );
    EXPECT_EQ( sim.World()->GetRagdollCount(), 1u );
    EXPECT_EQ( scene.GetRagdollCount(), 0u ) << "the preview ragdoll landed in the scene's world";
    EXPECT_EQ( scene.GetBodyCount(), 0u );

    ASSERT_EQ( sim.Parts().size(), 2u );
    const float startY = sim.Parts()[0].Position.y;
    EXPECT_NEAR( startY, 100.0f, 1e-2f );

    for ( int i = 0; i < 60; ++i )
        sim.Step( 1.0f / 60.0f );
    EXPECT_LT( sim.Parts()[0].Position.y, startY - 20.0f ) << "the ragdoll did not fall";
    EXPECT_TRUE( sim.Overrides( rig, animator.GetLocalPose() ).IsSuccess() );
    EXPECT_EQ( scene.GetBodyCount(), 0u );

    sim.Stop();
    EXPECT_FALSE( sim.IsRunning() );
    EXPECT_EQ( sim.World(), nullptr );
    EXPECT_TRUE( sim.Parts().empty() );
    EXPECT_FALSE( sim.Overrides( rig, animator.GetLocalPose() ).IsSuccess() );

    // Every Start begins from the pose, not from where the last run fell.
    ASSERT_TRUE( sim.Start( asset, rig, animator.GetLocalPose() ).IsSuccess() );
    EXPECT_NEAR( sim.Parts()[0].Position.y, 100.0f, 1e-2f );
}
