// FIX8: AN IMPORTED STATIC MESH'S IDENTITY IS ITS IMPORT RECORD (`<name>.<ext>.deimport`, UE: the .uasset's
// persistent GUID). Since AF4h nothing is written at `<stem>.stmesh`, so before the record the mesh had no GUID
// anywhere: the content registry could not describe it ("cannot open base.stmesh"), a foliage type could not
// name it, and a GUID minted per import died with every edit of the source. These tests hold the chain:
// the import writes the record once; a re-import of changed bytes keeps it; the registry row and the mesh's
// handle come from it; a source without a record has no identity and says which file is missing.

#include <Common/Content/ContentScan.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Editor/Import/MaterialAdoption.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <gtest/gtest.h>

#include <fstream>

namespace fs = std::filesystem;
using namespace Desert;
namespace Ser = Assets::Serialization;

namespace
{
    // One quad: enough for the importer's conversion to accept the source.
    Ser::MeshAssetData Quad()
    {
        Ser::MeshAssetData data;
        Ser::SubmeshData   sub{};
        sub.Name      = "Quad";
        sub.Transform = glm::mat4( 1.0f );
        for ( int i = 0; i < 4; ++i )
            data.StaticVertices.push_back(
                 { { static_cast<float>( i & 1 ) * 40.0f, 0.0f, static_cast<float>( i >> 1 ) * 40.0f },
                   { 0.0f, 1.0f, 0.0f },
                   { 1.0f, 0.0f, 0.0f },
                   { 0.0f, 0.0f, 1.0f },
                   { 0.0f, 0.0f } } );
        data.Indices.push_back( { 0u, 2u, 1u } );
        data.Indices.push_back( { 1u, 2u, 3u } );
        sub.VertexCount = 4;
        sub.IndexCount  = 6;
        data.Submeshes.push_back( sub );
        return data;
    }

    struct Project
    {
        fs::path Source;
        explicit Project( const char* name )
        {
            const fs::path  root = fs::temp_directory_path() / ( std::string( "desert_fix8_" ) + name );
            std::error_code ec;
            fs::remove_all( root, ec );
            fs::create_directories( root, ec );
            Common::Constants::Path::SetProjectRoot( root, "Assets" );
            Source = Common::Constants::Path::ASSETS_PATH / "Meshes" / "Rock.fbx";
            fs::create_directories( Source.parent_path(), ec );
            Write( "rock v1" );
        }
        void Write( const std::string& bytes ) const
        {
            std::ofstream( Source, std::ios::binary | std::ios::trunc ) << bytes;
        }
        [[nodiscard]] fs::path Asset() const
        {
            return Editor::CookPaths::MeshAsset( Source );
        }
    };

    // The registry's row for the mesh, from the same walk the editor's registry is built from.
    std::optional<Common::Utils::AssetRegistryEntry> RowOf( const fs::path& asset )
    {
        const auto scanned = Common::Content::ScanContentRoots();
        const auto it      = scanned.find( Common::AssetHandle::StableKeyForPath( asset ) );
        if ( it == scanned.end() )
            return std::nullopt;
        auto row = Common::Content::RegistryRowFor( it->first, it->second );
        return row ? std::optional( row.GetValue() ) : std::nullopt;
    }
} // namespace

TEST( ImportRecord, TheImportWritesTheRecordOnceAndAReimportOfChangedBytesKeepsIt )
{
    const Project project( "keep" );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, project.Source ).IsSuccess() );
    const fs::path record = Common::Content::ImportRecordPathFor( project.Source );
    ASSERT_TRUE( fs::is_regular_file( record ) ) << "the import wrote no record";
    EXPECT_EQ( record.filename(), "Rock.fbx.deimport" );
    const auto first     = Ser::ReadImportRecordGuid( project.Source );
    const auto textFirst = Common::Utils::FileSystem::ReadFileContent( record );
    ASSERT_TRUE( first && textFirst ) << ( first ? "" : first.GetError() );

    project.Write( "rock v2, edited in the DCC" );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, project.Source ).IsSuccess() );
    const auto again     = Ser::ReadImportRecordGuid( project.Source );
    const auto textAgain = Common::Utils::FileSystem::ReadFileContent( record );
    ASSERT_TRUE( again && textAgain );
    EXPECT_EQ( again.GetValue(), first.GetValue() ) << "a re-import gave the mesh a new identity";
    EXPECT_EQ( textAgain.GetValue(), textFirst.GetValue() ) << "a re-import rewrote the record";

    // The envelope in the DDC states the same identity (the loader's view of the mesh).
    const auto envelope = Assets::LoadMeshSourceAsset( project.Asset() );
    ASSERT_TRUE( envelope ) << envelope.GetError();
    EXPECT_EQ( envelope.GetValue().Guid, first.GetValue() );
}

TEST( ImportRecord, TheRegistryRowOfAMeshWithNoFileIsItsRecord )
{
    const Project project( "row" );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, project.Source ).IsSuccess() );
    ASSERT_FALSE( fs::exists( project.Asset() ) ) << "AF4h: nothing is written at the asset path";
    const auto guid = Ser::ReadImportRecordGuid( project.Source );
    ASSERT_TRUE( guid );

    // Described by path (ContentRegistry::Update after a cook) and found by the walk: both are the record.
    const auto described =
         Common::Content::DescribeContentFile( project.Asset(), Common::Content::ContentKind::StaticMesh );
    ASSERT_TRUE( described.Header.has_value() ) << described.HeaderError;
    EXPECT_EQ( described.Header->Guid, guid.GetValue() );

    const auto row = RowOf( project.Asset() );
    ASSERT_TRUE( row.has_value() ) << "the content walk made no row for the imported mesh";
    ASSERT_TRUE( row->Guid.has_value() );
    EXPECT_EQ( *row->Guid, guid.GetValue() );
    // The handle a GUID reference resolves to (a foliage type's GetMeshHandle, a scene's MeshGuid) IS the row's.
    const uint64_t byGuid = static_cast<uint64_t>( Common::Content::HandleForGuid( guid.GetValue() ) );
    EXPECT_EQ( row->EffectiveHandle(), byGuid );

    // ...and stays so across an edit of the source: the reference survives the re-import.
    project.Write( "rock v3" );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, project.Source ).IsSuccess() );
    const auto after = RowOf( project.Asset() );
    ASSERT_TRUE( after.has_value() );
    EXPECT_EQ( after->EffectiveHandle(), byGuid );
}

// DIMP 2: THE RECORD STATES THE IMPORTED MESH'S BOX, and the registry takes it from there - nothing is built and
// the DDC is not asked, so a machine that never imported the source (a cold DDC) still knows the box.
TEST( ImportRecord, TheRecordStatesTheImportedBoxAndTheRegistryReadsItWithoutTheDdc )
{
    const Project project( "box" );
    const auto    expected = Ser::MeshDataBounds( Quad() );
    ASSERT_TRUE( expected.has_value() );
    // A cold DDC: only the record exists - no envelope was ever cached for this source.
    ASSERT_TRUE( Ser::EnsureImportRecord( project.Source, *expected ) );
    ASSERT_FALSE( Editor::ImportedMeshAssetIsFresh( project.Source ) ) << "the DDC is not cold";

    const auto described =
         Common::Content::DescribeContentFile( project.Asset(), Common::Content::ContentKind::StaticMesh );
    ASSERT_TRUE( described.HeaderError.empty() ) << described.HeaderError;
    ASSERT_TRUE( described.HeaderBounds.has_value() && described.HeaderBounds->Stated );
    ASSERT_TRUE( described.HeaderBounds->Bounds.has_value() ) << "the record states no box";
    const Common::Math::AABB box = *described.HeaderBounds->Bounds;
    EXPECT_EQ( box.Min, expected->Min );
    EXPECT_EQ( box.Max, expected->Max );
    const auto row = RowOf( project.Asset() );
    ASSERT_TRUE( row.has_value() && row->Bounds.has_value() ) << "the registry row carries no box";
    EXPECT_EQ( row->Bounds->Max, expected->Max );
}

// No legacy: a version-1 record (no box) is refused, naming the record, and is re-imported - never read.
TEST( ImportRecord, AVersionOneRecordIsRefusedByItsPath )
{
    const Project  project( "v1" );
    const fs::path record = Common::Content::ImportRecordPathFor( project.Source );
    std::ofstream( record ) << R"({ "Header": { "Kind": "StaticMesh", "Guid": "56916479183d19ac580f5e8f62a520c4",
        "Versions": { "DIMP": 1 }, "Dependencies": [] }, "Source": "Rock.fbx" })";
    const auto guid = Ser::ReadImportRecordGuid( project.Source );
    ASSERT_FALSE( guid );
    EXPECT_NE( guid.GetError().find( record.string() ), std::string::npos ) << guid.GetError();
}

TEST( ImportRecord, ASourceWithoutARecordHasNoIdentityAndSaysWhichFileIsMissing )
{
    const Project project( "none" );
    const auto    guid = Ser::ReadImportRecordGuid( project.Source );
    ASSERT_FALSE( guid );
    EXPECT_NE( guid.GetError().find( "Rock.fbx.deimport" ), std::string::npos ) << guid.GetError();

    // No row, no header: never a GUID made up from the path.
    const auto described =
         Common::Content::DescribeContentFile( project.Asset(), Common::Content::ContentKind::StaticMesh );
    EXPECT_FALSE( described.Header.has_value() );
    EXPECT_FALSE( RowOf( project.Asset() ).has_value() );
    EXPECT_FALSE( Common::Content::ImportRecordStandingFor( project.Asset() ).has_value() );
}

TEST( ImportRecord, ARecordCopiedBesideAnotherSourceIsRefusedByName )
{
    const Project project( "copied" );
    ASSERT_TRUE( Ser::EnsureImportRecord( project.Source, *Ser::MeshDataBounds( Quad() ) ) );
    const fs::path other = project.Source.parent_path() / "Tree.fbx";
    std::ofstream( other ) << "tree";
    fs::copy_file( Common::Content::ImportRecordPathFor( project.Source ),
                   Common::Content::ImportRecordPathFor( other ) );
    const auto guid = Ser::ReadImportRecordGuid( other );
    ASSERT_FALSE( guid );
    EXPECT_NE( guid.GetError().find( "Rock.fbx" ), std::string::npos ) << guid.GetError();
}

// THM1a4: a fresh envelope does not make the import current while a material it names has no .demat. A deleted
// material used to stay deleted for good (every later run skipped the import that writes it), and the mesh drew
// the default material with nothing in the log.
TEST( ImportRecord, AFreshEnvelopeIsNotCurrentWhileAMaterialItNamesHasNoFile )
{
    const Project                    project( "materials_present" );
    const Common::Content::AssetGuid guid = Common::Content::AssetGuid::Generate();
    auto                             quad = Quad();
    quad.Submeshes[0].MaterialGuid        = guid;
    const std::vector<Assets::MeshMaterialSlot> named{ { "model", guid } };
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( quad, named, project.Source ).IsSuccess() );
    ASSERT_TRUE( Editor::ImportedMeshAssetIsFresh( project.Source ) ) << "the envelope was not cached";

    const fs::path material = Editor::MaterialAdoption::MaterialAssetPath( project.Source, "model" );
    EXPECT_FALSE( Editor::ImportedMaterialsPresent( project.Source ) ) << material.generic_string();
    EXPECT_FALSE( Editor::ImportedMeshAssetIsCurrent( project.Source ) )
         << "a fresh envelope skipped the import that writes " << material.generic_string();

    std::error_code ec;
    fs::create_directories( material.parent_path(), ec );
    std::ofstream( material, std::ios::binary ) << "{}";
    EXPECT_TRUE( Editor::ImportedMaterialsPresent( project.Source ) );
    EXPECT_TRUE( Editor::ImportedMeshAssetIsCurrent( project.Source ) );

    // Changed source bytes: the envelope is stale, so the import runs whatever the materials say.
    project.Write( "rock v2" );
    EXPECT_FALSE( Editor::ImportedMeshAssetIsCurrent( project.Source ) );
}
