#include <Engine/Assets/MaterialFormat.hpp>
#include <iterator>
// THM1i: one static mesh per source node (UE's Combine Meshes OFF), Combine Meshes ON keeps the one mesh.
//
// The assimp half (which node placed which submesh, ImportResult::SubmeshNodes) is not linkable here; the
// fixture stands in for what the importer hands over for a three-node glTF - three tufts, their node world
// transforms already baked, in a row along x - and the source file on disk is that glTF's text.

#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Editor/Import/MaterialAdoption.hpp>
#include <Editor/Import/NodeMeshSplit.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <gtest/gtest.h>

#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Desert;
namespace Ser = Assets::Serialization;

namespace
{
    // A 20 x 20 cm tuft of two triangles standing 30 cm tall, its box starting at @p x (world, baked).
    void AddTuft( Ser::MeshAssetData& data, const std::string& name, const float x,
                  const Common::Content::AssetGuid material )
    {
        Ser::SubmeshData sub{};
        sub.Name                   = name;
        sub.VertexOffset           = static_cast<uint32_t>( data.StaticVertices.size() );
        sub.IndexOffset            = static_cast<uint32_t>( data.Indices.size() * 3 );
        sub.Transform              = glm::mat4( 1.0f );
        sub.MaterialGuid           = material;
        const glm::vec3 corners[4] = {
             { x, 5.0f, 0.0f }, { x + 20.0f, 5.0f, 0.0f }, { x + 20.0f, 35.0f, 20.0f }, { x, 35.0f, 20.0f } };
        for ( const glm::vec3& c : corners )
            data.StaticVertices.push_back( { { c.x, c.y, c.z },
                                             { 0.0f, 0.0f, 1.0f },
                                             { 1.0f, 0.0f, 0.0f },
                                             { 0.0f, 1.0f, 0.0f },
                                             { 0.0f, 0.0f } } );
        data.Indices.push_back( { 0, 1, 2 } );
        data.Indices.push_back( { 0, 2, 3 } );
        sub.VertexCount = 4;
        sub.IndexCount  = 6;
        sub.BoundingBox = { { x, 5.0f, 0.0f }, { x + 20.0f, 35.0f, 20.0f } };
        data.Submeshes.push_back( sub );
    }

    struct GrassProject
    {
        fs::path                              Source;
        Common::Content::AssetGuid            Material = Common::Content::AssetGuid::Generate();
        std::vector<Assets::MeshMaterialSlot> Named;

        GrassProject()
        {
            const fs::path  root = fs::temp_directory_path() / "desert_node_mesh_split";
            std::error_code ec;
            fs::remove_all( root, ec );
            fs::create_directories( root, ec );
            Common::Constants::Path::SetProjectRoot( root, "Assets" );
            Source = Common::Constants::Path::ASSETS_PATH / "Meshes" / "Grass.gltf";
            fs::create_directories( Source.parent_path(), ec );
            std::ofstream( Source, std::ios::binary | std::ios::trunc )
                 << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1,2]}],)"
                    R"("nodes":[{"name":"TuftA","mesh":0},{"name":"TuftB","mesh":1},{"name":"TuftC","mesh":2}]})";
            Named = { { "GrassAtlas", Material } };
        }

        // Where the fixture's glTF places each tuft along x.
        [[nodiscard]] static float TuftX( const std::string& node )
        {
            if ( node == "TuftA" )
                return 0.0f;
            if ( node == "TuftB" )
                return 280.0f;
            return 560.0f;
        }

        // The importer's hand-over for @p order (node names, left to right in the file).
        [[nodiscard]] std::pair<Ser::MeshAssetData, std::vector<std::string>>
        Import( const std::vector<std::string>& order ) const
        {
            Ser::MeshAssetData       data;
            std::vector<std::string> nodes;
            for ( const std::string& node : order )
            {
                AddTuft( data, std::format( "{}_Mesh", node ), TuftX( node ), Material );
                nodes.push_back( node );
            }
            return { data, nodes };
        }

        // name -> GUID of every node mesh one import of @p order writes.
        [[nodiscard]] std::map<std::string, Common::Content::AssetGuid>
        ImportAndWrite( const std::vector<std::string>& order ) const
        {
            const auto [data, nodes] = Import( order );
            auto meshes              = Editor::NodeMeshesOfImport( data, nodes, Source );
            EXPECT_TRUE( meshes.IsSuccess() ) << meshes.GetError();
            std::map<std::string, Common::Content::AssetGuid> guids;
            if ( !meshes )
                return guids;
            for ( const Editor::NodeMesh& node : meshes.GetValue() )
            {
                const auto path = Editor::WriteNodeMeshAsset( node, Named, Source );
                EXPECT_TRUE( path.IsSuccess() ) << path.GetError();
                if ( !path )
                    continue;
                const auto asset = Assets::LoadMeshSourceAsset( path.GetValue() );
                EXPECT_TRUE( asset.IsSuccess() ) << asset.GetError();
                if ( asset )
                    guids[node.Node] = asset.GetValue().Guid;
            }
            return guids;
        }
    };
} // namespace

TEST( NodeMeshSplit, EveryNodeIsItsOwnMeshAroundItsBottomCentre )
{
    const GrassProject project;
    const auto [data, nodes] = project.Import( { "TuftA", "TuftB", "TuftC" } );
    const auto split         = Editor::SplitStaticMeshByNode( data, nodes );
    ASSERT_TRUE( split.IsSuccess() ) << split.GetError();
    ASSERT_EQ( split.GetValue().size(), 3u ) << "three nodes, three static meshes (Combine Meshes off)";
    for ( const Editor::NodeMesh& node : split.GetValue() )
    {
        ASSERT_EQ( node.Mesh.StaticVertices.size(), 4u ) << node.Node;
        ASSERT_EQ( node.Mesh.Indices.size(), 2u ) << node.Node;
        const auto box = Ser::MeshDataBounds( node.Mesh );
        if ( !box.has_value() )
            FAIL() << node.Node << " has no bounds";
        EXPECT_FLOAT_EQ( box->Min.y, 0.0f ) << node.Node << " does not stand on its pivot";
        EXPECT_FLOAT_EQ( box->Min.x + box->Max.x, 0.0f ) << node.Node << " is not centred in x";
        EXPECT_FLOAT_EQ( box->Min.z + box->Max.z, 0.0f ) << node.Node << " is not centred in z";
        EXPECT_FLOAT_EQ( node.Mesh.StaticVertices[0].Position.x, -10.0f ) << "vertices moved with the box";
    }
    EXPECT_EQ( split.GetValue()[0].Node, "TuftA" );
    EXPECT_EQ( split.GetValue()[2].Node, "TuftC" );
}

TEST( NodeMeshSplit, GuidsSurviveAReimportAndAReorderedExport )
{
    const GrassProject project;
    const auto         first = project.ImportAndWrite( { "TuftA", "TuftB", "TuftC" } );
    ASSERT_EQ( first.size(), 3u );
    std::error_code ec;
    for ( const auto& [node, guid] : first )
    {
        EXPECT_TRUE( fs::exists( Editor::NodeMeshAssetPath( project.Source, node ), ec ) ) << node;
        EXPECT_FALSE( guid.IsNull() ) << node;
    }
    EXPECT_NE( first.at( "TuftA" ), first.at( "TuftB" ) );

    EXPECT_EQ( project.ImportAndWrite( { "TuftA", "TuftB", "TuftC" } ), first ) << "a re-import minted new GUIDs";
    // The identity is the NODE NAME: an export that lists the nodes in another order names the same assets.
    EXPECT_EQ( project.ImportAndWrite( { "TuftC", "TuftA", "TuftB" } ), first )
         << "a node's GUID followed its index instead of its name";
}

TEST( NodeMeshSplit, CombineMeshesKeepsTheOneMesh )
{
    const GrassProject project;
    const auto [data, nodes] = project.Import( { "TuftA", "TuftB", "TuftC" } );
    const auto box           = Ser::MeshDataBounds( data );
    ASSERT_TRUE( box.has_value() );
    ASSERT_TRUE( Ser::EnsureImportRecord( project.Source, Common::Content::ContentKind::StaticMesh, box,
                                          Assets::SourceImportSettings{} )
                      .IsSuccess() );
    const auto combineDefault = Ser::ReadImportRecordSettings( project.Source );
    ASSERT_TRUE( combineDefault.IsSuccess() ) << combineDefault.GetError();
    ASSERT_FALSE( combineDefault.GetValue().CombineMeshes ) << "UE's default is off";

    // The import writes the options it runs with into the record, which every later reader reads.
    Assets::SourceImportSettings on;
    on.CombineMeshes = true;
    ASSERT_TRUE( Ser::EnsureImportRecord( project.Source, Common::Content::ContentKind::StaticMesh,
                                          Common::Math::AABB{ { 0, 0, 0 }, { 1, 1, 1 } }, on )
                      .IsSuccess() );
    {
        const auto combine = Ser::ReadImportRecordSettings( project.Source );
        ASSERT_TRUE( combine.IsSuccess() && combine.GetValue() == on );
    }

    const auto meshes = Editor::NodeMeshesOfImport( data, nodes, project.Source );
    ASSERT_TRUE( meshes.IsSuccess() ) << meshes.GetError();
    EXPECT_TRUE( meshes.GetValue().empty() ) << "Combine Meshes on: only the one combined mesh is written";
}

TEST( NodeMeshSplit, ASingleNodeIsNotSplitAndAMismatchIsRefused )
{
    const GrassProject project;
    const auto [one, oneNode] = project.Import( { "TuftA" } );
    const auto single         = Editor::NodeMeshesOfImport( one, oneNode, project.Source );
    ASSERT_TRUE( single.IsSuccess() ) << single.GetError();
    EXPECT_TRUE( single.GetValue().empty() ) << "one node is the combined mesh already";

    const auto [data, nodes] = project.Import( { "TuftA", "TuftB" } );
    const std::vector<std::string> short1{ "TuftA" };
    EXPECT_FALSE( Editor::SplitStaticMeshByNode( data, short1 ).IsSuccess() );
}

// THM1j: a split import writes NO combined mesh (UE imports none with Combine Meshes off); its record names the
// node meshes and its freshness is theirs. Combine Meshes on writes the combined mesh and no node list.
TEST( NodeMeshSplit, ASplitImportWritesNoCombinedMesh )
{
    const GrassProject project;
    const auto [data, nodes] = project.Import( { "TuftA", "TuftB", "TuftC" } );
    const fs::path material  = Editor::MaterialAdoption::MaterialAssetPath( project.Source, "GrassAtlas" );
    fs::create_directories( material.parent_path() );
    std::ofstream( material ) << "{}";

    auto split = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source, {} );
    ASSERT_TRUE( split.IsSuccess() ) << split.GetError();
    ASSERT_EQ( split.GetValue().size(), 3u );
    EXPECT_FALSE( Editor::ImportedMeshAssetIsFresh( project.Source ) ) << "a split import wrote the combined mesh";
    EXPECT_FALSE( fs::exists( Editor::CookPaths::MeshAsset( project.Source ) ) );
    const auto record = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    const auto& recordData = record.GetValue();
    if ( !recordData.has_value() || !recordData->Nodes.has_value() )
        FAIL() << "a split import recorded no node list";
    EXPECT_EQ( recordData->Nodes->size(), 3u );
    EXPECT_TRUE( Editor::ImportedMeshAssetIsCurrent( project.Source ) ) << "the node meshes are the import";

    fs::remove( split.GetValue()[1].second );
    EXPECT_FALSE( Editor::ImportedMeshAssetIsCurrent( project.Source ) ) << "a deleted node mesh re-imports";

    // Combine Meshes on: the one combined mesh, the node list cleared.
    Assets::SourceImportSettings on;
    on.CombineMeshes = true;
    auto combined    = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source, on );
    ASSERT_TRUE( combined.IsSuccess() ) << combined.GetError();
    EXPECT_TRUE( combined.GetValue().empty() );
    EXPECT_TRUE( Editor::ImportedMeshAssetIsFresh( project.Source ) );
    const auto after = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( after.IsSuccess() ) << after.GetError();
    const auto& afterData = after.GetValue();
    if ( !afterData.has_value() )
        FAIL() << "the combined import left no record";
    EXPECT_FALSE( afterData->Nodes.has_value() );
}

// THE ORBIT'S ONE HOME IS THE IMPORT RECORD (THM1l-c2; UE: UStaticMesh::ThumbnailInfo in the package): each mesh
// the import writes reads its own entry, the entry survives a re-import that rewrites every node mesh, and a
// stated default is refused so one picture has one spelling.
TEST( NodeMeshSplit, EachImportedMeshReadsItsOrbitFromTheRecord )
{
    const GrassProject project;
    const auto [data, nodes] = project.Import( { "TuftA", "TuftB", "TuftC" } );
    const fs::path material  = Editor::MaterialAdoption::MaterialAssetPath( project.Source, "GrassAtlas" );
    fs::create_directories( material.parent_path() );
    std::ofstream( material ) << "{}";
    auto split = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source, {} );
    ASSERT_TRUE( split.IsSuccess() ) << split.GetError();
    ASSERT_EQ( split.GetValue().size(), 3u );
    const fs::path tuftB = split.GetValue()[1].second;

    const auto before = Editor::MeshThumbnailOrbit( tuftB );
    ASSERT_TRUE( before.IsSuccess() ) << before.GetError();
    EXPECT_EQ( before.GetValue(), Assets::ThumbnailOrbit{} ) << "no entry = the default orbit";

    const auto record = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    const auto& recordData = record.GetValue();
    if ( !recordData.has_value() )
        FAIL() << "the import left no record";
    Ser::ImportRecordData        stated = *recordData;
    const Assets::ThumbnailOrbit yawed{ 10.0f, 180.0f, 0.25f };
    stated.Thumbnail = std::map<std::string, Assets::ThumbnailOrbitRecord>{
         { tuftB.filename().string(), Assets::ToRecord( yawed ) },
         { project.Source.filename().string(),
           Assets::ThumbnailOrbitRecord{ .Pitch = std::nullopt, .Yaw = 90.0f, .Zoom = std::nullopt } } };
    const fs::path recordPath = Common::Content::ImportRecordPathFor( project.Source );
    const auto     written    = Ser::WriteImportRecord( stated, Common::Content::ContentKind::StaticMesh );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    std::ofstream( recordPath, std::ios::binary | std::ios::trunc ) << written.GetValue();

    const auto node = Editor::MeshThumbnailOrbit( tuftB );
    ASSERT_TRUE( node.IsSuccess() ) << node.GetError();
    EXPECT_EQ( node.GetValue(), yawed ) << "the node mesh reads its own key";
    const auto other = Editor::MeshThumbnailOrbit( split.GetValue()[0].second );
    ASSERT_TRUE( other.IsSuccess() ) << other.GetError();
    EXPECT_EQ( other.GetValue(), Assets::ThumbnailOrbit{} ) << "a sibling node does not read TuftB's orbit";
    const auto combined = Editor::MeshThumbnailOrbit( project.Source );
    ASSERT_TRUE( combined.IsSuccess() ) << combined.GetError();
    EXPECT_EQ( combined.GetValue().Yaw, 90.0f ) << "the combined mesh is keyed by the source's own name";

    // A re-import rewrites every node .stmesh; the record, and so the orbit, is kept.
    auto again = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source, {} );
    ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
    const auto kept = Editor::MeshThumbnailOrbit( tuftB );
    ASSERT_TRUE( kept.IsSuccess() ) << kept.GetError();
    EXPECT_EQ( kept.GetValue(), yawed ) << "a re-import lost the orbit";

    // A stated default is refused by name.
    stated.Thumbnail->at( tuftB.filename().string() ) =
         Assets::ThumbnailOrbitRecord{ .Pitch = 0.0f, .Yaw = std::nullopt, .Zoom = std::nullopt };
    const auto restated = Ser::WriteImportRecord( stated, Common::Content::ContentKind::StaticMesh );
    ASSERT_TRUE( restated.IsSuccess() ) << restated.GetError();
    const auto refused = Ser::ParseImportRecord( restated.GetValue() );
    ASSERT_FALSE( refused.IsSuccess() );
    EXPECT_NE( refused.GetError().find( "default" ), std::string::npos ) << refused.GetError();
}

// EDIT THUMBNAIL WRITES THE RECORD AND THE PICTURE GOES STALE (THM1l-c3): SetMeshThumbnailOrbit is the one writer,
// MeshThumbnailFreshness the hash a mesh picture is judged against; the default orbit removes the entry and
// gives the bytes' own hash back.
TEST( NodeMeshSplit, AnEditedOrbitIsWrittenToTheRecordAndStalesThePicture )
{
    const GrassProject project;
    const auto [data, nodes] = project.Import( { "TuftA", "TuftB", "TuftC" } );
    const fs::path material  = Editor::MaterialAdoption::MaterialAssetPath( project.Source, "GrassAtlas" );
    fs::create_directories( material.parent_path() );
    std::ofstream( material ) << "{}";
    auto split = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source, {} );
    ASSERT_TRUE( split.IsSuccess() ) << split.GetError();
    const fs::path tuftB = split.GetValue()[1].second;

    const auto untouched = Editor::MeshThumbnailFreshness( tuftB );
    ASSERT_TRUE( untouched.has_value() );

    const Assets::ThumbnailOrbit yawed{ 10.0f, 135.0f, 0.5f };
    const auto                   written = Editor::SetMeshThumbnailOrbit( tuftB, yawed );
    ASSERT_TRUE( written.IsSuccess() ) << written.GetError();
    const auto read = Editor::MeshThumbnailOrbit( tuftB );
    ASSERT_TRUE( read.IsSuccess() ) << read.GetError();
    EXPECT_EQ( read.GetValue(), yawed );
    const auto edited = Editor::MeshThumbnailFreshness( tuftB );
    ASSERT_TRUE( edited.has_value() );
    EXPECT_NE( edited, untouched ) << "an edit of the orbit left the picture fresh";

    const auto reset = Editor::SetMeshThumbnailOrbit( tuftB, Assets::ThumbnailOrbit{} );
    ASSERT_TRUE( reset.IsSuccess() ) << reset.GetError();
    const auto record = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( record.IsSuccess() ) << record.GetError();
    const auto& recordData = record.GetValue();
    if ( !recordData.has_value() )
        FAIL() << "the reset left no record";
    EXPECT_FALSE( recordData->Thumbnail.has_value() ) << "the default is written as no key";
    EXPECT_EQ( Editor::MeshThumbnailFreshness( tuftB ), untouched );

    EXPECT_FALSE(
         Editor::SetMeshThumbnailOrbit( tuftB, Assets::ThumbnailOrbit{ 0.0f, 0.0f, -1.0f } ).IsSuccess() );
}

// EDIT THUMBNAIL'S WRITER FOR A MATERIAL (THM1l-c3): the .demat's Thumbnail replaced in place, the default info
// written as no key.
TEST( MaterialThumbnailEdit, SetMaterialFileThumbnailWritesTheInfoAndTheDefaultAsNoKey )
{
    const auto dir = std::filesystem::temp_directory_path() / "desert_material_thumbnail_edit";
    std::filesystem::create_directories( dir );
    const auto file = dir / "edit.demat";
    ASSERT_TRUE( Desert::Assets::WriteMaterialFile( file, Desert::Assets::MaterialData{} ) );

    Desert::Assets::ThumbnailInfo info;
    info.Primitive = Desert::Assets::ThumbnailPrimitive::Cylinder;
    info.Orbit     = Desert::Assets::ThumbnailOrbit{ 20.0f, -45.0f, 0.5f };
    const auto set = Desert::Assets::SetMaterialFileThumbnail( file, info );
    ASSERT_TRUE( set ) << set.GetError();
    const auto readBack = [&file]
    {
        std::ifstream     in( file, std::ios::binary );
        const std::string text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
        return Desert::Assets::ParseMaterialJson( file.generic_string(), text );
    };
    auto back = readBack();
    ASSERT_TRUE( back ) << back.GetError();
    EXPECT_EQ( back.GetValue().ThumbnailOrDefault(), info );

    ASSERT_TRUE( Desert::Assets::SetMaterialFileThumbnail( file, Desert::Assets::ThumbnailInfo{} ) );
    back = readBack();
    ASSERT_TRUE( back ) << back.GetError();
    EXPECT_FALSE( back.GetValue().Thumbnail.has_value() );
    std::filesystem::remove_all( dir );
}

// THM1l: the Details' Import Settings + Reimport. Changing an option and importing again changes what the import
// writes - Combine Meshes the NUMBER of meshes, Uniform Scale / Up Axis / LOD the options each mesh is built with
// - and the record keeps the options for the next re-import.
TEST( NodeMeshSplit, ReimportWithChangedSettingsChangesTheMeshes )
{
    const GrassProject project;
    const auto [data, nodes] = project.Import( { "TuftA", "TuftB", "TuftC" } );
    const fs::path material  = Editor::MaterialAdoption::MaterialAssetPath( project.Source, "GrassAtlas" );
    fs::create_directories( material.parent_path() );
    std::ofstream( material ) << "{}";

    Assets::SourceImportSettings split;
    split.Mesh.UniformScale = 2.0f;
    split.Mesh.UpAxis       = Assets::MeshSourceUpAxis::Z;
    auto first              = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source, split );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    ASSERT_EQ( first.GetValue().size(), 3u ) << "Combine Meshes off: one static mesh per node";
    const auto nodeMesh = Assets::ReadMeshSourceAssetFile( first.GetValue().front().second );
    ASSERT_TRUE( nodeMesh.IsSuccess() ) << nodeMesh.GetError();
    EXPECT_EQ( nodeMesh.GetValue().Import.Settings, split.Mesh ) << "the node mesh is built with the options";

    // The Details panel's edit: Combine Meshes on, then Reimport with the record's options.
    auto stored = Ser::ReadImportRecordSettings( project.Source );
    ASSERT_TRUE( stored.IsSuccess() && stored.GetValue() == split ) << "the record keeps the options";
    Assets::SourceImportSettings combine = stored.GetValue();
    combine.CombineMeshes                = true;
    combine.Mesh.LodPolicy               = Assets::MeshLodPolicy::None;
    auto second = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source, combine );
    ASSERT_TRUE( second.IsSuccess() ) << second.GetError();
    EXPECT_TRUE( second.GetValue().empty() ) << "Combine Meshes on: the one combined mesh, no node meshes";
    EXPECT_TRUE( Editor::ImportedMeshAssetIsFresh( project.Source ) );
    const auto after = Ser::ReadImportRecordSettings( project.Source );
    ASSERT_TRUE( after.IsSuccess() && after.GetValue() == combine );

    // Back off again: the same bytes, three meshes - the options, not the file, decided the count.
    auto third = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source, split );
    ASSERT_TRUE( third.IsSuccess() ) << third.GetError();
    EXPECT_EQ( third.GetValue().size(), 3u );
}

// THM1l-b5: a SKINNED import is recorded like a static one (UE: every import leaves its AssetImportData). The
// Import Options window's skinned import used to write no record, so the next drop found the file "new" and
// offered the window forever. RecordImport is the one writer: the options are the record's, the source is no
// longer new, and a file with no mesh (a skeleton and its clips) is recorded without a box.
TEST( NodeMeshSplit, ASkinnedImportIsRecordedWithItsOptions )
{
    const GrassProject project;
    Ser::MeshAssetData skinned;
    skinned.IsSkinned = true;
    Ser::SkinnedVertexData v{};
    v.Position = { 0.0f, 10.0f, 20.0f };
    skinned.SkinnedVertices.push_back( v );
    Ser::SubmeshData sub{};
    sub.VertexCount = 1;
    sub.Transform   = glm::mat4( 1.0f );
    sub.BoundingBox = { { 0.0f, 10.0f, 20.0f }, { 0.0f, 10.0f, 20.0f } };
    skinned.Submeshes.push_back( sub );

    const fs::path record = Common::Content::ImportRecordPathFor( project.Source );
    ASSERT_FALSE( fs::exists( record ) ) << "the fixture's source starts new";

    Assets::SourceImportSettings chosen;
    chosen.Mesh.UniformScale = 100.0f;
    chosen.Mesh.UpAxis       = Assets::MeshSourceUpAxis::Z;
    const auto recorded =
         Editor::RecordImport( project.Source, Common::Content::ContentKind::SkinnedMesh, &skinned, chosen );
    ASSERT_TRUE( recorded.IsSuccess() ) << recorded.GetError();
    EXPECT_TRUE( fs::is_regular_file( record ) )
         << "a recorded source is not new: the window is not offered again";
    const auto stored = Ser::ReadImportRecordSettings( project.Source );
    ASSERT_TRUE( stored.IsSuccess() ) << stored.GetError();
    EXPECT_EQ( stored.GetValue(), chosen ) << "Reimport reads the options the window confirmed";
    const auto whole = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( whole.IsSuccess() ) << whole.GetError();
    const auto& wholeData = whole.GetValue();
    if ( !wholeData.has_value() || !wholeData->Bounds.has_value() )
        FAIL() << "the recorded import has no box";
    // The box the placed mesh has: (x, y, z) -> (x, z, -y) for Z up, times 100.
    EXPECT_NEAR( wholeData->Bounds->Min[1], 2000.0f, 1e-2f )
         << "the box is the placed mesh's, the options applied";
    EXPECT_NEAR( wholeData->Bounds->Min[2], -1000.0f, 1e-2f );
    if ( !wholeData->Header.has_value() )
        FAIL() << "the record has no header";
    EXPECT_EQ( wholeData->Header->Kind, "SkinnedMesh" ) << "a skinned source's record says what it imports as";
    const auto kind = Ser::ReadImportRecordKind( project.Source );
    ASSERT_TRUE( kind.IsSuccess() ) << kind.GetError();
    EXPECT_EQ( kind.GetValue(), Common::Content::ContentKind::SkinnedMesh )
         << "the Details' Import Settings show the skeletal mesh's fields for it";
    const auto guid = Ser::ReadImportRecordGuid( project.Source );
    ASSERT_TRUE( guid.IsSuccess() ) << guid.GetError();

    // A second import (Reimport with other options) keeps the identity; one with no mesh keeps the box.
    const Assets::SourceImportSettings again{};
    ASSERT_TRUE( Editor::RecordImport( project.Source, Common::Content::ContentKind::SkinnedMesh, nullptr, again )
                      .IsSuccess() );
    const auto after = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( after.IsSuccess() ) << after.GetError();
    const auto& afterData = after.GetValue();
    if ( !afterData.has_value() || !afterData->Bounds.has_value() )
        FAIL() << "a re-import with no mesh dropped the box";
    const auto againStored = Ser::ReadImportRecordSettings( project.Source );
    ASSERT_TRUE( againStored.IsSuccess() ) << againStored.GetError();
    EXPECT_EQ( againStored.GetValue(), again );
    const auto sameGuid = Ser::ReadImportRecordGuid( project.Source );
    ASSERT_TRUE( sameGuid.IsSuccess() ) << sameGuid.GetError();
    EXPECT_EQ( sameGuid.GetValue(), guid.GetValue() );
}
