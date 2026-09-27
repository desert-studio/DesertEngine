// M11: the Cube Grid tool reopens on a blockout it built. The relations pinned, on the editor's own path:
//  - the voxels a blockout carries survive the scene block (WriteCubeGridBlockout -> ReadCubeGridBlockout);
//  - the key of the mesh they baked to is the SAME whether the entity holds the EditMesh (Output: Dynamic) or
//    the .stmesh Output: Static Mesh wrote and a tool lifts back - otherwise every static-mesh blockout would be
//    refused as stale;
//  - reopen refuses, by reason, what it cannot edit, and a session never touches the state Cancel / undo
//    return to: that state reopens to the mesh it had.

#include <Editor/Import/MeshDeriver.hpp>
#include <Editor/Import/StaticMeshOutput.hpp>
#include <Editor/Panels/ViewportPanel/Tools/BlockoutSession.hpp>

#include <Common/Core/Constants.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/ECS/CubeGridBlockoutComponent.hpp>
#include <Engine/Geometry/DynamicMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshRenderConversion.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace Desert;
namespace VB = Geometry::VoxelBlockout;

namespace
{
    // The editor's first session: a 4x4 floor, a new marquee commits it, a 2x2 block on top.
    VB::Volume FloorWithBlock()
    {
        VB::Volume v;
        v.m_Unit = 100.0f;
        VB::WorkPlane floor;
        v.PushPull( floor, VB::Rect{ 0, 3, 0, 3 }, +1, 1, 0 );
        EXPECT_TRUE( v.Freeze() );
        v.m_Unit = 100.0f;
        VB::WorkPlane top{ 1, 1, 1 };
        v.PushPull( top, VB::Rect{ 1, 2, 1, 2 }, +1, 1, 0 );
        return v;
    }

    // RegenMesh's path: Bake -> FromRenderMesh -> the DynamicMesh the component holds.
    Geometry::DynamicMesh3 MeshOf( const VB::Volume& v )
    {
        auto edit = Geometry::FromRenderMesh( v.Bake() );
        EXPECT_TRUE( edit.IsSuccess() ) << edit.GetError();
        auto dyn = Geometry::DynamicMeshFromSerialized( Geometry::ToSerialized( edit.GetValue().Mesh ), "M11" );
        EXPECT_TRUE( dyn.IsSuccess() ) << dyn.GetError();
        return dyn.ExtractValue();
    }

    // The scene's round trip of the component block.
    std::optional<ECS::CubeGridBlockoutComponent> ThroughTheScene( const ECS::CubeGridBlockoutComponent& c,
                                                                   Common::Json::Issues&                 issues )
    {
        const Common::Json::Value block = ECS::WriteCubeGridBlockout( c );
        return ECS::ReadCubeGridBlockout( Common::Json::Node( block, Common::Json::Path{} ), issues );
    }

    class ReopenProject : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_Saved = Common::Constants::Path::CurrentProjectRoot();
            m_Dir   = fs::temp_directory_path() / ( "CubeGridReopen_" + std::to_string( std::random_device{}() ) );
            fs::remove_all( m_Dir );
            fs::create_directories( m_Dir / "Assets" );
            Common::Constants::Path::SetProjectRoot( m_Dir, "Assets" );
            Assets::ContentRegistry::ResetForTest();
        }
        void TearDown() override
        {
            Common::Constants::Path::SetProjectRoot( m_Saved.ProjectDir, m_Saved.AssetsRoot );
            Assets::ContentRegistry::ResetForTest();
            std::error_code ec;
            fs::remove_all( m_Dir, ec );
        }

        Common::Constants::Path::ProjectRootState m_Saved;
        fs::path                                  m_Dir;
    };
} // namespace

TEST_F( ReopenProject, AnAcceptedBlockoutReopensOnItsStaticMeshAsOnItsEditMesh )
{
    const VB::Volume             built = FloorWithBlock();
    const Geometry::DynamicMesh3 mesh  = MeshOf( built );
    const uint64_t               key   = Editor::Tools::MeshKeyOf( mesh );

    Common::Json::Issues issues;
    const auto read = ThroughTheScene( ECS::CubeGridBlockoutComponent{ VB::Save( built, key ) }, issues );
    ASSERT_TRUE( read.has_value() ) << ( issues.empty() ? "" : Common::Json::Describe( issues.front() ) );
    EXPECT_TRUE( issues.empty() );
    EXPECT_EQ( read->Saved.MeshKey, VB::Save( built, key ).MeshKey );
    ASSERT_EQ( read->Saved.Layers.size(), 2u );
    EXPECT_EQ( read->Saved.Layers[0].Flat, VB::Save( built, key ).Layers[0].Flat );

    // Output: Static Mesh writes the .stmesh; the tool reads it back through the lift (ModelingToolTarget).
    const std::vector<Common::Content::AssetGuid> guids( 1 );
    auto                                          folder = Editor::StaticMeshOutputFolder( "Modeling" );
    ASSERT_TRUE( folder.IsSuccess() ) << folder.GetError();
    auto written = Editor::WriteStaticMeshAsset( mesh, guids, folder.GetValue(), "Blockout" );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    // The render form the editor derives from the source file (LoadMeshPlatformData's answer).
    const auto source = Assets::ReadMeshSourceAssetFile( written.GetValue() );
    ASSERT_TRUE( source.IsSuccess() ) << source.GetError();
    const auto derived = Editor::BuildMeshPlatformData( source.GetValue() );
    ASSERT_TRUE( derived.IsSuccess() ) << derived.GetError();
    auto data = Assets::Serialization::ReadMeshAssetData( derived.GetValue(), "M11" );
    ASSERT_TRUE( data.IsSuccess() ) << data.GetError();
    auto lifted = Geometry::DynamicMeshFromMeshAssetData( data.GetValue() );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    EXPECT_EQ( Editor::Tools::MeshKeyOf( lifted.GetValue().Mesh ), key )
         << "the lifted .stmesh must key as the EditMesh it was written from";

    // Reopened on the entity, moved and turned since: two layers, in the world.
    const glm::mat4 world  = glm::rotate( glm::translate( glm::mat4( 1.0f ), { 300.0f, 0.0f, 20.0f } ),
                                          glm::radians( 40.0f ), glm::vec3( 0.0f, 1.0f, 0.0f ) );
    auto            opened = Editor::Tools::ReopenBlockout(
         "Blockout", &read->Saved, Common::MakeSuccess( Editor::Tools::MeshKeyOf( lifted.GetValue().Mesh ) ),
         world );
    ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
    EXPECT_EQ( opened.GetValue().Volume.m_Frozen.size(), 2u );
    const glm::vec3 corner = opened.GetValue().Volume.m_Frozen[0].Frame.ToWorldPoint( { 100.0f, 0.0f, 0.0f } );
    EXPECT_LT( glm::length( corner - glm::vec3( world * glm::vec4( 100.0f, 0.0f, 0.0f, 1.0f ) ) ), 1e-3f );
}

TEST( CubeGridReopen, ReopenRefusesByReason )
{
    const VB::Volume        built = FloorWithBlock();
    const uint64_t          key   = Editor::Tools::MeshKeyOf( MeshOf( built ) );
    const VB::SavedBlockout saved = VB::Save( built, key );
    const glm::mat4         id( 1.0f );
    auto                    why = []( const Common::ResultStr<Editor::Tools::ReopenedBlockout>& r )
    { return r.IsSuccess() ? std::string( "<opened>" ) : r.GetError(); };

    EXPECT_NE( why( Editor::Tools::ReopenBlockout( "Cube", nullptr, Common::MakeSuccess( key ), id ) )
                    .find( "'Cube' was not built by CubeGrid" ),
               std::string::npos );
    EXPECT_NE( why( Editor::Tools::ReopenBlockout( "B", &saved, Common::MakeSuccess( key + 1 ), id ) )
                    .find( "was edited after CubeGrid built it" ),
               std::string::npos );
    EXPECT_NE( why( Editor::Tools::ReopenBlockout( "B", &saved, Common::MakeError<uint64_t>( "no file" ), id ) )
                    .find( "cannot be read: no file" ),
               std::string::npos );
    EXPECT_NE( why( Editor::Tools::ReopenBlockout( "B", &saved, Common::MakeSuccess( key ),
                                                   glm::scale( id, glm::vec3( 2.0f ) ) ) )
                    .find( "is scaled" ),
               std::string::npos );
    EXPECT_TRUE( Editor::Tools::ReopenBlockout( "B", &saved, Common::MakeSuccess( key ), id ).IsSuccess() );
}

TEST( CubeGridReopen, AnEditChangesTheMeshAndTheStateCancelReturnsToReopensToTheOldOne )
{
    // Accept of the first session: the entity's component and mesh - what Cancel and undo restore.
    const VB::Volume     built = FloorWithBlock();
    const uint64_t       keyA  = Editor::Tools::MeshKeyOf( MeshOf( built ) );
    Common::Json::Issues issues;
    const auto before = ThroughTheScene( ECS::CubeGridBlockoutComponent{ VB::Save( built, keyA ) }, issues );
    ASSERT_TRUE( before.has_value() );

    // The second session: reopen, Q through the block and the floor under it, Accept.
    auto opened =
         Editor::Tools::ReopenBlockout( "B", &before->Saved, Common::MakeSuccess( keyA ), glm::mat4( 1.0f ) );
    ASSERT_TRUE( opened.IsSuccess() ) << opened.GetError();
    VB::Volume w = opened.GetValue().Volume;
    w.m_Frame    = w.m_Frozen.back().Frame;
    w.m_Unit     = 100.0f;
    VB::WorkPlane q{ 1, 1, 2 };
    w.PushPull( q, VB::Rect{ 1, 2, 1, 2 }, -1, 2, 0 );
    const uint64_t keyB = Editor::Tools::MeshKeyOf( MeshOf( w ) );
    EXPECT_NE( keyB, keyA ) << "Q changed the reopened blockout's mesh";

    // The state Cancel / undo put back still reopens, and to the mesh it had.
    auto again =
         Editor::Tools::ReopenBlockout( "B", &before->Saved, Common::MakeSuccess( keyA ), glm::mat4( 1.0f ) );
    ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
    EXPECT_EQ( Editor::Tools::MeshKeyOf( MeshOf( again.GetValue().Volume ) ), keyA );
    // ... and the edited mesh does not pass for it: the old voxels are refused on the new mesh.
    EXPECT_FALSE(
         Editor::Tools::ReopenBlockout( "B", &before->Saved, Common::MakeSuccess( keyB ), glm::mat4( 1.0f ) )
              .IsSuccess() );
}

TEST( CubeGridReopen, ABlockThatDoesNotLoadIsAnIssueAndNoComponent )
{
    ECS::CubeGridBlockoutComponent bad;
    bad.Saved.Layers.push_back( VB::SavedLayer{ -1.0f, {}, { 1, 0, 0, 0 }, {}, {} } );
    bad.Saved.MeshKey = "0000000000000000";
    Common::Json::Issues issues;
    EXPECT_FALSE( ThroughTheScene( bad, issues ).has_value() );
    ASSERT_EQ( issues.size(), 1u );
    EXPECT_NE( issues.front().Expected.find( "not positive" ), std::string::npos ) << issues.front().Expected;

    ECS::CubeGridBlockoutComponent badKey;
    badKey.Saved.MeshKey = "xyz";
    issues.clear();
    EXPECT_FALSE( ThroughTheScene( badKey, issues ).has_value() );
    ASSERT_EQ( issues.size(), 1u );
}
