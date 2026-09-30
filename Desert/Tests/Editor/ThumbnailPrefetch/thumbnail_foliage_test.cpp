#include <Editor/Widgets/ThumbnailFoliage.hpp>

#include <Engine/Assets/Serialization/FoliageType.hpp>

#include <gtest/gtest.h>

// THM1n-5 (b): a foliage type is photographed as the mesh its `.defoliage` names (UE: UFoliageType's
// thumbnail is its mesh), so the resolution from the file to that mesh is the whole of its producer.
namespace
{
    namespace Ser = Desert::Assets::Serialization;
    namespace Tf  = Desert::Editor::ThumbnailFoliage;

    const std::filesystem::path kRoot = "/content/Assets";

    std::string MockType( Ser::FoliageTypeKind kind, const std::string& meshGuid, const std::string& meshPath )
    {
        Ser::FoliageTypeData data;
        data.Kind      = kind;
        data.Mesh.Guid = meshGuid;
        data.Mesh.Path = meshPath;
        return Ser::WriteFoliageType( data );
    }
} // namespace

TEST( ThumbnailFoliage, AMeshTypeResolvesToTheCookedMeshItNamesUnderTheAssetsRoot )
{
    const std::string text =
         MockType( Ser::FoliageTypeKind::Mesh, "8f3c2a615b7d4e19a0c42d6e9f1b3a57", "Meshes/Props/grass.stmesh" );
    const auto source = Tf::MeshSourceOf( text, kRoot );
    ASSERT_TRUE( source.IsSuccess() ) << source.GetError();
    EXPECT_EQ( source.GetValue(), kRoot / "Meshes/Props/grass.stmesh" );
}

TEST( ThumbnailFoliage, ATypeWithNoMeshYetIsRefusedNotPhotographedEmpty )
{
    const auto source = Tf::MeshSourceOf( MockType( Ser::FoliageTypeKind::Mesh, "", "" ), kRoot );
    ASSERT_FALSE( source.IsSuccess() );
    EXPECT_NE( source.GetError().find( "names no mesh" ), std::string::npos ) << source.GetError();
}

TEST( ThumbnailFoliage, TextThatDoesNotParseIsRefusedWithTheParsersReason )
{
    const auto source = Tf::MeshSourceOf( "{ not a foliage type", kRoot );
    ASSERT_FALSE( source.IsSuccess() );
    EXPECT_NE( source.GetError().find( "does not parse" ), std::string::npos ) << source.GetError();
}
