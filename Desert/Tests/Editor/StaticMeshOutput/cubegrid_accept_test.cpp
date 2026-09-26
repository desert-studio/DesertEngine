// CG1: every Cube Grid Accept was refused ("mesh source LOD0 triangle 0 uses material slot 0, the asset has
// 0 slots") because the blockout's fresh StaticMeshComponent had no slot for the material its triangles use,
// and the tool bar had already ended the tool, so the refused piece stayed the tool's and the next grid's
// Cancel destroyed it. Pinned here on the editor's own inputs: a baked blockout, the slots a fresh component
// gets from ECS::SetEditableMesh (CoverMaterialIds), the write Output: Static Mesh makes, and the Accept /
// Cancel / tool-bar rules of BlockoutSession.hpp and ActiveToolBar.hpp.

#include <Editor/Import/StaticMeshOutput.hpp>
#include <Editor/Panels/ViewportPanel/Tools/ActiveToolBar.hpp>
#include <Editor/Panels/ViewportPanel/Tools/BlockoutSession.hpp>

#include <Common/Core/Constants.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/ECS/EditableMesh.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Geometry/VoxelBlockout.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Desert;
using MS = Editor::Core::ModelingState;

namespace
{
    class AcceptProject : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_Saved = Common::Constants::Path::CurrentProjectRoot();
            m_Dir   = fs::temp_directory_path() / ( "CubeGridAccept_" + std::to_string( std::random_device{}() ) );
            fs::remove_all( m_Dir );
            fs::create_directories( m_Dir / "Assets" );
            Common::Constants::Path::SetProjectRoot( m_Dir, "Assets" );
            Assets::ContentRegistry::ResetForTest();
            MS::Get().ActiveTool = MS::Tool::CubeGrid;
        }
        void TearDown() override
        {
            MS::Get().ActiveTool        = MS::Tool::None;
            MS::Get().ReqAccept         = false;
            MS::Get().ReqAcceptEndsTool = false;
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
            Assets::ContentRegistry::ResetForTest();
            std::error_code ec;
            fs::remove_all( m_Dir, ec );
        }

        Common::Constants::Path::ProjectRootState m_Saved;
        fs::path                                  m_Dir;
    };

    // The Output: Static Mesh write of a fresh two-block blockout, step for step as the editor makes it:
    // Bake -> FromRenderMesh (CubeGridTool::RegenMesh) -> the DynamicMesh the component holds -> its render
    // submeshes' material ids -> the slots SetEditableMesh gives a component that had none -> handle 0 is
    // an empty GUID (WriteEntityAsStaticMesh) -> WriteStaticMeshAsset.
    Common::BoolResultStr WriteFreshBlockout( const std::string& name )
    {
        Geometry::VoxelBlockout::Volume volume;
        volume.m_Unit                                                = 100.0f;
        volume.m_Cells[Geometry::VoxelBlockout::Pack( { 0, 0, 0 } )] = {};
        volume.m_Cells[Geometry::VoxelBlockout::Pack( { 1, 0, 0 } )] = {};
        auto edit                                                    = Geometry::FromRenderMesh( volume.Bake() );
        if ( !edit.IsSuccess() )
            return Common::MakeError<bool>( edit.GetError() );
        auto dyn = Geometry::DynamicMeshFromSerialized( Geometry::ToSerialized( edit.GetValue().Mesh ), "CG1" );
        if ( !dyn.IsSuccess() )
            return Common::MakeError<bool>( dyn.GetError() );
        auto render = Geometry::ToRenderMesh( dyn.GetValue() );
        if ( !render.IsSuccess() )
            return Common::MakeError<bool>( render.GetError() );

        std::vector<Common::AssetHandle> slots; // a fresh StaticMeshComponent's
        (void)ECS::CoverMaterialIds( slots, render.GetValue().SubmeshMaterialIds );
        const std::vector<Common::Content::AssetGuid> guids( slots.size() );

        auto folder = Editor::StaticMeshOutputFolder( "Modeling" );
        if ( !folder.IsSuccess() )
            return Common::MakeError<bool>( folder.GetError() );
        auto written = Editor::WriteStaticMeshAsset( dyn.GetValue(), guids, folder.GetValue(), name );
        if ( !written.IsSuccess() )
            return Common::MakeError<bool>( written.GetError() );
        return Common::MakeSuccess( true );
    }
} // namespace

TEST_F( AcceptProject, AFreshBlockoutIsWrittenWithTheSlotItsTrianglesUse )
{
    auto written = WriteFreshBlockout( "Blockout" );
    EXPECT_TRUE( written.IsSuccess() ) << written.GetError();
}

TEST( CubeGridAcceptSlots, SlotsGrowToTheLargestIdAndNeverShrink )
{
    std::vector<Common::AssetHandle> slots;
    const std::vector<int>           ids = { 0, 2 };
    EXPECT_TRUE( ECS::CoverMaterialIds( slots, ids ) );
    ASSERT_EQ( slots.size(), 3u );
    const std::vector<int> fewer = { 0 };
    EXPECT_FALSE( ECS::CoverMaterialIds( slots, fewer ) );
    EXPECT_EQ( slots.size(), 3u );
}

TEST_F( AcceptProject, TheToolBarAcceptLeavesTheToolToEndItselfAndARefusalKeepsItOpen )
{
    MS& ms = MS::Get();
    Editor::Tools::PressToolBar( ms, /*hasCancel*/ true, /*finish*/ true, /*cancel*/ false );
    EXPECT_EQ( ms.ActiveTool, MS::Tool::CubeGrid );
    EXPECT_TRUE( ms.ReqAccept && ms.ReqAcceptEndsTool );

    Common::UUID              piece( 7001 );
    std::vector<Common::UUID> committed;
    auto                      accepted = Editor::Tools::AcceptBlockout(
         piece, ms, true, []( const Common::UUID& ) { return Common::MakeError<bool>( "the write refused" ); },
         [&]( const Common::UUID& p ) { committed.push_back( p ); } );
    EXPECT_FALSE( accepted.IsSuccess() );
    EXPECT_EQ( ms.ActiveTool, MS::Tool::CubeGrid ) << "a refused Accept must not end the tool";
    EXPECT_EQ( piece, Common::UUID( 7001 ) ) << "the refused piece stays the session's";
    EXPECT_TRUE( committed.empty() );
}

TEST_F( AcceptProject, CancelAfterAnAcceptLeavesTheAcceptedPiece )
{
    MS&                       ms = MS::Get();
    const Common::UUID        first( 8001 ), second( 8002 );
    std::set<Common::UUID>    scene = { first };
    std::vector<Common::UUID> committed;

    Common::UUID piece    = first;
    auto         accepted = Editor::Tools::AcceptBlockout(
         piece, ms, true, []( const Common::UUID& ) { return WriteFreshBlockout( "Blockout" ); },
         [&]( const Common::UUID& p ) { committed.push_back( p ); } );
    ASSERT_TRUE( accepted.IsSuccess() ) << accepted.GetError();
    EXPECT_EQ( ms.ActiveTool, MS::Tool::None ) << "the tool bar's Accept ends the tool once it succeeded";
    EXPECT_EQ( committed, std::vector<Common::UUID>{ first } );
    EXPECT_EQ( piece, Common::UUID::Null() );

    // The next grid, then Cancel: only the new piece goes.
    scene.insert( second );
    piece = second;
    Editor::Tools::CancelBlockout( piece, [&]( const Common::UUID& p ) { scene.erase( p ); } );
    EXPECT_EQ( scene, std::set<Common::UUID>{ first } );
}
