#include <Engine/Assets/MaterialFormat.hpp>
#include <iterator>
// THM1i: one static mesh per source node (UE's Combine Meshes OFF), Combine Meshes ON keeps the one mesh.
//
// The assimp half (which node placed which submesh, ImportResult::SubmeshNodes) is not linkable here; the
// fixture stands in for what the importer hands over for a three-node glTF - three tufts, their node world
// transforms already baked, in a row along x - and the source file on disk is that glTF's text.

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Editor/Import/MaterialAdoption.hpp>
#include <Editor/Import/NodeMeshSplit.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <gtest/gtest.h>

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

        // The importer's hand-over for @p order (node names, left to right in the file).
        std::pair<Ser::MeshAssetData, std::vector<std::string>>
        Import( const std::vector<std::string>& order ) const
        {
            Ser::MeshAssetData       data;
            std::vector<std::string> nodes;
            for ( const std::string& node : order )
            {
                const float x = node == "TuftA" ? 0.0f : node == "TuftB" ? 280.0f : 560.0f;
                AddTuft( data, node + "_Mesh", x, Material );
                nodes.push_back( node );
            }
            return { data, nodes };
        }

        // name -> GUID of every node mesh one import of @p order writes.
        std::map<std::string, Common::Content::AssetGuid>
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
        ASSERT_TRUE( box.has_value() );
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
    ASSERT_TRUE( Ser::EnsureImportRecord( project.Source, *box ).IsSuccess() );
    const auto combineDefault = Ser::ReadImportRecordCombineMeshes( project.Source );
    ASSERT_TRUE( combineDefault.IsSuccess() ) << combineDefault.GetError();
    ASSERT_FALSE( combineDefault.GetValue() ) << "UE's default is off";

    const fs::path record = Common::Content::ImportRecordPathFor( project.Source );
    const auto     text   = Common::Utils::FileSystem::ReadFileContent( record );
    ASSERT_TRUE( text.IsSuccess() );
    auto parsed = Ser::ParseImportRecord( text.GetValue() );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    Ser::ImportRecordData data2 = parsed.GetValue();
    data2.CombineMeshes         = true;
    std::ofstream( record, std::ios::binary | std::ios::trunc ) << Ser::WriteImportRecord( data2 );
    {
        const auto combine = Ser::ReadImportRecordCombineMeshes( project.Source );
        ASSERT_TRUE( combine.IsSuccess() && combine.GetValue() );
    }

    // The option survives a re-import that rewrites the record's box.
    ASSERT_TRUE( Ser::EnsureImportRecord( project.Source, { { 0, 0, 0 }, { 1, 1, 1 } } ).IsSuccess() );
    {
        const auto combine = Ser::ReadImportRecordCombineMeshes( project.Source );
        ASSERT_TRUE( combine.IsSuccess() && combine.GetValue() );
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

    auto split = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source );
    ASSERT_TRUE( split.IsSuccess() ) << split.GetError();
    ASSERT_EQ( split.GetValue().size(), 3u );
    EXPECT_FALSE( Editor::ImportedMeshAssetIsFresh( project.Source ) ) << "a split import wrote the combined mesh";
    EXPECT_FALSE( fs::exists( Editor::CookPaths::MeshAsset( project.Source ) ) );
    const auto record = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( record.IsSuccess() && record.GetValue() && record.GetValue()->Nodes );
    EXPECT_EQ( record.GetValue()->Nodes->size(), 3u );
    EXPECT_TRUE( Editor::ImportedMeshAssetIsCurrent( project.Source ) ) << "the node meshes are the import";

    fs::remove( split.GetValue()[1].second );
    EXPECT_FALSE( Editor::ImportedMeshAssetIsCurrent( project.Source ) ) << "a deleted node mesh re-imports";

    // Combine Meshes on: the one combined mesh, the node list cleared.
    Ser::ImportRecordData on = *record.GetValue();
    on.CombineMeshes         = true;
    std::ofstream( Common::Content::ImportRecordPathFor( project.Source ), std::ios::binary | std::ios::trunc )
         << Ser::WriteImportRecord( on );
    auto combined = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source );
    ASSERT_TRUE( combined.IsSuccess() ) << combined.GetError();
    EXPECT_TRUE( combined.GetValue().empty() );
    EXPECT_TRUE( Editor::ImportedMeshAssetIsFresh( project.Source ) );
    const auto after = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( after.IsSuccess() && after.GetValue() );
    EXPECT_FALSE( after.GetValue()->Nodes.has_value() );
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
    auto split = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source );
    ASSERT_TRUE( split.IsSuccess() ) << split.GetError();
    ASSERT_EQ( split.GetValue().size(), 3u );
    const fs::path tuftB = split.GetValue()[1].second;

    const auto before = Editor::MeshThumbnailOrbit( tuftB );
    ASSERT_TRUE( before.IsSuccess() ) << before.GetError();
    EXPECT_EQ( before.GetValue(), Assets::ThumbnailOrbit{} ) << "no entry = the default orbit";

    const auto record = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( record.IsSuccess() && record.GetValue() );
    Ser::ImportRecordData        stated = *record.GetValue();
    const Assets::ThumbnailOrbit yawed{ 10.0f, 180.0f, 0.25f };
    stated.Thumbnail = std::map<std::string, Assets::ThumbnailOrbitRecord>{
         { tuftB.filename().string(), Assets::ToRecord( yawed ) },
         { project.Source.filename().string(), Assets::ThumbnailOrbitRecord{ .Yaw = 90.0f } } };
    const fs::path recordPath = Common::Content::ImportRecordPathFor( project.Source );
    std::ofstream( recordPath, std::ios::binary | std::ios::trunc ) << Ser::WriteImportRecord( stated );

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
    auto again = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source );
    ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
    const auto kept = Editor::MeshThumbnailOrbit( tuftB );
    ASSERT_TRUE( kept.IsSuccess() ) << kept.GetError();
    EXPECT_EQ( kept.GetValue(), yawed ) << "a re-import lost the orbit";

    // A stated default is refused by name.
    stated.Thumbnail->at( tuftB.filename().string() ) = Assets::ThumbnailOrbitRecord{ .Pitch = 0.0f };
    const auto refused                                = Ser::ParseImportRecord( Ser::WriteImportRecord( stated ) );
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
    auto split = Editor::WriteStaticMeshImport( data, nodes, project.Named, project.Source );
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
    EXPECT_NE( *edited, *untouched ) << "an edit of the orbit left the picture fresh";

    const auto reset = Editor::SetMeshThumbnailOrbit( tuftB, Assets::ThumbnailOrbit{} );
    ASSERT_TRUE( reset.IsSuccess() ) << reset.GetError();
    const auto record = Ser::ReadImportRecord( project.Source );
    ASSERT_TRUE( record.IsSuccess() && record.GetValue() );
    EXPECT_FALSE( record.GetValue()->Thumbnail.has_value() ) << "the default is written as no key";
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
