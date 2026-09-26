// A modeling result written as a .stmesh and read back (M7). The relation pinned is the round trip:
// EditMesh -> MeshAssetData -> MeshBinary bytes -> MeshAssetData -> EditMesh keeps every polygroup, corner,
// normal, tangent, UV and material - judged by rendering both meshes and comparing the render arrays, which
// is what a frame of the entity before and after the conversion would compare. Around it: the registry row
// the write leaves (with its box), and the name that is never overwritten.

#include <Editor/Import/StaticMeshOutput.hpp>

#include <Common/Core/Constants.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Assets/ContentRegistry.hpp>
#include <Engine/Assets/MeshSourceAsset.hpp>
#include <Engine/Geometry/EditMeshAsset.hpp>
#include <Engine/Geometry/DynamicMeshSerialization.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
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

namespace fs = std::filesystem;
using namespace Desert;

namespace
{
    // The writer takes the component's core; the fixtures are built on EditMesh and cross through the saved
    // form (what EditMeshBridge does, without linking the bridge's ECS half into a GPU-free suite).
    std::shared_ptr<const Geometry::FDynamicMesh3> Dyn( const Geometry::EditMesh& mesh )
    {
        auto converted = Geometry::DynamicMeshFromSerialized( Geometry::ToSerialized( mesh ), "StaticMeshOutput" );
        EXPECT_TRUE( converted.IsSuccess() ) << ( converted.IsSuccess() ? "" : converted.GetError() );
        return std::make_shared<const Geometry::FDynamicMesh3>(
             converted.IsSuccess() ? converted.ExtractValue() : Geometry::FDynamicMesh3{} );
    }

    // A box with one polygroup per face and two materials (odd faces on slot 1), so a round trip that loses
    // groups, reorders faces or collapses submeshes has something to lose.
    Geometry::EditMesh TwoMaterialBox()
    {
        auto shape = Geometry::MakeBox( glm::vec3( 200.0f, 100.0f, 50.0f ), glm::ivec3( 2, 1, 1 ) );
        auto mesh  = Geometry::ShapeToEditMesh( shape );
        EXPECT_TRUE( mesh.IsSuccess() ) << ( mesh.IsSuccess() ? "" : mesh.GetError() );
        Geometry::EditMesh out = mesh.ExtractValue();
        for ( const int t : out.TriangleIds() )
            out.Attributes().SetMaterialId( t, out.Attributes().GetPolyGroup( t ) % 2 );
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

TEST( StaticMeshOutput, TheRoundTripThroughTheFileKeepsGroupsCornersAndMaterials )
{
    const Geometry::EditMesh original = TwoMaterialBox();

    auto data = Geometry::ToMeshAssetData( original, kSlots );
    ASSERT_TRUE( data.IsSuccess() ) << data.GetError();
    const std::string bytes   = Assets::Serialization::EncodeMeshBinary( data.GetValue() );
    auto              decoded = Assets::Serialization::DecodeMeshBinary( bytes, "round trip" );
    ASSERT_TRUE( decoded.IsSuccess() ) << decoded.GetError();
    const auto& file = decoded.GetValue();

    ASSERT_EQ( file.Submeshes.size(), 2u );
    EXPECT_EQ( file.Submeshes[0].MaterialGuid, kSlots[0] );
    EXPECT_EQ( file.Submeshes[1].MaterialGuid, kSlots[1] );
    ASSERT_EQ( file.PolyGroups.size(), file.Indices.size() ) << "the PolyGroups section was not filled";

    auto lifted = Geometry::FromMeshAssetData( file );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    const Geometry::EditMesh& back = lifted.GetValue().Mesh;
    ASSERT_EQ( back.TriangleCount(), original.TriangleCount() );
    ASSERT_EQ( back.VertexCount(), original.VertexCount() ) << "the weld did not close the shell again";

    // Face k of the file came from original triangle SourceTriangles[k] and is triangle k of the lifted mesh.
    auto before = Geometry::ToRenderMesh( original );
    auto after  = Geometry::ToRenderMesh( back );
    ASSERT_TRUE( before.IsSuccess() && after.IsSuccess() );
    const auto&   a = before.GetValue();
    const auto&   b = after.GetValue();
    std::set<int> groups;
    for ( size_t k = 0; k < a.SourceTriangles.size(); ++k )
    {
        const int from = a.SourceTriangles[k];
        EXPECT_EQ( back.Attributes().GetPolyGroup( static_cast<int>( k ) ),
                   original.Attributes().GetPolyGroup( from ) )
             << "face " << k;
        EXPECT_EQ( back.Attributes().GetMaterialId( static_cast<int>( k ) ),
                   original.Attributes().GetMaterialId( from ) )
             << "face " << k;
        groups.insert( original.Attributes().GetPolyGroup( from ) );
    }
    EXPECT_EQ( groups.size(), 6u ) << "the box should carry one group per face";

    // What the entity draws: the same render arrays, corner for corner, bit for bit.
    ASSERT_EQ( a.Vertices.size(), b.Vertices.size() );
    ASSERT_EQ( a.Indices.size(), b.Indices.size() );
    ASSERT_EQ( a.Submeshes.size(), b.Submeshes.size() );
    for ( size_t i = 0; i < a.Vertices.size(); ++i )
    {
        EXPECT_EQ( a.Vertices[i].Position, b.Vertices[i].Position ) << "vertex " << i;
        EXPECT_EQ( a.Vertices[i].Normal, b.Vertices[i].Normal ) << "vertex " << i;
        EXPECT_EQ( a.Vertices[i].Tangent, b.Vertices[i].Tangent ) << "vertex " << i;
        EXPECT_EQ( a.Vertices[i].Bitangent, b.Vertices[i].Bitangent ) << "vertex " << i;
        EXPECT_EQ( a.Vertices[i].TexCoord, b.Vertices[i].TexCoord ) << "vertex " << i;
    }
    for ( size_t i = 0; i < a.Indices.size(); ++i )
        EXPECT_TRUE( a.Indices[i].V1 == b.Indices[i].V1 && a.Indices[i].V2 == b.Indices[i].V2 &&
                     a.Indices[i].V3 == b.Indices[i].V3 )
             << "face " << i;
}

TEST( StaticMeshOutput, AFileWithoutGroupsLiftsWithEveryFaceInGroupZero )
{
    auto data = Geometry::ToMeshAssetData( TwoMaterialBox(), kSlots );
    ASSERT_TRUE( data.IsSuccess() ) << data.GetError();
    auto v1Shape       = data.ExtractValue();
    v1Shape.PolyGroups = {};
    auto lifted        = Geometry::FromMeshAssetData( v1Shape );
    ASSERT_TRUE( lifted.IsSuccess() ) << lifted.GetError();
    for ( const int t : lifted.GetValue().Mesh.TriangleIds() )
        EXPECT_EQ( lifted.GetValue().Mesh.Attributes().GetPolyGroup( t ), 0 );
}

TEST( StaticMeshOutput, ALayerTheFileCannotHoldIsRefusedByName )
{
    Geometry::EditMesh coloured = TwoMaterialBox();
    coloured.Attributes().EnableColors();
    auto refused = Geometry::ToMeshAssetData( coloured, kSlots );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "colour" ), std::string::npos ) << refused.GetError();

    Geometry::EditMesh twoUV = TwoMaterialBox();
    ASSERT_TRUE( twoUV.Attributes().SetUVLayerCount( 2 ) );
    auto refusedUV = Geometry::ToMeshAssetData( twoUV, kSlots );
    ASSERT_FALSE( refusedUV.IsSuccess() );
    EXPECT_NE( refusedUV.GetError().find( "2 UV layers" ), std::string::npos ) << refusedUV.GetError();
}

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

    Geometry::EditMesh other = TwoMaterialBox();
    for ( const int t : other.TriangleIds() )
        other.Attributes().SetMaterialId( t, 0 );
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
    Geometry::EditMesh coloured = TwoMaterialBox();
    coloured.Attributes().EnableColors();
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
        std::filesystem::path here = std::filesystem::current_path();
        for ( int up = 0;
              up < 8 && !std::filesystem::exists( here / "Editor" / "Resources" / "Assets" / "Meshes" ); ++up )
            here = here.parent_path();
        return here / "Editor" / "Resources" / "Assets" / "Meshes" / "StaticProbe.stmesh";
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

int main( int argc, char** argv )
{
    testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
