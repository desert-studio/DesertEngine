// "An edit of a .retarget chain undoes, Auto-map fills the target bones the runtime then resolves, and what
// the window saves reads back equal." Unit under test: Editor/Panels/Retarget/RetargetDocumentModel.cpp, over
// the shipped ForeignArm -> IKProbe retarget and its two real skeletons (Desert/Tests/Data).
#include <gtest/gtest.h>

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Panels/Retarget/RetargetDocumentModel.hpp>

#include <Engine/Animation/BoneInfo.hpp>
#include <Engine/Animation/Skeleton.hpp>
#include <Engine/Assets/Serialization/Retarget.hpp>
#include <Engine/Assets/Serialization/Skeleton.hpp>

#include <Common/Core/Serialization/GlmReflection.hpp>
#include <Common/Json/Json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../TestSupport/scratch_dir.hpp"

namespace
{
    namespace File = Desert::Assets::Serialization;
    using Desert::Animation::BoneInfo;
    using Desert::Animation::Skeleton;
    using Desert::Editor::CommandHistory;
    using Desert::Editor::RetargetDocumentModel;
    using Desert::TestSupport::TestDataDir;

    std::string ReadFile( const std::filesystem::path& path )
    {
        const std::ifstream in( path, std::ios::binary );
        std::ostringstream  ss;
        ss << in.rdbuf();
        return ss.str();
    }

    Skeleton RigFrom( const char* path )
    {
        auto data = Common::Json::Read<File::SkeletonAssetData>( ReadFile( TestDataDir() / path ) );
        EXPECT_TRUE( data.IsSuccess() ) << ( data.IsSuccess() ? "" : data.GetError() );
        Skeleton rig( data.IsSuccess() ? data.ExtractValue().Bones : std::vector<BoneInfo>{} );
        rig.RecomputeOffsetMatrices();
        return rig;
    }

    File::RetargetAssetData Shipped()
    {
        auto parsed = File::ParseRetarget(
             ReadFile( TestDataDir() / "Resources/Assets/Retargets/ForeignArm_To_IKProbe.retarget" ) );
        EXPECT_TRUE( parsed.IsSuccess() ) << ( parsed.IsSuccess() ? "" : parsed.GetError() );
        return parsed.IsSuccess() ? parsed.ExtractValue() : File::RetargetAssetData{};
    }
} // namespace

TEST( RetargetDocument, EditUndoAutoMapSaveReadsBackEqual )
{
    const Skeleton source = RigFrom( "Resources/Assets/Meshes/Skinned/ForeignArm.skeleton" );
    const Skeleton target = RigFrom( "Resources/Assets/Meshes/Skinned/IKProbe.skeleton" );
    CommandHistory history;
    const File::RetargetAssetData shipped = Shipped();
    ASSERT_EQ( shipped.Chains.size(), 1U );

    RetargetDocumentModel model( shipped, history );
    ASSERT_TRUE( model.Validate( source, target ).IsSuccess() ) << model.Validate( source, target ).GetError();
    EXPECT_FALSE( model.IsDirty() );

    // An edit that breaks the chain: the runtime's resolver says so, per chain.
    auto chain            = model.GetData().Chains[0];
    chain.TargetStartBone = "";
    chain.TargetEndBone   = "IK_Post";
    ASSERT_TRUE( model.EditChain( 0, chain ).IsSuccess() );
    EXPECT_TRUE( model.IsDirty() );
    EXPECT_FALSE( model.ChainProblems( source, target )[0].empty() );

    // One undo puts the shipped chain back, whole.
    ASSERT_TRUE( history.Undo() );
    EXPECT_EQ( model.GetData(), shipped );
    EXPECT_FALSE( model.IsDirty() );
    ASSERT_TRUE( history.Redo() );
    EXPECT_EQ( model.GetData().Chains[0].TargetEndBone, "IK_Post" );

    // Auto-map: IK_Shoulder by name, Foreign_Hand -> IK_Hand by the file's BoneRenames row.
    const auto mapped = model.AutoMap( source, target );
    ASSERT_TRUE( mapped.IsSuccess() ) << mapped.GetError();
    EXPECT_EQ( mapped.GetValue(), 1U );
    EXPECT_EQ( model.GetData().Chains[0].TargetStartBone, "IK_Shoulder" );
    EXPECT_EQ( model.GetData().Chains[0].TargetEndBone, "IK_Hand" );
    EXPECT_TRUE( model.ChainProblems( source, target )[0].empty() ) << model.ChainProblems( source, target )[0];
    EXPECT_TRUE( model.Validate( source, target ).IsSuccess() );

    // A duplicate name is refused and changes nothing.
    ASSERT_TRUE( model.AddChain( "Extra" ).IsSuccess() );
    EXPECT_FALSE( model.AddChain( "Arm" ).IsSuccess() );
    ASSERT_TRUE( model.RemoveChain( 1 ).IsSuccess() );

    // Save -> read back equal; the saved state is clean.
    const Desert::TestSupport::ScratchDir scratch( "RetargetDocument" );
    const auto                            path = scratch.Path() / "Edited.retarget";
    ASSERT_TRUE( model.Save( path ).IsSuccess() );
    EXPECT_FALSE( model.IsDirty() );
    auto reread = File::LoadRetargetFile( path );
    ASSERT_TRUE( reread.IsSuccess() ) << reread.GetError();
    auto written = model.GetData();
    auto back    = reread.ExtractValue();
    written.Header.reset();
    back.Header.reset();
    EXPECT_EQ( back, written );
}

TEST( RetargetDocument, ClosingDropsItsUndoRecords )
{
    CommandHistory history;
    {
        RetargetDocumentModel model( Shipped(), history );
        ASSERT_TRUE( model.AddChain( "Extra" ).IsSuccess() );
        EXPECT_EQ( history.UndoStack().size(), 1U );
    }
    EXPECT_TRUE( history.UndoStack().empty() );
    EXPECT_FALSE( history.Undo() );
}
