// AF4c: the mesh deriver - the DDC key is (SRCE hash, build settings, builder version) and nothing else, and a
// second load of the same asset is a hit, not a rebuild.
#include <Common/Core/Constants.hpp>
#include <Editor/Import/MeshDeriver.hpp>
#include <Engine/Assets/MeshDerivedData.hpp>
#include <Engine/Geometry/EditMeshConversion.hpp>
#include <Engine/Geometry/EditMeshSerialization.hpp>

#include <gtest/gtest.h>

#include <filesystem>

namespace fs = std::filesystem;
using namespace Desert;

namespace
{
    // A 4x4-quad plane in XZ: 32 triangles, enough for the LOD simplifier to act (it skips < 8).
    Assets::MeshSourceAsset MakePlaneAsset()
    {
        Geometry::RenderMeshData render;
        constexpr uint32_t       kN = 4;
        for ( uint32_t z = 0; z <= kN; ++z )
            for ( uint32_t x = 0; x <= kN; ++x )
                render.Vertices.push_back(
                     { { static_cast<float>( x ) * 10.0f, 0.0f, static_cast<float>( z ) * 10.0f },
                       { 0.0f, 1.0f, 0.0f },
                       { 1.0f, 0.0f, 0.0f },
                       { 0.0f, 0.0f, 1.0f },
                       { static_cast<float>( x ) / kN, static_cast<float>( z ) / kN } } );
        for ( uint32_t z = 0; z < kN; ++z )
            for ( uint32_t x = 0; x < kN; ++x )
            {
                const uint32_t i = z * ( kN + 1 ) + x;
                render.Indices.push_back( { i, i + kN + 1, i + 1 } );
                render.Indices.push_back( { i + 1, i + kN + 1, i + kN + 2 } );
            }
        auto imported = Geometry::FromRenderMesh( render );
        EXPECT_TRUE( imported.IsSuccess() ) << ( imported.IsSuccess() ? "" : imported.GetError() );

        Assets::MeshSourceAsset asset;
        asset.Guid                 = Common::Content::AssetGuid::Generate();
        asset.Name                 = "Plane";
        asset.Import.SourceFile    = "Meshes/Plane.fbx";
        asset.Import.SourceHash    = 0x1234;
        asset.Source.Mesh          = Geometry::ToSerialized( imported.GetValue().Mesh );
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
    moved.Source.Mesh.Positions[0] += 1.0f;
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

int main( int argc, char** argv )
{
    ::testing::InitGoogleTest( &argc, argv );
    return RUN_ALL_TESTS();
}
