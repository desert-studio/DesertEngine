// AF4c: the mesh deriver - the DDC key is (SRCE hash, build settings, builder version) and nothing else, and a
// second load of the same asset is a hit, not a rebuild.
#include <Common/Core/Constants.hpp>
#include <Editor/Import/LODFold.hpp>
#include <Editor/Import/MeshDeriver.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Assets/Serialization/MeshBinary.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using namespace Desert;

namespace
{
    // An n x n-quad plane in XZ, 40 cm wide at any n. twoSlots: the first half of the triangles draws slot 0,
    // the rest slot 1.
    Geometry::EditMeshSer MakePlane( const uint32_t kN, const bool twoSlots )
    {
        Geometry::RenderMeshData render;
        for ( uint32_t z = 0; z <= kN; ++z )
            for ( uint32_t x = 0; x <= kN; ++x )
                render.Vertices.push_back( { { static_cast<float>( x ) * 40.0f / static_cast<float>( kN ), 0.0f,
                                               static_cast<float>( z ) * 40.0f / static_cast<float>( kN ) },
                                             { 0.0f, 1.0f, 0.0f },
                                             { 1.0f, 0.0f, 0.0f },
                                             { 0.0f, 0.0f, 1.0f },
                                             { static_cast<float>( x ) / static_cast<float>( kN ),
                                               static_cast<float>( z ) / static_cast<float>( kN ) } } );
        for ( uint32_t z = 0; z < kN; ++z )
            for ( uint32_t x = 0; x < kN; ++x )
            {
                const uint32_t i = z * ( kN + 1 ) + x;
                render.Indices.push_back( { i, i + kN + 1, i + 1 } );
                render.Indices.push_back( { i + 1, i + kN + 1, i + kN + 2 } );
            }
        auto imported = Geometry::FromRenderMesh( render );
        if ( !imported.IsSuccess() )
        {
            ADD_FAILURE() << imported.GetError();
            return {};
        }
        Geometry::EditMeshSer mesh = Geometry::ToSerialized( imported.GetValue().Mesh );
        if ( twoSlots )
            for ( size_t t = mesh.MaterialIds.size() / 2; t < mesh.MaterialIds.size(); ++t )
                mesh.MaterialIds[t] = 1;
        return mesh;
    }

    // A 4x4-quad plane: 32 triangles, enough for the LOD simplifier to act (it skips < 8).
    Assets::MeshSourceAsset MakePlaneAsset()
    {

        Assets::MeshSourceAsset asset;
        asset.Guid                 = Common::Content::AssetGuid::Generate();
        asset.Name                 = "Plane";
        asset.Import.SourceFile    = "Meshes/Plane.fbx";
        asset.Import.SourceHash    = 0x1234;
        asset.Source.Models        = { { MakePlane( 4, false ) } };
        asset.Source.MaterialSlots = { { "Default", Common::Content::AssetGuid::Generate() } };
        return asset;
    }
} // namespace

TEST( MeshDerivedData, KeyIsStableAndBlindToNamePathAndGuid )
{
    const Assets::MeshSourceAsset a = MakePlaneAsset();
    Assets::MeshSourceAsset       b = a;
    b.Name                          = "Renamed";
    b.Guid                          = Common::Content::AssetGuid::Generate();
    b.Import.SourceFile             = "Elsewhere/Other.fbx";
    EXPECT_EQ( Assets::MeshAssetDerivedDataKey( a ), Assets::MeshAssetDerivedDataKey( a ) );
    EXPECT_EQ( Assets::MeshAssetDerivedDataKey( a ), Assets::MeshAssetDerivedDataKey( b ) )
         << "the file name, the asset name and the GUID are not inputs of the render data";
}

TEST( MeshDerivedData, EverySettingAndTheSourceAreKeyInputs )
{
    const Assets::MeshSourceAsset a    = MakePlaneAsset();
    const uint64_t                base = Assets::MeshAssetDerivedDataKey( a );

    Assets::MeshSourceAsset lod   = a;
    lod.Import.Settings.LodPolicy = Assets::MeshLodPolicy::None;
    EXPECT_NE( base, Assets::MeshAssetDerivedDataKey( lod ) ) << "LOD policy";

    Assets::MeshSourceAsset scale      = a;
    scale.Import.Settings.UniformScale = 2.0f;
    EXPECT_NE( base, Assets::MeshAssetDerivedDataKey( scale ) ) << "uniform scale";

    Assets::MeshSourceAsset axis = a;
    axis.Import.Settings.UpAxis  = Assets::MeshSourceUpAxis::Z;
    EXPECT_NE( base, Assets::MeshAssetDerivedDataKey( axis ) ) << "up axis";

    Assets::MeshSourceAsset moved = a;
    moved.Source.Models[0].Mesh.Positions[0] += 1.0f;
    EXPECT_NE( base, Assets::MeshAssetDerivedDataKey( moved ) ) << "a source vertex";

    const uint64_t            hash = Assets::MeshSourceHash( a.Source );
    Assets::MeshBuildSettings bumped{ a.Import.Settings };
    bumped.BuilderVersion = Assets::kMeshBuilderVersion + 1;
    EXPECT_EQ( base, Assets::MeshDerivedDataKey( hash, Assets::MeshBuildSettings{ a.Import.Settings } ) );
    EXPECT_NE( base, Assets::MeshDerivedDataKey( hash, bumped ) ) << "the builder version";
}

TEST( MeshDerivedData, SecondLoadIsAHitAndAGameWithoutABuilderSaysWhy )
{
    const fs::path  project = fs::temp_directory_path() / "desert_mesh_ddc";
    std::error_code ec;
    fs::remove_all( project, ec );
    fs::create_directories( project, ec );
    Common::Constants::Path::SetProjectRoot( project, "Assets" );

    const Assets::MeshSourceAsset asset = MakePlaneAsset();
    const fs::path                file  = project / "Plane.stmesh";
    const auto                    wrote = Assets::WriteMeshSourceAssetFile( file, asset );
    if ( !wrote.IsSuccess() )
    {
        ADD_FAILURE() << wrote.GetError();
        return;
    }

    int builds = 0;
    Assets::SetMeshPlatformDataBuilder(
         [&builds]( const Assets::MeshSourceAsset& a )
         {
             ++builds;
             return Editor::BuildMeshPlatformData( a );
         } );
    const auto first = Assets::LoadMeshPlatformData( file );
    if ( !first.IsSuccess() )
    {
        ADD_FAILURE() << first.GetError();
        return;
    }
    EXPECT_EQ( builds, 1 ) << "a cold DDC builds once";
    EXPECT_FALSE( first.GetValue().empty() );

    const auto second = Assets::LoadMeshPlatformData( file );
    EXPECT_EQ( builds, 1 ) << "the second load must be a DDC hit";
    EXPECT_TRUE( second.IsSuccess() && second.GetValue() == first.GetValue() );

    const fs::path renamed = project / "Moved" / "Renamed.stmesh";
    fs::create_directories( renamed.parent_path(), ec );
    fs::rename( file, renamed, ec );
    EXPECT_TRUE( Assets::LoadMeshPlatformData( renamed ).IsSuccess() );
    EXPECT_EQ( builds, 1 ) << "renaming the asset hits the same entry";

    // No LOD chain -> a different entry, and a smaller one.
    Assets::MeshSourceAsset flat   = asset;
    flat.Import.Settings.LodPolicy = Assets::MeshLodPolicy::None;
    const auto flatBytes           = Editor::BuildMeshPlatformData( flat );
    EXPECT_TRUE( flatBytes.IsSuccess() && flatBytes.GetValue().size() < first.GetValue().size() );

    // A packaged game: no builder, so a miss is an error naming the entry's bucket.
    Assets::SetMeshPlatformDataBuilder( nullptr );
    EXPECT_TRUE( Assets::WriteMeshSourceAssetFile( file, flat ).IsSuccess() );
    const auto miss = Assets::LoadMeshPlatformData( file );
    EXPECT_FALSE( miss.IsSuccess() );
    if ( !miss.IsSuccess() )
        EXPECT_NE( miss.GetError().find( "StaticMesh" ), std::string::npos ) << miss.GetError();

    Common::Constants::Path::ResetToSandbox();
    fs::remove_all( project, ec );
}

TEST( MeshDerivedData, SkinnedSourceIsRefusedByName )
{
    Assets::MeshSourceAsset asset = MakePlaneAsset();
    asset.Source.Skin             = Assets::MeshSkin{};
    const auto built              = Editor::BuildMeshPlatformData( asset );
    EXPECT_FALSE( built.IsSuccess() );
}

namespace
{
    // LOD0 = 4x4 quads, LOD1 = 2x2, LOD2 = 1x1; two slots, so each LOD folds per section.
    Assets::MeshSourceAsset MakeThreeLodAsset()
    {
        Assets::MeshSourceAsset asset = MakePlaneAsset();
        asset.Source.Models = { { MakePlane( 4, true ) }, { MakePlane( 2, true ) }, { MakePlane( 1, true ) } };
        asset.Source.MaterialSlots.push_back( { "Trim", Common::Content::AssetGuid::Generate() } );
        return asset;
    }

    std::optional<Assets::Serialization::MeshAssetData> Build( const Assets::MeshSourceAsset& asset )
    {
        const auto bytes = Editor::BuildMeshPlatformData( asset );
        if ( !bytes.IsSuccess() )
        {
            ADD_FAILURE() << bytes.GetError();
            return std::nullopt;
        }
        auto data = Assets::Serialization::DecodeMeshBinary( bytes.GetValue(), asset.Name );
        if ( !data.IsSuccess() )
        {
            ADD_FAILURE() << data.GetError();
            return std::nullopt;
        }
        return data.ExtractValue();
    }
} // namespace

TEST( MeshDerivedData, EveryLodModelIsAKeyInput )
{
    const Assets::MeshSourceAsset a    = MakeThreeLodAsset();
    const uint64_t                base = Assets::MeshAssetDerivedDataKey( a );
    for ( size_t k = 0; k < a.Source.Models.size(); ++k )
    {
        Assets::MeshSourceAsset moved = a;
        moved.Source.Models[k].Mesh.Positions[0] += 1.0f;
        EXPECT_NE( base, Assets::MeshAssetDerivedDataKey( moved ) ) << "LOD" << k;
    }
}

// The builder's fold against the importer's: the same three meshes, imported the old way as one file with
// "<section>_LOD<k>" siblings and folded by FoldExternalLODMeshes, give the same render form byte for byte.
TEST( MeshDerivedData, ThreeModelsFoldLikeTheImporterFold )
{
    namespace Ser                       = Assets::Serialization;
    const Assets::MeshSourceAsset asset = MakeThreeLodAsset();
    const auto                    built = Build( asset );
    if ( !built )
    {
        ADD_FAILURE();
        return;
    }
    ASSERT_EQ( built->Submeshes.size(), 2u );
    for ( const Ser::SubmeshData& section : built->Submeshes )
    {
        ASSERT_EQ( section.LODs.size(), 2u ) << section.Name << ": two authored levels, none simplified";
        EXPECT_EQ( section.LODs[0].size(), 4u ) << section.Name;
        EXPECT_EQ( section.LODs[1].size(), 1u ) << section.Name;
    }
    EXPECT_EQ( built->PolyGroups.size(), built->Indices.size() );

    std::vector<Ser::MeshAssetData> parts;
    for ( const Assets::MeshSourceModel& model : asset.Source.Models )
    {
        Assets::MeshSourceAsset one   = asset;
        one.Source.Models             = { model };
        one.Import.Settings.LodPolicy = Assets::MeshLodPolicy::None;
        auto part                     = Build( one );
        if ( !part )
        {
            ADD_FAILURE();
            return;
        }
        parts.push_back( std::move( *part ) );
    }
    Ser::MeshAssetData old = parts[0];
    for ( size_t k = 1; k < parts.size(); ++k )
        for ( Ser::SubmeshData sub : parts[k].Submeshes )
        {
            for ( const Ser::SubmeshData& base : parts[0].Submeshes )
                if ( base.MaterialGuid == sub.MaterialGuid )
                    sub.Name = base.Name + "_LOD" + std::to_string( k );
            const auto firstVertex = static_cast<uint32_t>( old.StaticVertices.size() );
            old.StaticVertices.insert( old.StaticVertices.end(),
                                       parts[k].StaticVertices.begin() + sub.VertexOffset,
                                       parts[k].StaticVertices.begin() + sub.VertexOffset + sub.VertexCount );
            const uint32_t firstFace = sub.IndexOffset / 3;
            sub.IndexOffset          = static_cast<uint32_t>( old.Indices.size() * 3 );
            old.Indices.insert( old.Indices.end(), parts[k].Indices.begin() + firstFace,
                                parts[k].Indices.begin() + firstFace + sub.IndexCount / 3 );
            sub.VertexOffset = firstVertex;
            old.Submeshes.push_back( std::move( sub ) );
        }
    EXPECT_EQ( Editor::FoldExternalLODMeshes( old ), 4 );
    old.PolyGroups = parts[0].PolyGroups; // the importer's fold leaves them alone; only LOD0's faces remain
    EXPECT_EQ( Ser::EncodeMeshBinary( *built ), Ser::EncodeMeshBinary( old ) );
}

TEST( MeshDerivedData, LodPolicyNoneKeepsOnlyLodZero )
{
    Assets::MeshSourceAsset asset   = MakeThreeLodAsset();
    asset.Import.Settings.LodPolicy = Assets::MeshLodPolicy::None;
    const auto built                = Build( asset );
    if ( !built )
    {
        ADD_FAILURE();
        return;
    }
    for ( const Assets::Serialization::SubmeshData& section : built->Submeshes )
        EXPECT_TRUE( section.LODs.empty() ) << section.Name;
}

TEST( MeshDerivedData, LodSectionsMustLineUpWithLodZero )
{
    Assets::MeshSourceAsset asset = MakeThreeLodAsset();
    asset.Source.Models[0].Mesh   = MakePlane( 4, false ); // LOD0 draws slot 0 only; LOD1/2 draw slot 1 too
    const auto built              = Editor::BuildMeshPlatformData( asset );
    ASSERT_FALSE( built.IsSuccess() );
    EXPECT_NE( built.GetError().find( "LOD1 draws material slot 1" ), std::string::npos ) << built.GetError();

    Assets::MeshSourceAsset gap = MakeThreeLodAsset();
    gap.Source.Models[1].Mesh   = MakePlane( 2, false ); // slot 1 skips LOD1 and comes back at LOD2
    const auto gapped           = Editor::BuildMeshPlatformData( gap );
    ASSERT_FALSE( gapped.IsSuccess() );
    EXPECT_NE( gapped.GetError().find( "no faces in LOD1" ), std::string::npos ) << gapped.GetError();
}

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
