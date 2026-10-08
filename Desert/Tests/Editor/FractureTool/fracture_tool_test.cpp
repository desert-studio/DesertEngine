// DST-02b: the Fracture mode's editor state (Editor/Panels/Fracture/FractureTool.hpp) - what the panel draws and
// the command palette drives. Undo / redo of a Generate reach the panel without a manual Load; the first
// Generate of a new .dfrac names its source mesh; the panel picks the interior material from the material
// asset rows, edits Planar planes, and the mode has a palette group (FractureCommands.cpp, read as text).

#include <Editor/Core/CommandHistory.hpp>
#include <Editor/Panels/Fracture/FractureTool.hpp>

#include <Engine/Destruction/FractureFormat.hpp>
#include <Engine/Geometry/MeshCore/VectorUtil.hpp>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    using namespace Desert::Destruction;
    using Desert::Editor::FractureTool;
    using Desert::Geometry::DynamicMesh3;

    DynamicMesh3 MakeCube()
    {
        DynamicMesh3 mesh;
        for ( int i = 0; i < 8; i++ )
            mesh.AppendVertex( glm::dvec3( -50.0 ) + 100.0 * glm::dvec3( i & 1, ( i >> 1 ) & 1, ( i >> 2 ) & 1 ) );
        const std::array<std::array<int, 4>, 6> faces{
             { { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 } } };
        for ( const auto& f : faces )
        {
            glm::dvec3 faceCenter( 0 );
            for ( const int v : f )
                faceCenter += 0.25 * mesh.GetVertex( v );
            for ( const auto [a, b, c] : { std::array{ f[0], f[1], f[2] }, std::array{ f[0], f[2], f[3] } } )
            {
                const glm::dvec3 n = Desert::Geometry::VectorUtil::Normal(
                     mesh.GetVertex( a ), mesh.GetVertex( b ), mesh.GetVertex( c ) );
                if ( glm::dot( n, faceCenter ) < 0 )
                    mesh.AppendTriangle( a, c, b );
                else
                    mesh.AppendTriangle( a, b, c );
            }
        }
        return mesh;
    }

    FractureLevelSettings Uniform( int sites )
    {
        FractureLevelSettings l;
        l.Method    = FractureMethod::Uniform;
        l.SiteCount = sites;
        return l;
    }

    // A tool aimed at a fresh absolute scratch file (FractureTool::File keeps an absolute Path as is).
    void AimAtScratch( FractureTool& tool, const char* name )
    {
        const auto dir = std::filesystem::temp_directory_path() / "DesertFractureTool";
        std::filesystem::create_directories( dir );
        const auto file = dir / name;
        std::filesystem::remove( file );
        tool.Path = file.string();
    }

    std::string RepoRoot()
    {
        std::string prefix = "./";
        for ( int up = 0; up < 6; ++up )
        {
            const std::ifstream probe( prefix + "Desert/Desert/Source/Engine/ECS/Components.hpp" );
            if ( probe )
                return prefix;
            prefix += "../";
        }
        return {};
    }

    std::string ReadText( const std::string& relative )
    {
        const std::ifstream in( RepoRoot() + relative );
        std::ostringstream  ss;
        ss << in.rdbuf();
        return ss.str();
    }

    const Desert::Common::Content::AssetGuid kMesh{ 0x5717Cull, 0xAE5Bull };
} // namespace

// Undo / redo of a Generate are seen by the tool the panel draws on its next Refresh (called every frame by
// FracturePanel::OnUIRender): the restored collection, its settings, and - after undoing the first Generate -
// no collection. Red when the panel keeps showing what it read before the undo until Load is pressed.
TEST( FractureTool, UndoAndRedoShowTheRestoredCollectionWithoutLoad )
{
    const DynamicMesh3 cube = MakeCube();
    FractureTool       tool;
    AimAtScratch( tool, "UndoRefresh.dfrac" );
    tool.Settings.Seed   = 1;
    tool.Settings.Levels = { Uniform( 4 ) };
    ASSERT_TRUE( tool.Generate( cube, kMesh ) ) << tool.Status();
    const auto first = EncodeFracturePayload( tool.Fracture() );

    tool.Settings.Seed   = 2;
    tool.Settings.Levels = { Uniform( 9 ) };
    ASSERT_TRUE( tool.Generate( cube, kMesh ) ) << tool.Status();
    const auto second = EncodeFracturePayload( tool.Fracture() );
    ASSERT_NE( first, second );
    EXPECT_FALSE( tool.Refresh() ); // nothing moved since the tool's own write

    auto& history = Desert::Editor::CommandHistory::Get();
    ASSERT_TRUE( history.Undo() );
    EXPECT_TRUE( tool.Refresh() );
    EXPECT_EQ( EncodeFracturePayload( tool.Fracture() ), first );
    EXPECT_EQ( tool.Settings.Seed, 1u );

    ASSERT_TRUE( history.Redo() );
    EXPECT_TRUE( tool.Refresh() );
    EXPECT_EQ( EncodeFracturePayload( tool.Fracture() ), second );

    ASSERT_TRUE( history.Undo() );
    ASSERT_TRUE( history.Undo() ); // the first Generate: the file did not exist before it
    EXPECT_TRUE( tool.Refresh() );
    EXPECT_FALSE( tool.Loaded() );
    EXPECT_TRUE( tool.Fracture().Nodes.empty() );

    // And the panel calls it every frame.
    const std::string panel  = ReadText( "Editor/Source/Editor/Panels/Fracture/FracturePanel.cpp" );
    const auto        render = panel.find( "void FracturePanel::OnUIRender()" );
    ASSERT_NE( render, std::string::npos );
    EXPECT_NE( panel.find( ".Refresh()", render ), std::string::npos );
    EXPECT_LT( panel.find( ".Refresh()", render ), panel.find( "DrawTarget();", render ) );
}

// The first Generate of a NEW .dfrac writes the selected mesh's GUID as its source. Red when the source is copied
// from the (empty) current fracture, or when a mesh with no asset GUID is fractured into an unnamed source.
TEST( FractureTool, FirstGenerateOfANewFileRecordsTheSourceMesh )
{
    const DynamicMesh3 cube = MakeCube();
    FractureTool       tool;
    AimAtScratch( tool, "FirstSource.dfrac" );
    tool.Settings.Levels = { Uniform( 4 ) };
    EXPECT_FALSE( tool.Generate( cube, {} ) ); // no asset, no source to name
    EXPECT_FALSE( std::filesystem::exists( tool.File() ) );
    ASSERT_TRUE( tool.Generate( cube, kMesh ) ) << tool.Status();
    EXPECT_EQ( tool.Fracture().SourceMesh, kMesh );

    // The palette / panel path resolves the selection's asset GUID (GuidForHandle) and hands it here.
    const std::string commands = ReadText( "Editor/Source/Editor/Panels/Fracture/FractureCommands.cpp" );
    EXPECT_NE( commands.find( "GuidForHandle( component.MeshHandle )" ), std::string::npos );
    EXPECT_NE( commands.find( "tool.Generate( *target.GetValue().Mesh, *guid )" ), std::string::npos );
}

// The interior material is set by the material picker (material asset rows -> the row's GUID), not a typed hex
// id, and is one undo step. Red when the panel goes back to a GUID text field or the edit is not undoable.
TEST( FractureTool, InteriorMaterialIsPickedFromMaterialAssetsAsOneUndoStep )
{
    const DynamicMesh3 cube = MakeCube();
    FractureTool       tool;
    AimAtScratch( tool, "Interior.dfrac" );
    tool.Settings.Levels = { Uniform( 4 ) };
    ASSERT_TRUE( tool.Generate( cube, kMesh ) );
    const Desert::Common::Content::AssetGuid material{ 0x3A7Eull, 0x21A1ull };
    ASSERT_TRUE( tool.SetInteriorMaterial( material ) );
    EXPECT_EQ( tool.Fracture().InteriorMaterial, material );
    ASSERT_TRUE( Desert::Editor::CommandHistory::Get().Undo() );
    EXPECT_TRUE( tool.Refresh() );
    EXPECT_TRUE( tool.Fracture().InteriorMaterial.IsNull() );

    const std::string panel = ReadText( "Editor/Source/Editor/Panels/Fracture/FracturePanel.cpp" );
    EXPECT_NE( panel.find( "ContentRegistry::Rows( Common::Content::ContentKind::Material )" ),
               std::string::npos );
    EXPECT_NE( panel.find( "PickerDisplayName( row )" ), std::string::npos );
    EXPECT_NE( panel.find( "tool.SetInteriorMaterial( *row.Guid )" ), std::string::npos );
    EXPECT_EQ( panel.find( "InteriorGuid" ), std::string::npos );
    EXPECT_EQ( panel.find( "from_chars" ), std::string::npos );
}

// The mode is a palette group (so the control socket can drive it for a frame) and Planar planes are edited in
// the panel. Red when the group is not registered, loses an entry the frame needs, or planes become read-only.
TEST( FractureTool, TheModeIsDrivenFromThePaletteAndPlanarPlanesAreEditable )
{
    const std::string layer = ReadText( "Editor/Source/EditorLayer.cpp" );
    EXPECT_NE( layer.find( "AppendFractureCommands( out, m_Workspace.ActiveScene() )" ), std::string::npos );

    const std::string commands = ReadText( "Editor/Source/Editor/Panels/Fracture/FractureCommands.cpp" );
    for ( const char* label :
          { "\"Fracture mode\"", "\"Target: {}\"", "\"Generate from the selected static mesh\"",
            "\"Random Seed: next\"", "\"Add Level\"", "\"Last level method: {}\"",
            "\"Last level: add a plane across {} through the origin\"", "\"Interior material: none\"",
            "\"Explode Amount: {:.1f}\"", "\"Fracture Level: all\"" } )
        EXPECT_NE( commands.find( label ), std::string::npos ) << label;

    const std::string panel = ReadText( "Editor/Source/Editor/Panels/Fracture/FracturePanel.cpp" );
    EXPECT_NE( panel.find( "DrawPlanes( level )" ), std::string::npos );
    EXPECT_NE( panel.find( "&plane.Normal.x" ), std::string::npos );
    EXPECT_NE( panel.find( "&plane.Point.x" ), std::string::npos );
    EXPECT_NE( panel.find( "\"Add Plane\"" ), std::string::npos );
}
