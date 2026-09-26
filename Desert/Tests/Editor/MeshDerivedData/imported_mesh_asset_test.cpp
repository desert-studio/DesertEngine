// AF4d: the importer's output for a static mesh is the MeshSourceAsset envelope beside its source.

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>

namespace fs = std::filesystem;
using namespace Desert;
namespace Ser = Assets::Serialization;

namespace
{
    // Appends an n x n-quad plane at height y as one submesh (submesh-local indices, as the importer writes).
    void AddPlane( Ser::MeshAssetData& data, const std::string& name, const uint32_t kN, const float y,
                   const Common::Content::AssetGuid material )
    {
        Ser::SubmeshData sub{};
        sub.Name         = name;
        sub.VertexOffset = static_cast<uint32_t>( data.StaticVertices.size() );
        sub.IndexOffset  = static_cast<uint32_t>( data.Indices.size() * 3 );
        sub.Transform    = glm::mat4( 1.0f );
        sub.MaterialGuid = material;
        for ( uint32_t z = 0; z <= kN; ++z )
            for ( uint32_t x = 0; x <= kN; ++x )
            {
                const float u = static_cast<float>( x ) / static_cast<float>( kN );
                const float v = static_cast<float>( z ) / static_cast<float>( kN );
                data.StaticVertices.push_back( { { u * 40.0f, y, v * 40.0f },
                                                 { 0.0f, 1.0f, 0.0f },
                                                 { 1.0f, 0.0f, 0.0f },
                                                 { 0.0f, 0.0f, 1.0f },
                                                 { u, v } } );
            }
        for ( uint32_t z = 0; z < kN; ++z )
            for ( uint32_t x = 0; x < kN; ++x )
            {
                const uint32_t i = z * ( kN + 1 ) + x;
                data.Indices.push_back( { i, i + kN + 1, i + 1 } );
                data.Indices.push_back( { i + 1, i + kN + 1, i + kN + 2 } );
            }
        sub.VertexCount = static_cast<uint32_t>( data.StaticVertices.size() ) - sub.VertexOffset;
        sub.IndexCount  = static_cast<uint32_t>( data.Indices.size() * 3 ) - sub.IndexOffset;
        data.Submeshes.push_back( sub );
    }

    struct Project
    {
        fs::path Source;
        Project()
        {
            const fs::path  root = fs::temp_directory_path() / "desert_imported_mesh_asset";
            std::error_code ec;
            fs::remove_all( root, ec );
            fs::create_directories( root, ec );
            Common::Constants::Path::SetProjectRoot( root, "Assets" );
            Source = Common::Constants::Path::ASSETS_PATH / "Meshes" / "Grid.obj";
            fs::create_directories( Source.parent_path(), ec );
            Write( "o Grid\n" );
        }
        void Write( const std::string& bytes ) const
        {
            std::ofstream( Source, std::ios::binary | std::ios::trunc ) << bytes;
        }
    };
} // namespace

TEST( ImportedMeshAsset, ReimportOfTheSameBytesKeepsGuidKeyAndFile )
{
    const Project      project;
    const auto         material = Common::Content::AssetGuid::Generate();
    Ser::MeshAssetData imported;
    AddPlane( imported, "Grid", 4, 0.0f, material );
    const std::vector<Assets::MeshMaterialSlot> named{ { "Sand", material } };

    const auto first = Editor::WriteImportedMeshAsset( imported, named, project.Source );
    ASSERT_TRUE( first.IsSuccess() ) << first.GetError();
    EXPECT_EQ( first.GetValue(), Editor::MeshAssetWrite::Written );

    const fs::path file = Editor::CookPaths::MeshAsset( project.Source );
    EXPECT_EQ( file, Common::Constants::Path::ASSETS_PATH / "Meshes" / "Grid.stmesh" );
    const auto asset = Assets::ReadMeshSourceAssetFile( file );
    ASSERT_TRUE( asset.IsSuccess() ) << asset.GetError();
    EXPECT_EQ( asset.GetValue().Source.MaterialSlots.size(), 1u );
    EXPECT_EQ( asset.GetValue().Source.MaterialSlots[0].Name, "Sand" );
    EXPECT_EQ( asset.GetValue().Import.SourceFile, Common::AssetHandle::StableKeyForPath( project.Source ) );
    const auto bytesBefore = Common::Utils::FileSystem::ReadFileContent( file );
    const auto timeBefore  = fs::last_write_time( file );

    const auto second = Editor::WriteImportedMeshAsset( imported, named, project.Source );
    ASSERT_TRUE( second.IsSuccess() ) << second.GetError();
    EXPECT_EQ( second.GetValue(), Editor::MeshAssetWrite::Unchanged ) << "the asset file was rewritten";
    EXPECT_EQ( fs::last_write_time( file ), timeBefore );
    const auto again = Assets::ReadMeshSourceAssetFile( file );
    ASSERT_TRUE( again.IsSuccess() );
    EXPECT_EQ( again.GetValue().Guid, asset.GetValue().Guid );
    EXPECT_EQ( Assets::MeshAssetDerivedDataKey( again.GetValue() ),
               Assets::MeshAssetDerivedDataKey( asset.GetValue() ) );
    const auto bytesAfter = Common::Utils::FileSystem::ReadFileContent( file );
    EXPECT_EQ( bytesAfter.GetValue(), bytesBefore.GetValue() );

    // Nothing of a static import lands under Cooked/ any more.
    std::error_code ec;
    EXPECT_FALSE( fs::exists( Common::Constants::Path::MESH_PATH_COOKED / "Grid.stmesh", ec ) );
}

TEST( ImportedMeshAsset, FreshnessIsTheSourceHashNotItsTime )
{
    const Project      project;
    Ser::MeshAssetData imported;
    AddPlane( imported, "Grid", 2, 0.0f, Common::Content::AssetGuid::Generate() );
    EXPECT_FALSE( Editor::ImportedMeshAssetIsFresh( project.Source ) ) << "no asset yet";
    ASSERT_TRUE( Editor::WriteImportedMeshAsset( imported, {}, project.Source ).IsSuccess() );
    EXPECT_TRUE( Editor::ImportedMeshAssetIsFresh( project.Source ) );

    fs::last_write_time( project.Source, fs::file_time_type::clock::now() + std::chrono::hours( 1 ) );
    EXPECT_TRUE( Editor::ImportedMeshAssetIsFresh( project.Source ) ) << "a touch alone re-imported";

    project.Write( "o Grid\nv 0 0 0\n" );
    EXPECT_FALSE( Editor::ImportedMeshAssetIsFresh( project.Source ) ) << "changed bytes were not seen";

    // A re-import after the change keeps the GUID and rewrites the file.
    const fs::path file   = Editor::CookPaths::MeshAsset( project.Source );
    const auto     before = Assets::ReadMeshSourceAssetFile( file );
    ASSERT_TRUE( before.IsSuccess() ) << before.GetError();
    const auto re = Editor::WriteImportedMeshAsset( imported, {}, project.Source );
    ASSERT_TRUE( re.IsSuccess() ) << re.GetError();
    EXPECT_EQ( re.GetValue(), Editor::MeshAssetWrite::Written );
    const auto after = Assets::ReadMeshSourceAssetFile( file );
    ASSERT_TRUE( after.IsSuccess() ) << after.GetError();
    EXPECT_EQ( after.GetValue().Guid, before.GetValue().Guid );
}

TEST( ImportedMeshAsset, LodSiblingsBecomeSourceModels )
{
    const auto         a = Common::Content::AssetGuid::Generate();
    const auto         b = Common::Content::AssetGuid::Generate();
    Ser::MeshAssetData imported;
    AddPlane( imported, "Body", 4, 0.0f, a );
    AddPlane( imported, "Body_LOD2", 1, 0.0f, a );
    AddPlane( imported, "Lid", 4, 50.0f, b );
    AddPlane( imported, "Body_lod1", 2, 0.0f, a );
    const auto source = Editor::MeshSourceFromImport( imported, {}, "Crate" );
    ASSERT_TRUE( source.IsSuccess() ) << source.GetError();
    ASSERT_EQ( source.GetValue().Models.size(), 3u );
    EXPECT_EQ( source.GetValue().Models[0].Mesh.MaterialIds.size(), 64u ); // Body + Lid
    EXPECT_EQ( source.GetValue().Models[1].Mesh.MaterialIds.size(), 8u );
    EXPECT_EQ( source.GetValue().Models[2].Mesh.MaterialIds.size(), 2u );
    ASSERT_EQ( source.GetValue().MaterialSlots.size(), 2u );
    EXPECT_EQ( source.GetValue().MaterialSlots[0].Name, "Body" );
    EXPECT_EQ( source.GetValue().MaterialSlots[1].Material, b );
}

TEST( ImportedMeshAsset, WhatTheSourceCannotHoldIsRefusedByName )
{
    Ser::MeshAssetData morphs;
    AddPlane( morphs, "Face", 2, 0.0f, {} );
    morphs.MorphTargets.push_back( { "Smile", {}, {} } );
    const auto refusedMorphs = Editor::MeshSourceFromImport( morphs, {}, "Head" );
    ASSERT_FALSE( refusedMorphs.IsSuccess() );
    EXPECT_NE( refusedMorphs.GetError().find( "morph" ), std::string::npos ) << refusedMorphs.GetError();

    Ser::MeshAssetData gap;
    AddPlane( gap, "Rock", 2, 0.0f, {} );
    AddPlane( gap, "Rock_LOD2", 1, 0.0f, {} );
    const auto refusedGap = Editor::MeshSourceFromImport( gap, {}, "Rock" );
    ASSERT_FALSE( refusedGap.IsSuccess() );
    EXPECT_NE( refusedGap.GetError().find( "LOD1" ), std::string::npos ) << refusedGap.GetError();

    Ser::MeshAssetData degenerate;
    AddPlane( degenerate, "Flat", 1, 0.0f, {} );
    degenerate.Indices.push_back( degenerate.Indices[0] ); // a duplicate face does not weld one-to-one
    degenerate.Submeshes[0].IndexCount += 3;
    const auto refusedWeld = Editor::MeshSourceFromImport( degenerate, {}, "Flat" );
    ASSERT_FALSE( refusedWeld.IsSuccess() );
    EXPECT_NE( refusedWeld.GetError().find( "Flat" ), std::string::npos ) << refusedWeld.GetError();
}
