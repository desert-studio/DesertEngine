// AF4d: the importer's output for a static mesh is the MeshSourceAsset envelope beside its source.

#include <Common/Core/Constants.hpp>
#include <Common/Utilities/FileSystem.hpp>
#include <Editor/Import/CookPaths.hpp>
#include <Editor/Import/ImportedMeshAsset.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>

#include <gtest/gtest.h>

#include <algorithm>
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
    ASSERT_EQ( source.GetValue().Source.Models.size(), 3u );
    EXPECT_EQ( source.GetValue().Source.Models[0].Mesh.MaterialIds.size(), 64u ); // Body + Lid
    EXPECT_EQ( source.GetValue().Source.Models[1].Mesh.MaterialIds.size(), 8u );
    EXPECT_EQ( source.GetValue().Source.Models[2].Mesh.MaterialIds.size(), 2u );
    ASSERT_EQ( source.GetValue().Source.MaterialSlots.size(), 2u );
    EXPECT_EQ( source.GetValue().Source.MaterialSlots[0].Name, "Body" );
    EXPECT_EQ( source.GetValue().Source.MaterialSlots[1].Material, b );
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

    // Skipping debris is not a licence to write an empty source: a LOD whose every face is degenerate is
    // refused by name.
    Ser::MeshAssetData degenerate;
    AddPlane( degenerate, "Flat", 1, 0.0f, {} );
    for ( Ser::IndexData& face : degenerate.Indices )
        face.V3 = face.V1;
    const auto refusedWeld = Editor::MeshSourceFromImport( degenerate, {}, "Flat" );
    ASSERT_FALSE( refusedWeld.IsSuccess() );
    EXPECT_NE( refusedWeld.GetError().find( "Flat" ), std::string::npos ) << refusedWeld.GetError();
    EXPECT_NE( refusedWeld.GetError().find( "2 degenerate" ), std::string::npos ) << refusedWeld.GetError();
}

namespace
{
    // A 2 x 2-quad plane (faces 0..7) plus the three shapes an FBX export leaves behind: a degenerate face
    // (face 8), a duplicate of face 0 (face 9) and a fin on the plane's shared edge 1-3 (face 10), which
    // EditMesh cannot share and stands on its own corner copies. Face k carries polygroup 100 + k.
    Ser::MeshAssetData PlaneWithExportDebris()
    {
        Ser::MeshAssetData data;
        AddPlane( data, "Fins", 2, 0.0f, {} );
        data.StaticVertices.push_back(
             { { 10.0f, 20.0f, 10.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, {} } );
        data.Indices.push_back( { 0, 0, 1 } );
        data.Indices.push_back( data.Indices[0] );
        data.Indices.push_back( { 1, 3, 9 } );
        data.Submeshes[0].VertexCount = static_cast<uint32_t>( data.StaticVertices.size() );
        data.Submeshes[0].IndexCount  = static_cast<uint32_t>( data.Indices.size() * 3 );
        for ( size_t k = 0; k < data.Indices.size(); ++k )
            data.PolyGroups.push_back( 100 + static_cast<int32_t>( k ) );
        return data;
    }
} // namespace

TEST( ImportedMeshAsset, ExportDebrisIsSkippedAndTheRestKeepsItsGroups )
{
    // The editor's regression: base.fbx (204 degenerate, 4 duplicate, 8 detached of 120000) was refused whole,
    // so the Starter scene drew no mesh.
    const auto source = Editor::MeshSourceFromImport( PlaneWithExportDebris(), {}, "Fins" );
    ASSERT_TRUE( source.IsSuccess() ) << source.GetError();
    EXPECT_EQ( source.GetValue().DroppedDegenerate, 1 );
    EXPECT_EQ( source.GetValue().DroppedDuplicate, 1 );
    EXPECT_EQ( source.GetValue().DetachedTriangles, 1 );
    ASSERT_EQ( source.GetValue().Source.Models.size(), 1u );

    // Every surviving face keeps ITS group: 8 plane faces and the fin; the dropped faces' groups (108, 109)
    // are gone rather than shifted onto the fin.
    const Geometry::EditMeshSer& mesh = source.GetValue().Source.Models[0].Mesh;
    ASSERT_EQ( mesh.Triangles.size(), 9u * 3u );
    std::vector<int> groups = mesh.PolyGroups;
    std::ranges::sort( groups );
    EXPECT_EQ( groups, ( std::vector<int>{ 100, 101, 102, 103, 104, 105, 106, 107, 110 } ) );

    // The fin stands on its own corners: it shares no vertex with the plane (a component of its own).
    const auto finAt = static_cast<size_t>( std::ranges::find( mesh.PolyGroups, 110 ) - mesh.PolyGroups.begin() );
    for ( size_t t = 0; t < mesh.PolyGroups.size(); ++t )
        for ( int j = 0; j < 3 && t != finAt; ++j )
            for ( int i = 0; i < 3; ++i )
                EXPECT_NE( mesh.Triangles[finAt * 3 + i], mesh.Triangles[t * 3 + j] ) << "triangle " << t;

    // ONE warning line, naming the file and every count.
    const std::string warning = Editor::SkippedFacesWarning( source.GetValue(), "Meshes/base.fbx" );
    EXPECT_NE( warning.find( "'Meshes/base.fbx'" ), std::string::npos ) << warning;
    EXPECT_NE( warning.find( "1 degenerate" ), std::string::npos ) << warning;
    EXPECT_NE( warning.find( "1 duplicate" ), std::string::npos ) << warning;
    EXPECT_NE( warning.find( "detached 1" ), std::string::npos ) << warning;
    EXPECT_EQ( warning.find( '\n' ), std::string::npos ) << warning;

    const auto clean = Editor::MeshSourceFromImport( []
                                                     {
                                                         Ser::MeshAssetData d;
                                                         AddPlane( d, "Clean", 2, 0.0f, {} );
                                                         return d;
                                                     }(),
                                                     {}, "Clean" );
    ASSERT_TRUE( clean.IsSuccess() ) << clean.GetError();
    EXPECT_TRUE( Editor::SkippedFacesWarning( clean.GetValue(), "Clean" ).empty() );
}
