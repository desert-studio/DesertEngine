// P9b: a modeling edit of an IMPORTED mesh (UE: the tool commits into the UStaticMesh, the .fbx stays, a
// re-import overwrites the edit). The mesh a tool edits comes from the same rule the editor's tools use
// (GetToolTargetMeshAt over the asset path a MeshHandle resolves to), and the edit is committed through the
// same writer the editor's Accept calls.
#include <Common/Content/ContentScan.hpp>
#include <Common/Content/ImportRecord.hpp>
#include <Common/Core/Constants.hpp>
#include <Editor/Core/Selection/ModelingToolTarget.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/EditedMeshAsset.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Editor/Import/MeshDeriver.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>

#include <gtest/gtest.h>

#include <fstream>

namespace fs = std::filesystem;
using namespace Desert;
namespace Ser = Assets::Serialization;

namespace
{
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

    // An imported Rock.fbx in a throwaway project: its record, its DDC envelope, no file at Rock.stmesh.
    struct Imported
    {
        fs::path Source;
        fs::path Asset;
        explicit Imported( const char* name )
        {
            const fs::path  root = fs::temp_directory_path() / ( std::string( "desert_p9b_" ) + name );
            std::error_code ec;
            fs::remove_all( root, ec );
            fs::create_directories( root, ec );
            Common::Constants::Path::SetProjectRoot( root, "Assets" );
            Source = Common::Constants::Path::ASSETS_PATH / "Meshes" / "Rock.fbx";
            fs::create_directories( Source.parent_path(), ec );
            std::ofstream( Source, std::ios::binary | std::ios::trunc ) << "rock v1";
            Asset = Editor::CookPaths::MeshAsset( Source );
            Assets::SetMeshPlatformDataBuilder( []( const Assets::MeshSourceAsset& a )
                                                { return Editor::BuildMeshPlatformData( a ); } );
        }
        ~Imported()
        {
            Assets::SetMeshPlatformDataBuilder( nullptr );
            Common::Constants::Path::ResetToSandbox();
        }
        Imported( const Imported& )            = delete;
        Imported& operator=( const Imported& ) = delete;
    };

    double HighestY( const Geometry::DynamicMesh3& mesh )
    {
        double y = -1e30;
        for ( int v = 0; v < mesh.MaxVertexID(); ++v )
            if ( mesh.IsVertex( v ) )
                y = std::max( y, mesh.GetVertex( v ).y );
        return y;
    }

    // The tool's own edit stand-in: vertex 0 lifted 25 cm (any operation's result is a new DynamicMesh3).
    std::shared_ptr<const Geometry::DynamicMesh3> Edit( const Geometry::DynamicMesh3& before )
    {
        auto after = std::make_shared<Geometry::DynamicMesh3>( before );
        after->SetVertex( 0, before.GetVertex( 0 ) + glm::dvec3( 0.0, 25.0, 0.0 ) );
        return after;
    }

    std::optional<Common::Content::AssetGuid> RowGuid( const fs::path& asset )
    {
        const auto scanned = Common::Content::ScanContentRoots();
        const auto it      = scanned.find( Common::AssetHandle::StableKeyForPath( asset ) );
        if ( it == scanned.end() )
            return std::nullopt;
        auto row = Common::Content::RegistryRowFor( it->first, it->second );
        return row && row.GetValue().Guid ? row.GetValue().Guid : std::nullopt;
    }
} // namespace

TEST( EditedImport, TheToolTargetsAnImportedMeshThatHasNoFileOfItsOwn )
{
    Imported rock( "target" );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, rock.Source ).IsSuccess() );
    ASSERT_FALSE( fs::exists( rock.Asset ) ) << "AF4h: an import writes nothing at the asset path";
    // The live refusal was "static mesh <...>/Rock.stmesh: No such file or directory".
    const auto target = Editor::GetToolTargetMeshAt( nullptr, rock.Asset );
    ASSERT_TRUE( target.IsSuccess() ) << target.GetError();
    EXPECT_EQ( target.GetValue().Mesh->TriangleCount(), 2 );
}

TEST( EditedImport, AnEditIsSavedIntoTheAssetUnderTheSameGuidAndReadBackAfterAReload )
{
    Imported rock( "edit" );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, rock.Source ).IsSuccess() );
    const auto guid = Ser::ReadImportRecordGuid( rock.Source );
    ASSERT_TRUE( guid.IsSuccess() ) << guid.GetError();
    ASSERT_EQ( RowGuid( rock.Asset ), guid.GetValue() );

    const auto before = Editor::GetToolTargetMeshAt( nullptr, rock.Asset );
    ASSERT_TRUE( before.IsSuccess() ) << before.GetError();
    const auto edited = Edit( *before.GetValue().Mesh );
    const auto wrote  = Editor::WriteEditedMeshAsset( rock.Asset, *edited );
    ASSERT_TRUE( wrote.IsSuccess() ) << wrote.GetError();

    // The file at the asset path IS the asset now, same identity as the record (and so as every reference).
    ASSERT_TRUE( fs::is_regular_file( rock.Asset ) );
    EXPECT_TRUE( Assets::IsEditedImportedMesh( rock.Asset ) );
    const auto source = Assets::LoadMeshSourceAsset( rock.Asset );
    ASSERT_TRUE( source.IsSuccess() ) << source.GetError();
    EXPECT_EQ( source.GetValue().Guid, guid.GetValue() );
    EXPECT_EQ( source.GetValue().Import.SourceFile, Common::AssetHandle::StableKeyForPath( rock.Source ) )
         << "the edited asset still names the file it was imported from";
    EXPECT_EQ( RowGuid( rock.Asset ), guid.GetValue() ) << "the registry row a scene resolves its GUID through";
    std::ifstream fbx( rock.Source, std::ios::binary );
    EXPECT_EQ( std::string( std::istreambuf_iterator<char>( fbx ), {} ), "rock v1" ) << "the source is untouched";

    // A reload - a fresh lift of the asset path, what the next launch does - draws the edit.
    const auto after = Editor::GetToolTargetMeshAt( nullptr, rock.Asset );
    ASSERT_TRUE( after.IsSuccess() ) << after.GetError();
    EXPECT_NEAR( HighestY( *after.GetValue().Mesh ), 25.0, 1e-3 );
    EXPECT_NEAR( HighestY( *before.GetValue().Mesh ), 0.0, 1e-3 );

    // And the boot scan does not re-import over it: the edit keeps the source "cooked".
    EXPECT_TRUE( Editor::StaticMeshCookAvailable( rock.Asset, rock.Source ) );
}

TEST( EditedImport, AReimportOverwritesTheEditAndSaysSo )
{
    Imported rock( "reimport" );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, rock.Source ).IsSuccess() );
    const auto before = Editor::GetToolTargetMeshAt( nullptr, rock.Asset );
    ASSERT_TRUE( before.IsSuccess() ) << before.GetError();
    ASSERT_TRUE( Editor::WriteEditedMeshAsset( rock.Asset, *Edit( *before.GetValue().Mesh ) ).IsSuccess() );
    ASSERT_TRUE( Assets::IsEditedImportedMesh( rock.Asset ) );

    // The warning's own answer: the edited file it removed.
    const auto discarded = Editor::RemoveBesideSourceFile( rock.Source );
    ASSERT_TRUE( discarded.has_value() );
    EXPECT_EQ( *discarded, rock.Asset );
    // Put the edit back and re-import through the importer's one write site.
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, rock.Source ).IsSuccess() );
    ASSERT_TRUE( Editor::WriteEditedMeshAsset( rock.Asset, *Edit( *before.GetValue().Mesh ) ).IsSuccess() );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, rock.Source ).IsSuccess() );
    EXPECT_FALSE( fs::exists( rock.Asset ) ) << "the re-import replaced the edit";

    const auto again = Editor::GetToolTargetMeshAt( nullptr, rock.Asset );
    ASSERT_TRUE( again.IsSuccess() ) << again.GetError();
    EXPECT_NEAR( HighestY( *again.GetValue().Mesh ), 0.0, 1e-3 ) << "the import's geometry, not the edit";
    const auto guid = Ser::ReadImportRecordGuid( rock.Source );
    ASSERT_TRUE( guid.IsSuccess() );
    EXPECT_EQ( RowGuid( rock.Asset ), guid.GetValue() ) << "identity survives the re-import";
}

TEST( EditedImport, AFileWithAnotherGuidBesideTheSourceIsNeverReadInTheEnvelopesPlace )
{
    Imported rock( "stale" );
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( Quad(), {}, rock.Source ).IsSuccess() );
    const auto before = Editor::GetToolTargetMeshAt( nullptr, rock.Asset );
    ASSERT_TRUE( before.IsSuccess() ) << before.GetError();
    ASSERT_TRUE( Editor::WriteEditedMeshAsset( rock.Asset, *Edit( *before.GetValue().Mesh ) ).IsSuccess() );
    // A pre-AF4h leftover: the same bytes under the GUID that import minted, not the record's.
    auto stale = Assets::ReadMeshSourceAssetFile( rock.Asset );
    ASSERT_TRUE( stale.IsSuccess() );
    Assets::MeshSourceAsset leftover = stale.GetValue();
    leftover.Guid                    = Common::Content::AssetGuid::Generate();
    ASSERT_TRUE( Assets::WriteMeshSourceAssetFile( rock.Asset, leftover ).IsSuccess() );

    EXPECT_FALSE( Assets::IsEditedImportedMesh( rock.Asset ) );
    const auto loaded = Assets::LoadMeshSourceAsset( rock.Asset );
    ASSERT_TRUE( loaded.IsSuccess() ) << loaded.GetError();
    EXPECT_NE( loaded.GetValue().Guid, leftover.Guid ) << "the DDC envelope, not the stale file";
    EXPECT_FALSE( Editor::RemoveBesideSourceFile( rock.Source ).has_value() ) << "a leftover goes silently";
}
