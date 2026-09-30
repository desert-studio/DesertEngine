// SKEL-fixa/b: THE MIGRATOR STATES A COMMITTED SKINNED IMPORT'S RECORD (ImportRecordSourceHash.hpp). A `.skmesh`
// committed beside its raw source is that source's import; a record with no SourceHash made the editor re-import
// it at its first start and rewrite committed files. The step states the source's hash and - a SkinnedMesh record
// without a box is refused by its reader - the committed mesh's box. Run on the repository's own TwoJointProbe
// (the mesh and its glTF copied byte for byte into a scratch folder, no record beside them).
#include <ImportRecordSourceHash.hpp>

#include <Common/Content/ImportRecord.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/ImportRecord.hpp>
#include <Engine/Assets/Serialization/Mesh.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>

#include "../../TestSupport/scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    namespace Ser = Desert::Assets::Serialization;

    std::string ReadBytes( const std::filesystem::path& file )
    {
        std::ifstream in( file, std::ios::binary );
        return std::string{ std::istreambuf_iterator<char>( in ), std::istreambuf_iterator<char>() };
    }

    class ImportRecordSourceHash : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const std::filesystem::path corpus =
                 Desert::TestSupport::RepositoryRoot() / "Editor/Resources/Assets/Meshes";
            m_Gltf = ReadBytes( corpus / "TwoJointProbe.gltf" );
            m_Mesh = ReadBytes( corpus / "TwoJointProbe.skmesh" );
            ASSERT_FALSE( m_Gltf.empty() ) << "run from the tree root";
            ASSERT_FALSE( m_Mesh.empty() ) << "run from the tree root";
            std::error_code ec;
            std::filesystem::remove_all( m_Dir, ec );
            std::filesystem::create_directories( m_Dir );
            std::ofstream( m_Source, std::ios::binary ) << m_Gltf;
            std::ofstream( m_Skinned, std::ios::binary ) << m_Mesh;
        }
        void TearDown() override
        {
            std::error_code ec;
            std::filesystem::remove_all( m_Dir, ec );
        }

        std::filesystem::path m_Dir     = std::filesystem::temp_directory_path() / "ImportRecordSourceHash";
        std::filesystem::path m_Source  = m_Dir / "TwoJointProbe.gltf";
        std::filesystem::path m_Skinned = m_Dir / "TwoJointProbe.skmesh";
        std::string           m_Gltf;
        std::string           m_Mesh;
    };
} // namespace

// A source with no record gets a SkinnedMesh record stating the hash of its bytes and the committed mesh's box -
// a record the engine's reader accepts. Mutation: ImportRecordSourceHash.cpp without the `needsBox` block => the
// record states no Bounds, ParseImportRecord refuses it => red here.
TEST_F( ImportRecordSourceHash, ASourceWithoutARecordGetsItsHashAndTheMeshsBox )
{
    const auto stated = Desert::Migration::ImportRecordWithSourceHash( m_Source, m_Skinned );
    ASSERT_TRUE( stated.IsSuccess() ) << stated.GetError();
    const auto& record = stated.GetValue();
    if ( !record.has_value() )
        FAIL() << "a source with no record was left without one";

    const auto parsed = Ser::ParseImportRecord( *record );
    ASSERT_TRUE( parsed.IsSuccess() ) << parsed.GetError();
    const Ser::ImportRecordData& data = parsed.GetValue();
    const auto                   hash = Desert::Assets::HashMeshSourceFile( m_Source );
    ASSERT_TRUE( hash.IsSuccess() ) << hash.GetError();
    if ( !data.SourceHash.has_value() )
        FAIL() << "the record states no source hash";
    EXPECT_EQ( *data.SourceHash, hash.GetValue() );
    if ( !data.Header.has_value() )
        FAIL() << "the record states no header";
    EXPECT_EQ( data.Header->Kind, "SkinnedMesh" );
    EXPECT_EQ( data.Source, "TwoJointProbe.gltf" );

    const auto mesh = Ser::ReadMeshAssetData( m_Mesh, m_Skinned.string() );
    ASSERT_TRUE( mesh.IsSuccess() ) << mesh.GetError();
    const auto box = Ser::MeshDataBounds( mesh.GetValue() );
    if ( !box.has_value() )
        FAIL() << "the committed mesh states no box";
    if ( !data.Bounds.has_value() )
        FAIL() << "the record states no Bounds";
    for ( int axis = 0; axis < 3; ++axis )
    {
        EXPECT_EQ( data.Bounds->Min[axis], box->Min[axis] ) << "axis " << axis;
        EXPECT_EQ( data.Bounds->Max[axis], box->Max[axis] ) << "axis " << axis;
    }
}

// The step is idempotent: once the record states the hash and the box, a second run states nothing (the --check
// of a migrated corpus is clean). Mutation: the early `return Result{}` removed => a second run rewrites => red.
TEST_F( ImportRecordSourceHash, AStatedRecordIsLeftAsItIs )
{
    const auto first = Desert::Migration::ImportRecordWithSourceHash( m_Source, m_Skinned );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    const auto& firstRecord = first.GetValue();
    if ( !firstRecord.has_value() )
        FAIL() << "a source with no record was left without one";
    std::ofstream( Common::Content::ImportRecordPathFor( m_Source ), std::ios::binary ) << *firstRecord;

    const auto second = Desert::Migration::ImportRecordWithSourceHash( m_Source, m_Skinned );
    ASSERT_TRUE( second.IsSuccess() ) << second.GetError();
    EXPECT_FALSE( second.GetValue().has_value() ) << "a record that states its hash and box was rewritten";
}
