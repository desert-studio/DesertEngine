// A modeling result written as a .stmesh (M7): the registry row the write leaves (with its box), and the name
// that is never overwritten. The mesh <-> file round trip itself is the DynamicMeshAsset suite's.

#include <Editor/Core/StaticMeshOutput.hpp>

#include <Common/Core/Constants.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/MeshCore/DynamicMesh/DynamicMeshAttributeSet.hpp>
#include <Engine/Geometry/ShapeGenerators.hpp>

#include <gtest/gtest.h>

#include <Editor/Import/MeshDeriver.hpp>
#include <Editor/Panels/StaticMeshViewer/StaticMeshStats.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <filesystem>
#include <random>
#include <set>
#include <vector>
#include "../../TestSupport/scratch_dir.hpp"
#include "../../TestSupport/engine_dir.hpp"
#include "../../TestSupport/runner.hpp"

namespace fs = std::filesystem;
using namespace Desert;

namespace
{
    // The writer takes the component's mesh, shared and immutable as the component holds it.
    std::shared_ptr<const Geometry::DynamicMesh3> Dyn( const Geometry::DynamicMesh3& mesh )
    {
        return std::make_shared<const Geometry::DynamicMesh3>( mesh );
    }

    void SetMaterial( Geometry::DynamicMesh3& mesh, int t, int32_t material )
    {
        mesh.Attributes()->GetMaterialID()->SetValue( t, &material );
    }

    // A box with one polygroup per face and two materials (odd faces on slot 1), so a round trip that loses
    // groups, reorders faces or collapses submeshes has something to lose.
    Geometry::DynamicMesh3 TwoMaterialBox()
    {
        auto shape = Geometry::MakeBox( glm::vec3( 200.0f, 100.0f, 50.0f ), glm::ivec3( 2, 1, 1 ) );
        auto mesh  = Geometry::ShapeToDynamicMesh( shape );
        EXPECT_TRUE( mesh.IsSuccess() ) << ( mesh.IsSuccess() ? "" : mesh.GetError() );
        Geometry::DynamicMesh3 out = mesh.ExtractValue();
        for ( const int t : out.TriangleIndicesItr() )
            SetMaterial( out, t, out.GetTriangleGroup( t ) % 2 );
        return out;
    }

    const std::vector<Common::Content::AssetGuid> kSlots = { { 1111, 1 }, { 2222, 2 } };

    // A throw-away project, so the write lands under a mesh root and the registry can key it.
    class ScratchProject : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            m_Saved = Common::Constants::Path::CurrentProjectRoot();
            m_Dir = fs::temp_directory_path() / ( "StaticMeshOutput_" + std::to_string( std::random_device{}() ) );
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

TEST_F( ScratchProject, TheWriteLeavesARegistryRowWithTheMeshBox )
{
    auto folder = Editor::StaticMeshOutputFolder( "Modeling" );
    ASSERT_TRUE( folder.IsSuccess() ) << folder.GetError();
    auto written = Editor::WriteStaticMeshAsset( *Dyn( TwoMaterialBox() ), kSlots, folder.GetValue(), "Box" );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    const fs::path& path = written.GetValue();
    EXPECT_EQ( path.filename(), "Box.stmesh" );
    EXPECT_EQ( path.parent_path(), ( Common::Constants::Path::MESH_PATH / "Modeling" ).lexically_normal() );

    // The file is the source asset an import writes, with no source file behind it.
    auto asset = Assets::ReadMeshSourceAssetFile( path );
    ASSERT_TRUE( asset.IsSuccess() ) << asset.GetError();
    const Assets::MeshSourceAsset& read = asset.GetValue();
    EXPECT_EQ( read.Kind, Common::Content::ContentKind::StaticMesh );
    EXPECT_EQ( read.Name, "Box" );
    EXPECT_EQ( read.Import.Provenance, Assets::MeshSourceProvenance::Recovered );
    EXPECT_TRUE( read.Import.SourceFile.empty() );
    ASSERT_EQ( read.Source.Models.size(), 1u );
    EXPECT_EQ( read.Source.Models[0].Mesh, Geometry::ToSerialized( *Dyn( TwoMaterialBox() ) ) );
    ASSERT_EQ( read.Source.MaterialSlots.size(), kSlots.size() );
    for ( std::size_t k = 0; k < kSlots.size(); ++k )
        EXPECT_EQ( read.Source.MaterialSlots[k].Material, kSlots[k] ) << "slot " << k;
    const std::set<int> ids( read.Source.Models[0].Mesh.MaterialIds.begin(),
                             read.Source.Models[0].Mesh.MaterialIds.end() );
    EXPECT_EQ( ids, ( std::set<int>{ 0, 1 } ) ) << "material ids must stay slot indices";

    const auto* row = Assets::ContentRegistry::Get().FindByKey( Common::AssetHandle::StableKeyForPath( path ) );
    ASSERT_NE( row, nullptr ) << "no registry row for " << path;
    EXPECT_EQ( row->Kind, "StaticMesh" );
    EXPECT_EQ( row->Size, fs::file_size( path ) );
    ASSERT_TRUE( row->Bounds.has_value() ) << "the row carries no box (WP15)";
    // MakeBox with a base pivot: 200 x 100 x 50 cm standing on y = 0.
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_EQ( row->Bounds->Min, glm::vec3( -100.0f, 0.0f, -25.0f ) );
    // NOLINTEND(bugprone-unchecked-optional-access)
    // NOLINTBEGIN(bugprone-unchecked-optional-access)
    EXPECT_EQ( row->Bounds->Max, glm::vec3( 100.0f, 100.0f, 25.0f ) );
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST_F( ScratchProject, ATakenNameGetsASuffixAndTheFirstFileIsUntouched )
{
    auto target = Editor::StaticMeshOutputFolder( "Modeling" );
    ASSERT_TRUE( target.IsSuccess() ) << target.GetError();
    const fs::path& folder = target.GetValue();
    auto            first  = Editor::WriteStaticMeshAsset( *Dyn( TwoMaterialBox() ), kSlots, folder, "Box" );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    const auto firstTime = fs::last_write_time( first.GetValue() );

    Geometry::DynamicMesh3 other = TwoMaterialBox();
    for ( const int t : other.TriangleIndicesItr() )
        SetMaterial( other, t, 0 );
    auto second = Editor::WriteStaticMeshAsset( *Dyn( other ), kSlots, folder, "Box" );
    ASSERT_TRUE( second.IsSuccess() ) << second.GetError();
    auto third = Editor::WriteStaticMeshAsset( *Dyn( other ), kSlots, folder, "Box" );
    ASSERT_TRUE( third.IsSuccess() ) << third.GetError();

    EXPECT_EQ( first.GetValue().filename(), "Box.stmesh" );
    EXPECT_EQ( second.GetValue().filename(), "Box_1.stmesh" );
    EXPECT_EQ( third.GetValue().filename(), "Box_2.stmesh" );
    EXPECT_EQ( fs::last_write_time( first.GetValue() ), firstTime ) << "the first file was rewritten";
    auto firstAsset  = Assets::ReadMeshSourceAssetFile( first.GetValue() );
    auto secondAsset = Assets::ReadMeshSourceAssetFile( second.GetValue() );
    ASSERT_TRUE( firstAsset.IsSuccess() && secondAsset.IsSuccess() );
    EXPECT_NE( firstAsset.GetValue().Source, secondAsset.GetValue().Source );
    EXPECT_NE( firstAsset.GetValue().Guid, secondAsset.GetValue().Guid );
}

TEST_F( ScratchProject, ANameAndAFolderAreTakenAsGivenOrRefused )
{
    EXPECT_EQ( Editor::StaticMeshAssetName( "Box 2/a:b" ), "Box_2_a_b" );
    EXPECT_EQ( Editor::StaticMeshAssetName( "" ), "Mesh" );
    EXPECT_FALSE( Editor::StaticMeshOutputFolder( "../Escaped" ).IsSuccess() );
    EXPECT_FALSE( Editor::StaticMeshOutputFolder( "/abs" ).IsSuccess() );

    // A refused mesh writes nothing.
    Geometry::DynamicMesh3 coloured = TwoMaterialBox();
    coloured.Attributes()->EnablePrimaryColors();
    auto target = Editor::StaticMeshOutputFolder( "Modeling" );
    ASSERT_TRUE( target.IsSuccess() ) << target.GetError();
    const fs::path& folder = target.GetValue();
    EXPECT_FALSE( Editor::WriteStaticMeshAsset( *Dyn( coloured ), kSlots, folder, "Box" ).IsSuccess() );
    EXPECT_FALSE( fs::exists( folder / "Box.stmesh" ) );
}

// --- The static mesh viewer's statistics (AV1f) ------------------------------------------------------------
//
// The window prints DescribeStaticMesh of the mesh's BUILT platform data (the DDC value StaticMeshAsset draws);
// asserted here against the committed probe, derived by the editor's own builder and read through the engine's
// own reader, so the numbers on screen are the numbers the renderer draws.
namespace
{
    std::filesystem::path ProbeMeshFile()
    {
        // A test-only probe: it lives in the suite data tree, never in the engine's content (Editor/Resources).
        return Desert::TestSupport::TestDataDir() / "Resources" / "Assets" / "Meshes" / "StaticProbe.stmesh";
    }
} // namespace

TEST( StaticMeshViewerStats, TheProbeReportsFortyEightVerticesTwentyFourTrianglesTwoSections )
{
    const auto source = Desert::Assets::ReadMeshSourceAssetFile( ProbeMeshFile() );
    ASSERT_TRUE( source.IsSuccess() ) << source.GetError();
    const auto built = Desert::Editor::BuildMeshPlatformData( source.GetValue() );
    ASSERT_TRUE( built.IsSuccess() ) << built.GetError();
    const auto data =
         Desert::Assets::Serialization::ReadMeshAssetData( built.GetValue(), ProbeMeshFile().string() );
    ASSERT_TRUE( data.IsSuccess() ) << data.GetError();

    const auto stats = Desert::Editor::DescribeStaticMesh( data.GetValue() );
    EXPECT_EQ( stats.Vertices, 48u );
    EXPECT_EQ( stats.Triangles, 24u );
    EXPECT_EQ( stats.Sections, 2u );
    ASSERT_GE( stats.LODs(), 1u );
    EXPECT_EQ( stats.TrianglesPerLOD[0], stats.Triangles )
         << "LOD 0 summed over the sections disagrees with the triangle array: IndexCount is in index units";
    const auto expected = Desert::Assets::Serialization::MeshDataBounds( data.GetValue() );
    ASSERT_TRUE( stats.Bounds.has_value() && expected.has_value() );
    const auto& bounds = stats.Bounds.value(); // NOLINT(bugprone-unchecked-optional-access): asserted above
    const auto& want   = expected.value();     // NOLINT(bugprone-unchecked-optional-access): asserted above
    EXPECT_EQ( bounds.Min, want.Min );
    EXPECT_EQ( bounds.Max, want.Max );
    EXPECT_GT( bounds.Max.y - bounds.Min.y, 0.0f );
}

TEST( StaticMeshViewerStats, AShorterChainCountsItsCoarsestLevelAtDeeperLODs )
{
    Desert::Assets::Serialization::MeshAssetData data;
    data.Indices.resize( 5 );
    Desert::Assets::Serialization::SubmeshData a{};
    a.IndexCount = 9; // 3 triangles
    a.LODs       = { std::vector<Desert::Assets::Serialization::IndexData>( 2 ),
                     std::vector<Desert::Assets::Serialization::IndexData>( 1 ) };
    Desert::Assets::Serialization::SubmeshData b{};
    b.IndexCount   = 6; // 2 triangles, no chain
    data.Submeshes = { a, b };

    const auto stats = Desert::Editor::DescribeStaticMesh( data );
    ASSERT_EQ( stats.LODs(), 3u );
    EXPECT_EQ( stats.TrianglesPerLOD[0], 5u );
    EXPECT_EQ( stats.TrianglesPerLOD[1], 4u );
    EXPECT_EQ( stats.TrianglesPerLOD[2], 3u );
}

namespace
{
    // The host steps this suite's process takes before gtest starts (TestSupport/runner.hpp).
    const Desert::TestSupport::SuiteHost kHostSteps{ { .EngineDir = true } };
} // namespace
