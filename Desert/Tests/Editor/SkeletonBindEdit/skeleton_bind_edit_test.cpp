// The Skeleton Editor's Reference Pose (ANIM-FIX4a): an edit of one bone's bind is ONE undo record, Save writes it
// into the `.skeleton`, a fresh load reads the same value back, and the file - not the shared asset - is what
// "Save*" compares against and "Don't Save" puts back.
#include <gtest/gtest.h>

#include <Editor/Core/Commands/SkeletonBindEdit.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonReferenceAssets.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Content/TextAssetHeader.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <fstream>

using namespace Desert;

namespace
{
    constexpr Common::Content::AssetGuid kRigGuid{ 0x5e1e7a0e7c1c7200ull, 0x00000000000b17d0ull };

    std::filesystem::path WriteRig( const std::filesystem::path& dir )
    {
        std::filesystem::create_directories( dir );
        Animation::BoneInfo root;
        root.Name = "Root";
        Animation::BoneInfo child;
        child.Name               = "Child";
        child.ParentBoneID       = 0U;
        child.LocalBindTransform = glm::translate( glm::mat4( 1.0f ), glm::vec3( 0.0f, 10.0f, 0.0f ) );
        Assets::Serialization::SkeletonAssetData data;
        data.Header    = Common::Content::MakeTextHeader( Common::Content::ContentKind::Skeleton, kRigGuid,
                                                          Assets::Serialization::SkeletonTextSubsystems() );
        data.Bones     = { root, child };
        data.Signature = Animation::Skeleton::ComputeSignature( data.Bones );
        const auto    path = dir / "bind.skeleton";
        std::ofstream out( path, std::ios::binary | std::ios::trunc );
        out << Assets::Serialization::WriteSkeletonJson( data );
        return path;
    }

    std::filesystem::path TempDir( const char* name )
    {
        return std::filesystem::temp_directory_path() / "SkeletonBindEdit" / name;
    }

    const glm::mat4 kEdited = glm::translate( glm::mat4( 1.0f ), glm::vec3( 3.5f, 12.25f, -2.0f ) );
} // namespace

TEST( SkeletonBindEdit, AnEditedBindIsSavedAndALoadReadsTheSameValue )
{
    const auto           file = WriteRig( TempDir( "saved" ) );
    Assets::AssetManager manager;
    auto                 rig = manager.CreateAsset<Assets::SkeletonAsset>( Common::Filepath( file ) );
    ASSERT_TRUE( rig->Load().IsSuccess() );
    const uint64_t signature = rig->GetSignature();

    ASSERT_TRUE( Editor::CommitBindEdit( rig, 1U, kEdited ) );
    auto onDisk = Editor::ReadBindPoseOnDisk( *rig );
    ASSERT_TRUE( onDisk.IsSuccess() );
    EXPECT_TRUE( Editor::BindPoseDiffers( *rig, onDisk.GetValue() ) ) << "an unsaved edit is Save*";

    ASSERT_TRUE( Assets::Serialization::SaveSkeletonAsset( *rig ).IsSuccess() );
    ASSERT_TRUE( rig->Unload().IsSuccess() );
    ASSERT_TRUE( rig->Load().IsSuccess() );
    ASSERT_NE( rig->GetSkeleton(), nullptr );
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].LocalBindTransform, kEdited ) << "the file holds the edit";
    EXPECT_EQ( rig->GetSignature(), signature ) << "a rest-pose edit is the same rig (meshes still match it)";
    auto reread = Editor::ReadBindPoseOnDisk( *rig );
    ASSERT_TRUE( reread.IsSuccess() );
    EXPECT_FALSE( Editor::BindPoseDiffers( *rig, reread.GetValue() ) ) << "saved = clean";
    Editor::CommandHistory::Get().DropFor( rig.get() );
}

TEST( SkeletonBindEdit, OneUndoPutsTheBindBackAndRedoReappliesIt )
{
    const auto           file = WriteRig( TempDir( "undo" ) );
    Assets::AssetManager manager;
    auto                 rig = manager.CreateAsset<Assets::SkeletonAsset>( Common::Filepath( file ) );
    ASSERT_TRUE( rig->Load().IsSuccess() );
    const glm::mat4 before = rig->GetSkeleton()->GetBones()[1].LocalBindTransform;

    // A gizmo drag of many frames is ONE record.
    Editor::BindPoseGesture gesture;
    const glm::mat4         halfway = glm::translate( glm::mat4( 1.0f ), glm::vec3( 1.0f, 11.0f, 0.0f ) );
    gesture.Step( rig, 1U, true, nullptr );
    gesture.Step( rig, 1U, true, &halfway );
    gesture.Step( rig, 1U, true, &kEdited );
    const uint64_t revision = rig->GetBindRevision();
    gesture.Step( rig, 1U, false, nullptr );
    EXPECT_FALSE( gesture.Active() );
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].LocalBindTransform, kEdited );

    ASSERT_TRUE( Editor::CommandHistory::Get().Undo() );
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].LocalBindTransform, before ) << "one undo = the whole drag";
    EXPECT_NE( rig->GetBindRevision(), revision ) << "the preview's Animator is told to re-read the rest pose";
    ASSERT_TRUE( Editor::CommandHistory::Get().Redo() );
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].LocalBindTransform, kEdited );
    Editor::CommandHistory::Get().DropFor( rig.get() );
}

TEST( SkeletonBindEdit, DontSavePutsTheFilesBindBackAndForgetsTheRecords )
{
    const auto           file = WriteRig( TempDir( "discard" ) );
    Assets::AssetManager manager;
    auto                 rig = manager.CreateAsset<Assets::SkeletonAsset>( Common::Filepath( file ) );
    ASSERT_TRUE( rig->Load().IsSuccess() );
    auto onDisk = Editor::ReadBindPoseOnDisk( *rig );
    ASSERT_TRUE( onDisk.IsSuccess() );

    ASSERT_TRUE( Editor::CommitBindEdit( rig, 1U, kEdited ) );
    EXPECT_FALSE( Editor::CommitBindEdit( rig, 1U, kEdited ) ) << "the same value is no second record";
    ASSERT_TRUE( Editor::RestoreBindPose( rig, onDisk.GetValue() ) );
    EXPECT_FALSE( Editor::BindPoseDiffers( *rig, onDisk.GetValue() ) );
    EXPECT_FALSE( Editor::CommandHistory::Get().Undo() ) << "the discarded edit's record went with it";
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
