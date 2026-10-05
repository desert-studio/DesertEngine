// The Skeleton Editor's Reference Pose (ANIM-FIX4a): an edit of one bone's bind is ONE undo record, Save writes it
// into the `.skeleton`, a fresh load reads the same value back, and the file - not the shared asset - is what
// "Save*" compares against and "Don't Save" puts back.
#include <gtest/gtest.h>

#include <Editor/Core/Commands/SkeletonBindEdit.hpp>

#include <Engine/Assets/AssetManager.hpp>
#include <Engine/Assets/Mesh/SkeletonAsset.hpp>
#include <Engine/Assets/Mesh/SkeletonReferenceAssets.hpp>
#include <Engine/Assets/Mesh/AnimationAsset.hpp>
#include <Engine/Assets/Serialization/AnimationClipBuild.hpp>
#include <Engine/Assets/Serialization/AnimationClipWrite.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
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

    ASSERT_TRUE( Assets::Serialization::SaveSkeletonAsset( *rig, {} ).IsSuccess() );
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

// The document's Dirty state (AnimationEditorDocument::GetDiskState in Skeleton mode = BindPoseDiffers against the
// snapshot taken when the file was read): every way the bind moves turns it on, every way back turns it off.
TEST( SkeletonBindEdit, TheDocumentIsDirtyAfterAnEditAndCleanAfterUndoOrSave )
{
    const auto           file = WriteRig( TempDir( "dirty" ) );
    Assets::AssetManager manager;
    auto                 rig = manager.CreateAsset<Assets::SkeletonAsset>( Common::Filepath( file ) );
    ASSERT_TRUE( rig->Load().IsSuccess() );
    auto snapshot = Editor::ReadBindPoseOnDisk( *rig );
    ASSERT_TRUE( snapshot.IsSuccess() );
    EXPECT_FALSE( Editor::BindPoseDiffers( *rig, snapshot.GetValue() ) ) << "a freshly opened rig is clean";

    // A committed Details row (the Reference Pose Location X).
    ASSERT_TRUE( Editor::CommitBindEdit( rig, 1U, kEdited ) );
    EXPECT_TRUE( Editor::BindPoseDiffers( *rig, snapshot.GetValue() ) ) << "a row edit makes the tab Fox*";
    ASSERT_TRUE( Editor::CommandHistory::Get().Undo() );
    EXPECT_FALSE( Editor::BindPoseDiffers( *rig, snapshot.GetValue() ) ) << "undo back to the file is clean";
    ASSERT_TRUE( Editor::CommandHistory::Get().Redo() );
    EXPECT_TRUE( Editor::BindPoseDiffers( *rig, snapshot.GetValue() ) ) << "redo is dirty again";
    ASSERT_TRUE( Editor::CommandHistory::Get().Undo() );

    // A gizmo drag.
    Editor::BindPoseGesture gesture;
    gesture.Step( rig, 1U, true, nullptr );
    gesture.Step( rig, 1U, true, &kEdited );
    gesture.Step( rig, 1U, false, nullptr );
    EXPECT_TRUE( Editor::BindPoseDiffers( *rig, snapshot.GetValue() ) ) << "a gizmo drag makes the tab Fox*";

    // Save: the file now holds the bind, and the snapshot the document takes after it says clean.
    ASSERT_TRUE( Assets::Serialization::SaveSkeletonAsset( *rig, {} ).IsSuccess() );
    auto saved = Editor::ReadBindPoseOnDisk( *rig );
    ASSERT_TRUE( saved.IsSuccess() );
    EXPECT_FALSE( Editor::BindPoseDiffers( *rig, saved.GetValue() ) ) << "Save clears the marker";
    Editor::CommandHistory::Get().DropFor( rig.get() );
}

// Rename Bone (ANIM-FIX4b, UE Skeleton Editing): the name moves in memory as ONE undo record, and Save writes it
// to the .skeleton AND into this skeleton's clip - the clip's channel then finds the bone by its new name.
TEST( SkeletonBindEdit, ARenamedBoneIsSavedIntoTheSkeletonAndItsClip )
{
    const auto           dir  = TempDir( "rename" );
    const auto           file = WriteRig( dir );
    Assets::AssetManager manager;
    auto                 rig = manager.CreateAsset<Assets::SkeletonAsset>( Common::Filepath( file ) );
    ASSERT_TRUE( rig->Load().IsSuccess() );

    // A clip of this skeleton keying "Child" (a bone binding with a Transform track), written as a real .anim.
    Animation::AnimationClip clip;
    clip.AnimationName = "Wave";
    clip.Skeleton      = kRigGuid;
    clip.Sequence.End  = Animation::FrameNumber{ 30 };
    Animation::Timeline::Binding child;
    child.Guid    = Animation::Timeline::BindingGuid::ForObject( Animation::Timeline::BindingKind::Bone, "Child" );
    child.Kind    = Animation::Timeline::BindingKind::Bone;
    child.Locator = "Child";
    child.Label   = "Child";
    clip.Sequence.Bindings.push_back( child );
    const auto clipFile = dir / "wave.anim";
    ASSERT_TRUE( Assets::Serialization::SaveClipToFile( clipFile, clip ).IsSuccess() );

    // Refusals: no name, another bone's name - nothing moves, no record.
    EXPECT_FALSE( Editor::CommitBoneRename( rig, 1U, "" ).IsSuccess() );
    EXPECT_FALSE( Editor::CommitBoneRename( rig, 1U, "Root" ).IsSuccess() );
    EXPECT_FALSE( Editor::CommandHistory::Get().Undo() ) << "a refused rename is no record";

    const uint64_t revision = rig->GetBindRevision();
    ASSERT_TRUE( Editor::CommitBoneRename( rig, 1U, "Forearm" ).IsSuccess() );
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].Name, "Forearm" );
    EXPECT_EQ( rig->GetSkeleton()->FindBoneIndex( "Forearm" ), std::optional<uint32_t>( 1U ) );
    EXPECT_NE( rig->GetBindRevision(), revision ) << "the preview re-reads the rig";
    auto onDisk = Editor::ReadBindPoseOnDisk( *rig );
    ASSERT_TRUE( onDisk.IsSuccess() );
    EXPECT_TRUE( Editor::BindPoseDiffers( *rig, onDisk.GetValue() ) ) << "a rename is Save*";

    // Undo puts the name back, redo renames again: one record.
    ASSERT_TRUE( Editor::CommandHistory::Get().Undo() );
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].Name, "Child" );
    EXPECT_FALSE( Editor::BindPoseDiffers( *rig, onDisk.GetValue() ) );
    ASSERT_TRUE( Editor::CommandHistory::Get().Redo() );
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].Name, "Forearm" );

    // Save with the clip as this skeleton's referrer: both files speak the new name.
    Assets::SkeletonReferrers referrers;
    referrers.ClipFiles = { clipFile };
    ASSERT_TRUE( Assets::Serialization::SaveSkeletonAsset( *rig, referrers ).IsSuccess() );
    ASSERT_TRUE( rig->Unload().IsSuccess() );
    ASSERT_TRUE( rig->Load().IsSuccess() );
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].Name, "Forearm" ) << "the .skeleton holds the rename";
    EXPECT_EQ( rig->GetSkeleton()->GetBones()[1].ParentBoneID, std::optional<uint32_t>( 0U ) ) << "structure kept";

    auto clipAsset = manager.CreateAsset<Assets::AnimationAsset>( Common::Filepath( clipFile ) );
    ASSERT_TRUE( clipAsset->Load().IsSuccess() );
    const auto& bindings = clipAsset->GetClip().Sequence.Bindings;
    ASSERT_EQ( bindings.size(), 1U );
    EXPECT_EQ( bindings[0].Locator, "Forearm" ) << "the clip's channel names the bone by its new name";
    EXPECT_EQ( bindings[0].Guid, child.Guid ) << "the binding keeps its identity across the rename";
    EXPECT_EQ( rig->GetSkeleton()->FindBoneIndex( bindings[0].Locator ), std::optional<uint32_t>( 1U ) )
         << "the clip's channel finds the bone";
    Editor::CommandHistory::Get().DropFor( rig.get() );
}

// ANIM-FIX4b2: a retarget whose SOURCE rig is the renamed skeleton names its source bones by the new name after
// Save; its target side (the entity's own rig, not stated in the file) and a retarget of another rig are kept.
TEST( SkeletonBindEdit, ARenamedBoneIsSavedIntoTheRetargetsOfItsSkeleton )
{
    const auto           dir  = TempDir( "rename-retarget" );
    const auto           file = WriteRig( dir );
    Assets::AssetManager manager;
    auto                 rig = manager.CreateAsset<Assets::SkeletonAsset>( Common::Filepath( file ) );
    ASSERT_TRUE( rig->Load().IsSuccess() );

    const auto retargetOf = [&]( const Common::Content::AssetGuid& source, const char* name )
    {
        Assets::Serialization::RetargetAssetData data;
        data.Name                           = name;
        data.SourceSkeleton                 = { Common::Content::AssetGuidToText( source ), "bind.skeleton" };
        data.SourcePelvisBone               = "Root";
        data.TargetPelvisBone               = "Root";
        data.SourceRetargetPose.BoneOffsets = { { "Child", glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ) } };
        data.TargetRetargetPose.BoneOffsets = { { "Child", glm::quat( 1.0f, 0.0f, 0.0f, 0.0f ) } };
        data.Chains                         = { { "Arm", "Root", "Child", "Root", "Child", false } };
        data.BoneRenames                    = { { "Hand", "Child" } };
        const auto path                     = dir / ( std::string( name ) + ".retarget" );
        EXPECT_TRUE( Assets::Serialization::SaveRetargetFile( path, data ).IsSuccess() );
        return path;
    };
    const auto ours   = retargetOf( kRigGuid, "ours" );
    const auto theirs = retargetOf( Common::Content::AssetGuid{ 0x1ull, 0x2ull }, "theirs" );

    ASSERT_TRUE( Editor::CommitBoneRename( rig, 1U, "Forearm" ).IsSuccess() );
    Assets::SkeletonReferrers referrers;
    referrers.RetargetFiles = { ours, theirs };
    ASSERT_TRUE( Assets::Serialization::SaveSkeletonAsset( *rig, referrers ).IsSuccess() );

    const auto renamed = Assets::Serialization::LoadRetargetFile( ours );
    ASSERT_TRUE( renamed.IsSuccess() );
    const auto& r = renamed.GetValue();
    EXPECT_EQ( r.SourcePelvisBone, "Root" ) << "a bone that was not renamed keeps its name";
    ASSERT_EQ( r.Chains.size(), 1U );
    EXPECT_EQ( r.Chains[0].SourceStartBone, "Root" );
    EXPECT_EQ( r.Chains[0].SourceEndBone, "Forearm" ) << "the chain names the source bone by its new name";
    EXPECT_EQ( r.Chains[0].TargetEndBone, "Child" ) << "the target side is the entity's rig, not this one";
    ASSERT_EQ( r.SourceRetargetPose.BoneOffsets.size(), 1U );
    EXPECT_EQ( r.SourceRetargetPose.BoneOffsets[0].Bone, "Forearm" ) << "the source retarget pose follows";
    EXPECT_EQ( r.TargetRetargetPose.BoneOffsets[0].Bone, "Child" );
    ASSERT_EQ( r.BoneRenames.size(), 1U );
    EXPECT_EQ( r.BoneRenames[0].SourceBone, "Forearm" ) << "the target->source pair names the new source bone";
    EXPECT_EQ( r.BoneRenames[0].TargetBone, "Hand" );

    const auto other = Assets::Serialization::LoadRetargetFile( theirs );
    ASSERT_TRUE( other.IsSuccess() );
    EXPECT_EQ( other.GetValue().Chains[0].SourceEndBone, "Child" ) << "a retarget of another rig is not touched";
    Editor::CommandHistory::Get().DropFor( rig.get() );
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
